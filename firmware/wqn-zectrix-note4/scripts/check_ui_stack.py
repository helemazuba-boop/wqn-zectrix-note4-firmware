#!/usr/bin/env python3
"""Check actual Xtensa entry frames, not source-level sizeof estimates.

This is a build regression gate, NOT a whole-call-chain stack proof or HIL.
Run in the ESP-IDF environment: python3 scripts/check_ui_stack.py <candidate.elf>
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


def entry_frames(disassembly):
    frames, symbol = {}, None
    for line in disassembly.splitlines():
        header = re.match(r'^[0-9a-fA-F]+ <(.+)>:$', line)
        if header:
            symbol = header.group(1)
        entry = re.search(r'\bentry\s+a1,\s+(0x[0-9a-fA-F]+|\d+)\b', line)
        if entry and symbol is not None:
            frames[symbol] = max(frames.get(symbol, 0), int(entry.group(1), 0))
    return frames


def demangle_clones(frames, cxxfilt):
    # GCC 14 spells these as _ZZ..._clEv$constprop$0. objdump -C leaves
    # the entire name mangled. Strip only the compiler suffix, then use the
    # toolchain demangler; don't relax the required render-frame checks.
    clones = [name for name in frames if name.startswith('_Z') and '$' in name]
    if not clones:
        return frames
    output = subprocess.run([cxxfilt], input='\n'.join(name.split('$', 1)[0] for name in clones) + '\n',
                            capture_output=True, text=True, check=True, timeout=10).stdout.splitlines()
    if len(output) != len(clones):
        raise ValueError('demangler output count mismatch')
    normalized = dict(frames)
    for old, new in zip(clones, output):
        size = normalized.pop(old)
        normalized[new] = max(normalized.get(new, 0), size)
    return normalized


def check_frames(frames):
    checks = []
    selectors = [
        ('UI owner fixed frame', 2048, lambda name:
         'DeviceUiTask(void*)' in name and 'lambda' not in name),
        ('display submit fixed frame', 512, lambda name:
         name.startswith('device_ui_internal::RequestEpdUiRefresh(')),
        ('two out-of-line render frames', 6144, lambda name:
         'DeviceUiTask(void*)' in name and 'lambda' in name and 'operator()' in name),
    ]
    for label, limit, select in selectors:
        found = [(name, size) for name, size in frames.items() if select(name)]
        # Missing symbols/entry instructions must fail, never a default 0 B.
        required = 2 if label == 'two out-of-line render frames' else 1
        checks.append((label + ' present', len(found) >= required,
                       f'{len(found)} entry frames (need >= {required})'))
        for name, size in found:
            checks.append((label, size <= limit, f'{size} B <= {limit} B: {name}'))
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--objdump', default='xtensa-esp32s3-elf-objdump')
    parser.add_argument('--cxxfilt', default='xtensa-esp32s3-elf-c++filt')
    args = parser.parse_args()
    if not args.elf.is_file():
        parser.error(f'ELF does not exist: {args.elf}')
    objdump = shutil.which(args.objdump)
    if not objdump:
        parser.error(f'{args.objdump} unavailable; source ESP-IDF export.sh first')
    cxxfilt = shutil.which(args.cxxfilt)
    if not cxxfilt:
        parser.error(f'{args.cxxfilt} unavailable; source ESP-IDF export.sh first')
    output = subprocess.run([objdump, '-d', '-C', str(args.elf)], check=True,
                            capture_output=True, text=True, timeout=60).stdout
    checks = check_frames(demangle_clones(entry_frames(output), cxxfilt))
    for label, passed, detail in checks:
        print(f'[{"PASS" if passed else "FAIL"}] {label}: {detail}')
    print('Static entry frames only; deeper callees, ROM, interrupts and runtime HWM still need HIL.')
    return 0 if all(passed for _label, passed, _detail in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
