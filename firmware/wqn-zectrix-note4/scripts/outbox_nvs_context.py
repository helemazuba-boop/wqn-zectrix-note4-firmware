"""Source-bound, independently replayed NVS facts for offline Word plans.

This resolves facts from a pinned image. It cannot attest hardware identity,
live scope/pack admission, publication, installation or repair authorization.
"""
import copy
from pathlib import Path
import subprocess

import nvs_context as context

FORMAT = 'wqn-repair-nvs-inputs-v1'
PAIRS = (
    ('source_bundle', 'source_manifest_sha256'),
    ('profile', 'profile_sha256'),
    ('spiffs_build', 'spiffs_build_manifest_sha256'),
    ('import_bundle', 'import_manifest_sha256'),
    ('nvs_build', 'nvs_build_manifest_sha256'),
    ('context_bundle', 'context_manifest_sha256'),
)
DIRECTORIES = ('source_bundle', 'spiffs_build', 'import_bundle', 'nvs_build', 'context_bundle')
REVALIDATION = 'device_nvs_pause_and_scope_revalidation_required'


class ContextError(ValueError):
    pass


class VerifiedContext:
    def __init__(self, descriptor, descriptor_pin, source_pin, domain, sensitive_confirmed, output=None):
        if not sensitive_confirmed:
            raise ContextError('NVS_SENSITIVE_SCOPE_REQUIRED')
        if domain != 'word':
            raise ContextError('NVS_CONTEXT_WORD_ONLY')
        self.descriptor, self.descriptor_pin = Path(descriptor), descriptor_pin
        self.source_pin = source_pin
        self._facts, self._summary = self._verify(output)

    def _verify(self, output=None):
        raw = context.raw
        try:
            spec = raw.load(self.descriptor, self.descriptor_pin, 16384)
            expected = {'format', *(key for pair in PAIRS for key in pair)}
            if not isinstance(spec, dict) or set(spec) != expected or spec['format'] != FORMAT:
                raise ContextError('NVS_INPUTS_SHAPE_INVALID')
            paths = {}
            for name, pin in PAIRS:
                value = spec[name]
                if not isinstance(value, str) or not value or len(value) > 4096 or '\0' in value:
                    raise ContextError('NVS_INPUT_PATH_INVALID')
                path = Path(value)
                paths[name] = path if path.is_absolute() else self.descriptor.parent / path
                raw.wire.pin(spec[pin], 32)
            if output is not None and any(Path(output).resolve().is_relative_to(paths[name].resolve()) for name in DIRECTORIES):
                raise ContextError('NVS_OUTPUT_INSIDE_INPUT')
            values = (paths['source_bundle'], spec['source_manifest_sha256'], paths['profile'], spec['profile_sha256'],
                paths['spiffs_build'], spec['spiffs_build_manifest_sha256'], paths['import_bundle'], spec['import_manifest_sha256'],
                paths['nvs_build'], spec['nvs_build_manifest_sha256'], paths['context_bundle'])
            context.verify(*values, expected_manifest_sha256=spec['context_manifest_sha256'], sensitive_confirmed=True)
            manifest = raw.load(paths['context_bundle'] / 'manifest.json', spec['context_manifest_sha256'])
            info = manifest['artifacts']['context.json']
            facts = raw.load(paths['context_bundle'] / 'context.json', info['sha256'])
            if facts['binding']['forensics'].get('word') != self.source_pin:
                raise ContextError('NVS_FORENSIC_SOURCE_MISMATCH')
            summary = dict(format=FORMAT, inputs_sha256=self.descriptor_pin,
                **{pin: spec[pin] for _, pin in PAIRS},
                forensic_source_manifest_sha256=self.source_pin,
                image_sha256=facts['binding']['raw_import_image_sha256'],
                resolver_source_sha256=raw.hash_file(Path(__file__)),
                source_binding='operator_pinned_unattested', independent_original_image_replay=True,
                identity_authenticated=False, repair_authorized=False, installation_ready=False)
            return facts, summary
        except ContextError:
            raise
        except (context.raw.native.NativeExecutionError, subprocess.SubprocessError) as error:
            raise ContextError('NVS_NATIVE_EXECUTION_FAILED') from error
        except (ValueError, OSError, KeyError, TypeError) as error:
            raise ContextError('NVS_CONTEXT_VERIFICATION_FAILED') from error

    def summary(self):
        return copy.deepcopy(self._summary)

    def reverify(self):
        facts, summary = self._verify()
        if facts != self._facts or summary != self._summary:
            raise ContextError('NVS_INPUTS_CHANGED')

    def resolve(self, session, material, legacy_name):
        """Follow the production cursor precedence within the verified image."""
        facts = self._facts
        result = dict(usable=False, mode=session['mode'], session_id=session['session_id'],
            committed_scope_generation_decimal=facts['values']['word_deck_gen'].get('u64_decimal'),
            overlay_present=False, paused=None, source_binding='operator_pinned_unattested')
        def held(reason):
            return {**result, 'reason': reason}
        if not facts['selected_values_read_complete']:
            return held('nvs_selected_values_unresolved')
        if not facts['scope_context_usable']:
            return held('nvs_scope_unusable')
        if result['committed_scope_generation_decimal'] != str(session['scope_generation']):
            return held('nvs_scope_generation_mismatch')
        key = context.KEYS[session['mode']]
        row = facts['values'][key]
        result['nvs_key'] = key
        if row['state'] == 'present':
            cursor = row['cursor']
            if cursor['session_id'] == session['session_id']:
                result.update(overlay_present=True, paused=cursor['paused'], pause_source='nvs')
            else:
                # A valid previous SID suppresses legacy fallback too. The
                # production reader returns no overlay, not forced unpaused.
                result.update(pause_source='replayed_snapshot_after_stale_nvs_sid', observed_cursor_session_id=cursor['session_id'])
        elif row['state'] == 'absent':
            if legacy_name in material.manifest['files']:
                decoded = context.cursor(material.raw(legacy_name))
                result['legacy_source'] = material.reference(legacy_name)
                if not decoded['valid']:
                    return held('legacy_cursor_unresolved_after_nvs_absence')
                if decoded['session_id'] == session['session_id']:
                    result.update(overlay_present=True, paused=decoded['paused'], pause_source='legacy_after_proven_nvs_absence')
                else:
                    result.update(pause_source='replayed_snapshot_after_stale_legacy_sid', observed_cursor_session_id=decoded['session_id'])
            else:
                result.update(pause_source='replayed_snapshot_after_proven_cursor_absence')
        else:
            return held('nvs_pause_unresolved')
        result['usable'] = True
        return result
