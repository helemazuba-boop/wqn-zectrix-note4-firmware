#!/usr/bin/env python3
"""Exercise production deck-scope recovery and write boundaries with NVS ports.

Actual NVS helpers, marker reader, parser, recoverable tail and public switch
are extracted verbatim. Host ports model staged NVS values, atomic per-key
commits and before/after faults, not physical flash or StorageService HIL.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_outbox_forensics import function


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'main/storage.cpp')
    parser.add_argument('--suite', choices=('all', 'preservation'), default='all')
    parser.add_argument('--mutation', choices=('truncate-generation', 'erase-invalid-marker',
                                               'accept-invalid-uuid', 'publish-unverified-cache',
                                               'skip-prior-tail', 'reopen-after-failure'))
    args = parser.parse_args()
    source = args.source.read_text()
    if args.mutation:
        replacements = {
            'truncate-generation': ('if (found && committed > UINT32_MAX) {', 'if (false) {'),
            'erase-invalid-marker': ('return ESP_ERR_INVALID_STATE;  // Preserve unknown marker bytes.',
                                    'return ClearNvsKeyRaw(kDeckScopeMarkerKey);'),
            'accept-invalid-uuid': ('if (!hyphen && !hex) return false;',
                                   'if (!hyphen && !hex && size != 36) return false;'),
            'publish-unverified-cache': ('DeckScopeText marker;',
                                        'g_deck_scope_generation.store(committed_generation); DeckScopeText marker;'),
            'skip-prior-tail': ('ESP_RETURN_ON_ERROR(RecoverDeckScopeTransaction(&recovery), kTag, "deck change: recover prior tail");',
                                '(void)recovery;'),
            'reopen-after-failure': ('if (result == ESP_OK) g_deck_scope_ready.store(true, std::memory_order_release);',
                                    'g_deck_scope_ready.store(true, std::memory_order_release);'),
        }
        old, new = replacements[args.mutation]
        if source.count(old) != 1:
            raise ValueError('scope mutation boundary drift: ' + args.mutation)
        source = source.replace(old, new)
    helpers = ['struct NvsHandle {', 'esp_err_t LoadU64FromNvs(',
               'esp_err_t SaveStringToNvsRaw(', 'esp_err_t SaveU64ToNvsRaw(',
               'esp_err_t ClearNvsKeyRaw(', 'esp_err_t LoadStringFromNvs(', 'struct U64WriteContext {',
               'esp_err_t SaveU64Transaction(', 'esp_err_t SaveWordSequentialCursor(']
    if 'struct DeckScopeText {' in source:
        helpers += ['bool IsDeckScopeId(', 'struct DeckScopeText {',
                    'esp_err_t LoadDeckScopeTextLocked(']
    has_ready = 'std::atomic<bool> g_deck_scope_ready' in source
    if has_ready:
        helpers += ['struct DeckScopeTailGuard {', 'struct DeckScopeRecoveryContext {',
                    'bool IsWordDeckScopeReady(', 'bool IsDeckScopeCleanupOnStorageOwner(']
    bodies = [function(source, signature) + (';' if signature.startswith('struct ') else '')
              for signature in helpers]
    bodies += [function(source, 'esp_err_t ApplyDeckScopeChangeLocked('),
               function(source, 'struct DeckScopeChangeContext {') + ';',
               function(source, 'esp_err_t RecoverDeckScopeTransaction('),
               function(source, 'esp_err_t DeckScopeChangeTransaction('),
               function(source, 'esp_err_t ChangeDefaultWordDeckForeground('),
               function(source, 'esp_err_t WordSessionScopeResetTransaction('),
               function(source, 'esp_err_t LoadDefaultWordDeckId(')]
    template = (root / 'scripts/testdata/word_deck_scope_tail.cpp').read_text()
    if template.count('@@PRODUCTION@@') != 1:
        raise ValueError('scope fixture boundary drift')
    unit = template.replace('@@PRODUCTION@@', '\n\n'.join(bodies)).replace(
        '@@SCOPE_STATE@@', 'std::atomic<bool> g_deck_scope_ready{false};\nbool g_deck_scope_tail_active=false;'
        if has_ready else '')
    with tempfile.TemporaryDirectory(prefix='wqn-scope-tail-') as directory:
        path = Path(directory)
        (path/'nvs.h').write_text('#pragma once\n')  # API ports precede the real typed-presence header.
        (path / 'test.cpp').write_text(unit)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                        '-DWQN_SCOPE_FULL_PORTS=' + str(int(args.suite == 'all')),
                        '-DWQN_SCOPE_HAS_READY=' + str(int(has_ready)),
                        '-I'+str(path),'-I'+str(root/'main'),
                        str(path / 'test.cpp'), '-o', str(path / 'test')],
                       check=True, timeout=45)
        return subprocess.run([str(path / 'test')], timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
