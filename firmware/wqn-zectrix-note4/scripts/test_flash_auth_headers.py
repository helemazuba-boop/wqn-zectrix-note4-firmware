#!/usr/bin/env python3
"""Compile production Flash headers in both modes; no network or device access."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    args = parser.parse_args()
    root = args.source_root or Path(__file__).resolve().parents[1]
    source = (root / 'main/flash_session.cpp').read_text()
    # Check by header name only: never embed or print a retired credential.
    assert '"X-WS-Secret"' not in source, 'Flash must not contain a shared credential header'
    start = source.index('std::string bearer = "Bearer " + access_token;')
    end = source.index('#endif', start) + len('#endif')
    production_headers = source[start:end]
    unit = r'''
#include <cassert>
#include <map>
#include <string>
#define WQN_FIRMWARE_NAME "test-firmware"
#define WQN_FIRMWARE_VERSION "test-version"
#define WQN_FLASH_WS_SUBPROTOCOL "wqn-flash-v2"
struct { int ws_client = 1; } g_flash;
std::map<std::string, std::string> headers;
int esp_websocket_client_append_header(int, const char* name, const char* value) {
    assert(headers.emplace(name, value).second);
    return 0;
}
int main() {
    const std::string access_token = "fixture-device-token";
    @@HEADERS@@
    assert(headers.at("Authorization") == "Bearer fixture-device-token");
    assert(headers.at("Accept") == "application/json");
    assert(headers.at("Content-Type") == "application/json");
#if CONFIG_WQN_FLASH_PROTOCOL_V2
    assert(headers.at("X-WQN-Protocol") == "wqn-flash-v2");
    assert(headers.at("X-WQN-Client-Version") == "test-firmware@test-version");
    assert(headers.size() == 5);
#else
    assert(headers.size() == 3);
#endif
}
'''.replace('@@HEADERS@@', production_headers)
    with tempfile.TemporaryDirectory(prefix='wqn-flash-auth-') as temporary:
        directory = Path(temporary)
        cpp = directory / 'headers.cpp'
        cpp.write_text(unit)
        for mode in (0, 1):
            binary = directory / f'headers-{mode}'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            f'-DCONFIG_WQN_FLASH_PROTOCOL_V2={mode}',
                            str(cpp), '-o', str(binary)], check=True, timeout=20)
            subprocess.run([str(binary)], check=True, timeout=5)
    print('Flash V1/V2 production auth headers passed; legacy service acceptance and HIL not tested')


if __name__ == '__main__':
    main()
