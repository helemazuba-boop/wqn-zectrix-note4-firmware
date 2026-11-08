#!/usr/bin/env python3
"""Read-only checkpoint material checks; native codec parity is tested separately.

These checks validate retained bytes and candidate-window relationships. They
cannot attest NVS pause state, committed scope generation, current packs, source
device identity, or publication durability. They never authorize installation.
"""
import re
import struct

import outbox_forensics as f

MAX_SAFE = 9007199254740991
MAX_PAYLOAD = {'word': 96 * 1024, 'note': 128 * 1024}
STEMS = ('wsq', 'wsr', 'wsd', 'wsv', 'wsi', 'wsh', 'wsm')
URL_SEED = re.compile(r'[A-Za-z0-9_-]{1,64}\Z')
HEX = re.compile(r'[0-9a-f]{64}\Z')


class DecodeError(ValueError):
    pass


class Reader:
    def __init__(self, raw):
        self.raw, self.at = raw, 0

    def scalar(self, kind):
        width = struct.calcsize('<' + kind)
        if self.at + width > len(self.raw):
            raise DecodeError('short payload')
        value = struct.unpack_from('<' + kind, self.raw, self.at)[0]
        self.at += width
        return value

    def text(self, limit):
        count = self.scalar('H')
        if count > limit or self.at + count > len(self.raw):
            raise DecodeError('string size')
        # Preserve octets including embedded NUL. Repair policy below is
        # stricter than native decoding; no lossy Unicode normalization.
        value = self.raw[self.at:self.at + count].decode('latin1')
        self.at += count
        return value

    def count(self, limit):
        count = self.scalar('H')
        if count > limit:
            raise DecodeError('collection size')
        return count


def decode_payload(raw, domain):
    r = Reader(raw)
    word = domain == 'word'
    value = {key: r.scalar('B') for key in
             (('active', 'paused', 'phase', 'mode', 'purpose', 'ordering', 'include', 'has_more')
              if word else ('active', 'paused', 'mode', 'ordering', 'include', 'has_more'))}
    value['position'] = r.scalar('I')
    if word:
        value['start_index'] = r.scalar('I')
    value['optional_count'] = r.scalar('I')
    value['next_sequence'] = r.scalar('Q')
    if word:
        value['scope_generation'] = r.scalar('I')
    else:
        value['progress_revision'] = r.scalar('Q')
    value['session_id'], value['seed'] = r.text(36), r.text(64)
    if not word:
        value['notebook_id'] = r.text(36)
    value['cursor'] = r.text(256)
    if word:
        value['deck_ids'] = [r.text(36) for _ in range(r.count(32))]
    snapshots = []
    for _ in range(r.count(32)):
        snapshots.append({'id': r.text(36), 'content_revision': r.scalar('Q'),
                          'pack_revision': r.scalar('Q'), 'sha256': r.text(64)})
    value['snapshot'] = snapshots
    items = []
    for _ in range(r.count(500)):
        item = {'item_id': r.text(36), 'scope_id': r.text(36), 'ordinal': r.scalar('Q')}
        if not word:
            item['last_opened_at'] = r.text(32)
        items.append(item)
    value['items'] = items
    if word:
        value['progress_revision'] = r.scalar('Q')
    if r.at != len(raw) or value['position'] > len(items) or value['optional_count'] > 500 or \
            any(value[k] > 1 for k in ('active', 'paused', 'include', 'has_more')) or \
            value['mode'] > (6 if word else 1) or value['ordering'] > (6 if word else 1) or \
            (word and (value['phase'] > 1 or value['purpose'] > 1)):
        raise DecodeError('payload semantics')
    return value


def decode_session(raw, domain):
    if len(raw) < 16:
        return {'native_error': 266}
    magic, version, _, size, crc = struct.unpack_from('<IHHII', raw)
    if magic != (0x53535157 if domain == 'word' else 0x53534E51) or \
            version != (4 if domain == 'word' else 1) or size > MAX_PAYLOAD[domain]:
        return {'native_error': 266}
    if size + 16 != len(raw) or f.journal_crc(raw[16:]) != crc:
        return {'native_error': 265}
    try:
        return {'native_error': 0, 'value': decode_payload(raw[16:], domain)}
    except DecodeError:
        return {'native_error': 264}


def descriptor_semantics(s, total):
    if not s['active'] or s['paused'] or s['position'] != 0 or s['phase'] != 0 or \
            s['mode'] != 0 or s['purpose'] != 0 or s['ordering'] != 0 or not s['include'] or \
            s['next_sequence'] != 0 or s['progress_revision'] != 0 or s['has_more'] or s['cursor'] or \
            not 1 <= s['optional_count'] <= 500 or not URL_SEED.fullmatch(s['seed']) or \
            not f.UUID.fullmatch(s['session_id']) or not s['items'] or \
            not s['start_index'] < total <= 10000 or len(s['items']) > total - s['start_index'] or \
            not s['deck_ids'] or len(s['deck_ids']) != len(s['snapshot']) or \
            len(set(s['deck_ids'])) != len(s['deck_ids']):
        return False
    for deck, snap in zip(s['deck_ids'], s['snapshot']):
        if not f.UUID.fullmatch(deck) or deck != snap['id'] or not HEX.fullmatch(snap['sha256']) or \
                not 0 < snap['pack_revision'] <= MAX_SAFE or not 0 <= snap['content_revision'] <= MAX_SAFE:
            return False
    return len({item['item_id'] for item in s['items']}) == len(s['items']) and all(
        item['ordinal'] == i and f.UUID.fullmatch(item['item_id']) and
        f.UUID.fullmatch(item['scope_id']) and item['scope_id'] in s['deck_ids']
        for i, item in enumerate(s['items']))


def decode_descriptor(raw, expected_session_id):
    if not 61 <= len(raw) <= 96 * 1024:
        return {'native_error': 264}
    magic, version, reserved, size, crc, total = struct.unpack_from('<IHHIII', raw)
    if magic != 0x534C4457 or version != 1 or reserved or \
            struct.unpack_from('<I', raw, 57)[0] != f.journal_crc(raw[:57]):
        return {'native_error': 265}
    sid = raw[20:57].split(b'\0', 1)[0].decode('latin1')
    if not f.UUID.fullmatch(sid) or sid != expected_session_id or \
            size > MAX_PAYLOAD['word'] or size + 61 != len(raw):
        return {'native_error': 264}
    if f.journal_crc(raw[61:]) != crc:
        return {'native_error': 265}
    try:
        value = decode_payload(raw[61:], 'word')
        if value['session_id'] != sid or not descriptor_semantics(value, total):
            raise DecodeError('descriptor semantics')
        return {'native_error': 0, 'value': value, 'total_count': total}
    except DecodeError:
        return {'native_error': 264}


def decode_cursor(raw, session_id):
    if len(raw) != 52:
        return {'native_valid': False}
    magic, version, paused, sid, reserved, crc = struct.unpack('<IHH37s3sI', raw)
    valid = magic == 0x57435552 and version == 1 and paused <= 1 and sid[-1] == 0 and \
        f.journal_crc(raw[:48]) == crc and sid.split(b'\0', 1)[0].decode('latin1') == session_id
    return {'native_valid': valid, 'paused': bool(paused), 'reserved_zero': not any(reserved)}


def session_policy(s, domain):
    if not f.UUID.fullmatch(s['session_id']) or not URL_SEED.fullmatch(s['seed']) or \
            s['next_sequence'] > MAX_SAFE or s['progress_revision'] > MAX_SAFE or not s['snapshot']:
        return False
    scope = s['deck_ids'] if domain == 'word' else [s['notebook_id']]
    if not scope or any(not f.UUID.fullmatch(v) for v in scope) or len(scope) != len(set(scope)):
        return False
    pins = s['snapshot']
    if len(pins) != len(scope) or {p['id'] for p in pins} != set(scope) or any(
            not HEX.fullmatch(p['sha256']) or not 0 < p['pack_revision'] <= MAX_SAFE or
            p['content_revision'] > MAX_SAFE for p in pins):
        return False
    ids, last = set(), -1
    for item in s['items']:
        if not f.UUID.fullmatch(item['item_id']) or item['scope_id'] not in scope or \
                item['item_id'] in ids or not last < item['ordinal'] <= 0xFFFFFFFF:
            return False
        ids.add(item['item_id'])
        last = item['ordinal']
    return True


class Material:
    def __init__(self, source, manifest, nvs_context=None):
        self.source, self.manifest, self.cache = source, manifest, {}
        self.nvs_context = nvs_context

    def raw(self, name):
        if name not in self.cache:
            info = self.manifest['files'][name]
            with f.open_regular(self.source / name) as stream:
                raw = stream.read(MAX_PAYLOAD[self.manifest['domain']] + 62)
            if len(raw) != info['bytes'] or f.sha256(raw) != info['sha256']:
                raise DecodeError('checkpoint material changed or exceeds codec bounds')
            self.cache[name] = raw
        return self.cache[name]

    def reference(self, name):
        return {'file': name, **{k: self.manifest['files'][name][k] for k in ('bytes', 'sha256')}}

    def choose(self, stem, decoder, expected_mode=None):
        attempts = []
        for suffix in ('v1', 'bak'):
            name = stem + '.' + suffix
            if name not in self.manifest['files']:
                continue
            parsed = decoder(self.raw(name))
            attempts.append({**self.reference(name), 'native_error': parsed['native_error']})
            if parsed['native_error'] == 0:
                # A valid primary in the wrong mode is a native terminal
                # mismatch, not permission to substitute an older backup.
                if expected_mode is not None and parsed['value']['mode'] != expected_mode:
                    return None, None, attempts, 'session_slot_mode_mismatch'
                return parsed['value'], name, attempts, None
        return None, None, attempts, 'session_material_missing_or_invalid'

    def validate(self, row):
        domain = self.manifest['domain']
        word = domain == 'word'
        stem = STEMS[row['mode']] if word else 'nses'
        s, name, attempts, reason = self.choose(stem, lambda raw: decode_session(raw, domain), row['mode'])
        result = {'state': 'held', 'scope': 'single_fact_against_retained_snapshot', 'session_attempts': attempts,
                  'unverified_context': (['word_nvs_pause_not_exported', 'committed_scope_generation_unverified',
                      'word_session_origin_and_current_pack_pins_unverified'] if word else ['current_pack_pins_unverified'])}
        if self.nvs_context is not None:
            result['unverified_context'] = ['word_pause_and_scope_not_bound_to_retained_session',
                'word_session_origin_and_current_pack_pins_unverified', 'device_nvs_pause_and_scope_revalidation_required']
        def held(why):
            result['reason'] = why
            return result
        if reason:
            return held(reason)
        result['session_source'] = self.reference(name)
        if s['session_id'] != row['session_id']:
            return held('session_identity_mismatch')
        if not session_policy(s, domain):
            return held('session_repair_policy_unproven')
        if word:
            cursor = stem + '.cur'
            if self.nvs_context is not None:
                resolved = self.nvs_context.resolve(s, self, cursor)
                result['nvs_session_context'] = resolved
                if not resolved['usable']:
                    return held(resolved['reason'])
                result['unverified_context'].remove('word_pause_and_scope_not_bound_to_retained_session')
            elif cursor in self.manifest['files']:
                result['legacy_cursor'] = {**self.reference(cursor),
                    **decode_cursor(self.raw(cursor), s['session_id']), 'authoritative_nvs': False}
            descriptor_stem = 'wl' + s['session_id'].replace('-', '')[:24]
            if any(descriptor_stem + '.' + suffix in self.manifest['files'] for suffix in ('v1', 'bak', 'tmp')):
                descriptor, descriptor_file, traces, error = self.choose(descriptor_stem,
                    lambda raw: decode_descriptor(raw, s['session_id']))
                result['descriptor_attempts'] = traces
                if error:
                    return held('descriptor_missing_or_invalid')
                result['descriptor_source'] = self.reference(descriptor_file)
                if any(s[k] != descriptor[k] for k in ('session_id', 'mode', 'purpose', 'ordering', 'seed',
                        'include', 'start_index', 'scope_generation', 'deck_ids', 'snapshot')) or \
                        any(item not in descriptor['items'] for item in s['items']):
                    return held('descriptor_session_binding_mismatch')
        checkpoint = row['local_checkpoint']
        if not word and checkpoint['next_sequence'] != row['sequence'] + 1:
            return held('note_next_sequence_inconsistent')
        result['before'] = {k: s[k] for k in (('position', 'phase', 'next_sequence', 'paused', 'active')
                                             if word else ('position', 'next_sequence', 'paused', 'active'))}
        def complete():
            observed = result.get('nvs_session_context')
            if observed is not None and observed['overlay_present']:
                # Match LoadSessionTransaction: authoritative pause is overlaid
                # AFTER replay, even when terminal replay changed paused.
                result['candidate']['paused'] = int(observed['paused'])
            return result
        pin_update = None
        if not word and row['action'] == 0:
            item = next((item for item in s['items'] if item['item_id'] == row['item_id']), None)
            if item and item['last_opened_at'] != row['occurred_at']:
                pin_update = {'item_id': row['item_id'], 'before': item['last_opened_at'],
                              'after': row['occurred_at']}
        if row['sequence'] < s['next_sequence']:
            result.update(state='covered_by_retained_snapshot', candidate=result['before'].copy())
            if pin_update:
                result['state'] = 'requires_last_opened_pin_checkpoint'
                result['candidate']['last_opened_pin_update'] = pin_update
            return complete()
        if not any(item['item_id'] == row['item_id'] for item in s['items']):
            return held('observation_item_not_in_candidate_window')
        target = checkpoint['next_position']
        if word:
            positions = [i for i, item in enumerate(s['items']) if item['ordinal'] == target]
            if not positions and target == (s['items'][-1]['ordinal'] + 1 if s['items'] else 0):
                positions = [len(s['items'])]
            if not positions:
                return held('checkpoint_ordinal_outside_candidate_window')
            position = positions[0]
        else:
            if target > len(s['items']):
                return held('checkpoint_position_outside_candidate_window')
            position = target
        candidate = {**result['before'], 'position': position, 'next_sequence': row['sequence'] + 1}
        if pin_update:
            candidate['last_opened_pin_update'] = pin_update
        if word:
            candidate['phase'] = checkpoint['next_phase']
            if s['mode'] in (0, 5) and position == len(s['items']) and not s['has_more']:
                candidate.update(active=0, paused=0)
        result.update(state='structurally_replayable', candidate=candidate)
        return complete()
