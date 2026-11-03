#!/usr/bin/env python3
"""Check an explicitly selected product profile in IDF's resolved JSON config.

Only policy keys are read/reported; never dump sdkconfig (it can hold secrets).
Other development profiles remain available without selecting this gate.
"""
import argparse
import json
from pathlib import Path


AI_LOCAL = {
    'IDF_TARGET': 'esp32s3',
    'SPIRAM': True,
    'WQN_WIFI_STA_ENABLE': True,
    'WQN_DEVICE_CONTROL_V3_ENABLE': True,
    'WQN_AI_ENABLE': True,
    'WQN_AGENT_ENABLE': True,
    'WQN_EPD_UI_ENABLE': True,
    'WQN_EPD_LOCAL_PARTIAL_ENABLE': True,
    'PM_ENABLE': True,
    'PM_DFS_INIT_AUTO': True,
    'FREERTOS_USE_TICKLESS_IDLE': True,
    'USJ_NO_AUTO_LS_ON_CONNECTION': True,
    'WQN_DEEP_SLEEP_ENABLE': True,
    'WQN_RETAINED_STANDBY_IDLE_MS': 60000,
    'WQN_DEEP_SLEEP_IDLE_MS': 1800000,
    'WQN_AI_AUDIO_SELFTEST_ENABLE': False,
    'WQN_DEV_MENU_ENABLE': False,
}
PROFILES = {
    'ai-local': AI_LOCAL,
    'ai-local-agent-off': dict(AI_LOCAL, WQN_AGENT_ENABLE=False),
    'ai-local-dev-menu': dict(AI_LOCAL, WQN_DEV_MENU_ENABLE=True),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True,
                        help='build/config/sdkconfig.json, after IDF configuration')
    parser.add_argument('--profile', choices=tuple(PROFILES), required=True)
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        print('FAIL: resolved profile configuration is unavailable or invalid')
        return 1
    if not isinstance(config, dict):
        print('FAIL: resolved profile configuration must be an object')
        return 1
    failed = False
    for key, expected in PROFILES[args.profile].items():
        actual = config.get(key)
        if type(actual) is not type(expected) or actual != expected:
            # Do not echo unexpected values; an accidentally supplied config may
            # put a credential or arbitrary input under a policy key too.
            print(f'FAIL: resolved profile {args.profile}: {key} differs or is absent')
            failed = True
    if not failed:
        print(f'PASS: resolved profile {args.profile}: {len(PROFILES[args.profile])} policy keys (NOT HIL)')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
