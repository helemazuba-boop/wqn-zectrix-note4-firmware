#!/usr/bin/env python3
"""Build and archive a RAM-diagnostics on/off pair; never flash either image."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CONFIG_LINE = re.compile(r'^(CONFIG_\w+)=(.*)$|^# (CONFIG_\w+) is not set$')
DIAGNOSTIC = 'CONFIG_WQN_MEMORY_DIAGNOSTICS'
DEPENDENCIES = {
    DIAGNOSTIC, 'CONFIG_WQN_MEMORY_SAMPLE_SECONDS', 'CONFIG_WQN_MEMORY_TRACE_ENABLE',
    'CONFIG_WQN_MEMORY_HISTORY_ENABLE', 'CONFIG_WQN_MEMORY_HISTORY_HOURS',
    'CONFIG_WQN_MEMORY_TRACE_DELAY_SECONDS', 'CONFIG_WQN_MEMORY_TRACE_SECONDS',
    'CONFIG_WQN_MEMORY_TRACE_RECORDS',
}
CACHE_OVERRIDES = ('WQN_API_BASE', 'WQN_VOICE_WS_URL', 'CMAKE_BUILD_TYPE')


def config_values(text):
    result = {}
    for line in text.splitlines():
        match = CONFIG_LINE.fullmatch(line)
        if match:
            key, value, disabled = match.groups()
            result[key or disabled] = value if key else 'n'
    return result


def disabled_config(text):
    if config_values(text).get(DIAGNOSTIC) != 'y':
        raise ValueError('Baseline must have RAM diagnostics enabled.')
    return re.sub(r'^CONFIG_WQN_MEMORY_DIAGNOSTICS=y$',
                  '# CONFIG_WQN_MEMORY_DIAGNOSTICS is not set', text, flags=re.MULTILINE)


def compare_configs(baseline, control):
    before, after = config_values(baseline), config_values(control)
    changed = {key for key in before.keys() | after.keys() if before.get(key) != after.get(key)}
    if before.get(DIAGNOSTIC) != 'y' or after.get(DIAGNOSTIC) != 'n':
        raise ValueError('Pair must switch RAM diagnostics from y to n.')
    unexpected = changed - DEPENDENCIES
    if unexpected:
        # Names only: sdkconfig can contain credentials.
        raise ValueError('Unexpected configuration drift: ' + ', '.join(sorted(unexpected)))
    return {key: {'on': before.get(key), 'off': after.get(key)} for key in sorted(changed)}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def require_clean(commit):
    if git('rev-parse', 'HEAD') != commit or git('status', '--porcelain', '--untracked-files=no'):
        raise ValueError('Tracked worktree/HEAD changed; do not label a mixed-source pair.')


def cache_overrides(build):
    path = build / 'CMakeCache.txt'
    if not path.is_file():
        raise ValueError('Baseline build must already have a configured CMakeCache.txt.')
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'(\w+):[^=]+=(.*)', line)
        if match and match[1] in CACHE_OVERRIDES:
            values[match[1]] = match[2]
    return [f'-D{key}={value}' for key, value in sorted(values.items())]


def run_build(build, config, overrides, log, environment):
    command = ['idf.py', '-B', str(build), f'-DSDKCONFIG={config}', *overrides, 'reconfigure', 'build']
    print(f'Building {build.name}; output: {log}', flush=True)
    with log.open('w', encoding='utf-8') as stream:
        if (build / 'CMakeCache.txt').exists():
            # SOURCE_DATE_EPOCH alone cannot invalidate cached __DATE__/__TIME__
            # objects or an old bootloader. Config lives outside this directory.
            subprocess.run(['idf.py', '-B', str(build), 'fullclean'], cwd=ROOT,
                           env=environment, stdout=stream, stderr=subprocess.STDOUT, check=True)
        subprocess.run(command, cwd=ROOT, env=environment, stdout=stream, stderr=subprocess.STDOUT, check=True)
    output = log.read_text(errors='replace')
    if re.search(r'\bwarning:', output, flags=re.IGNORECASE):
        raise ValueError(f'Build contains warnings; inspect {log}.')
    if 'M8 architecture ownership gate passed' not in output:
        raise ValueError(f'M8 gate result missing; inspect {log}.')
    if '[PASS] UI owner fixed frame:' not in output or '[FAIL]' in output:
        raise ValueError(f'Compiled UI stack result missing; inspect {log}.')


def application_metadata(path):
    # ESP32-S3 application header (24 B), first segment header (8 B), then
    # esp_app_desc_t. Refuse unknown layouts instead of guessing artifact identity.
    data = path.read_bytes()[32:32 + 256]
    if len(data) != 256 or int.from_bytes(data[:4], 'little') != 0xabcd5432:
        raise ValueError('Unrecognized ESP32-S3 application descriptor.')
    fields = {'version': (16, 32), 'project': (48, 32), 'time': (80, 16),
              'date': (96, 16), 'idf_ver': (112, 32)}
    return {key: data[start:start + size].split(b'\0')[0].decode('ascii')
            for key, (start, size) in fields.items()}


def archive(build, config, destination):
    destination.mkdir()
    description = json.loads((build / 'project_description.json').read_text())
    project = description['project_name']
    files = {f'{project}.bin', f'{project}.elf', f'{project}.map', 'flasher_args.json'}
    flash = json.loads((build / 'flasher_args.json').read_text())
    files.update(flash['flash_files'].values())
    artifacts = {}
    for name in sorted(files):
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unexpected artifact path in build metadata.')
        source, target = build / relative, destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        artifacts[name] = {'bytes': target.stat().st_size, 'sha256': sha256(target)}
    target = destination / 'sdkconfig'
    # Local config may contain WiFi credentials. Archive privately, never print it.
    target.touch(mode=0o600)
    shutil.copyfile(config, target)
    artifacts['sdkconfig'] = {'sha256': sha256(target)}
    sdk = Path(description['idf_path'])
    idf_version = subprocess.check_output(['git', '-C', str(sdk), 'describe', '--tags', '--always', '--dirty'], text=True).strip()
    return {'build_directory': str(build), 'artifacts': artifacts,
            'application': application_metadata(destination / f'{project}.bin'),
            'idf_ver': idf_version, 'project_version': description['project_version']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New local artifact directory (private configs).')
    parser.add_argument('--baseline-build', type=Path, default=ROOT / 'build-wsl')
    parser.add_argument('--control-build', type=Path, default=ROOT / 'build-b5-diag-off-20261023')
    args = parser.parse_args()
    output, baseline, control = (p.resolve() for p in (args.output, args.baseline_build, args.control_build))
    if not shutil.which('idf.py'):
        parser.error('Source the ESP-IDF export.sh before running this script.')
    if os.environ.get('WQN_RELEASE_VERSION'):
        parser.error('Use a local dev environment with WQN_RELEASE_VERSION unset.')
    if control.exists() or output.exists():
        parser.error('Control build and output paths must be new; existing artifacts are never overwritten.')
    if control.parent != ROOT or not re.fullmatch(r'build-[\w-]+-\d{8}', control.name):
        parser.error('Control directory must be build-<purpose>-<YYYYMMDD> under the firmware root.')
    config = ROOT / 'sdkconfig'
    original = config.read_bytes()
    disabled_config(original.decode())  # Validate before invoking IDF or making directories.
    commit = git('rev-parse', 'HEAD')
    require_clean(commit)
    overrides = cache_overrides(baseline)
    epoch = git('show', '-s', '--format=%ct', commit)
    build_time = datetime.fromtimestamp(int(epoch), timezone.utc).strftime('%Y-%m-%d %H:%M')
    environment = os.environ | {'SOURCE_DATE_EPOCH': epoch, 'WQN_BUILD_TIME': build_time}
    output.mkdir(parents=True, mode=0o700)
    (output / 'sdkconfig.input').touch(mode=0o600)
    (output / 'sdkconfig.input').write_bytes(original)
    run_build(baseline, config, overrides, output / 'diagnostics-on-build.log', environment)
    normalized = config.read_text()
    # Reconfiguration must not silently change unrelated baseline options either.
    if config_values(original.decode()) != config_values(normalized):
        raise ValueError('Baseline reconfiguration changed options; review sdkconfig.input before continuing.')
    require_clean(commit)
    on = archive(baseline, config, output / 'diagnostics-on')
    control.mkdir()
    control_config = control / 'sdkconfig'
    control_config.touch(mode=0o600)
    control_config.write_text(disabled_config(normalized))
    run_build(control, control_config, overrides, output / 'diagnostics-off-build.log', environment)
    differences = compare_configs(normalized, control_config.read_text())
    if config.read_text() != normalized:
        raise ValueError('Default sdkconfig changed during control build.')
    require_clean(commit)
    off = archive(control, control_config, output / 'diagnostics-off')
    if on['idf_ver'] != off['idf_ver'] or on['project_version'] != off['project_version']:
        raise ValueError('SDK/project version differs between pair members.')
    if on['application'] != off['application']:
        raise ValueError('Application descriptor identity/date differs between pair members.')
    for name in ('bootloader/bootloader.bin', 'partition_table/partition-table.bin', 'ota_data_initial.bin'):
        if on['artifacts'][name] != off['artifacts'][name]:
            raise ValueError('Flash support artifact differs between pair members: ' + name)
    manifest = {'schema_version': 1, 'source_commit': commit,
                'baked_build': git('describe', '--always', '--dirty', '--abbrev=8'),
                'source_date_epoch': int(epoch), 'build_time_utc': build_time,
                'configuration_delta': differences, 'diagnostics_on': on, 'diagnostics_off': off,
                'limits': ['Builds only; neither image was flashed.',
                           'Includes all RAM diagnostics/history changes, not only periodic sampling.',
                           'Fault journal, storage logs, developer menu and log level stay enabled in both.',
                           'No electrical power or timing result is implied by artifact size differences.']}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Pair ready: {output / "manifest.json"}. Default build-wsl/config remains diagnostics-on.')


if __name__ == '__main__':
    main()
