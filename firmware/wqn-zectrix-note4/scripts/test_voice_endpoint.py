#!/usr/bin/env python3
"""Compile real CMake endpoint definitions and both production URI setup blocks."""
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    cmake = (root/'main/CMakeLists.txt').read_text()
    config = '\n'.join(re.search(rf'^set\({name} .*?\)$', cmake, re.M).group(0) + '\n' +
                        re.search(rf'if\({name}\)\n.*?endif\(\)', cmake, re.S).group(0)
                        for name in ('WQN_API_BASE', 'WQN_VOICE_WS_URL'))
    stdpro = (root/'main/stdpro_ws_transport.cpp').read_text()
    flash = (root/'main/flash_session.cpp').read_text()
    def block(source):
        start = source.index('const std::string ws_url = wqn::BuildVoiceWebSocketUrl();')
        end = source.index('cfg.uri = ws_url.c_str();', start) + len('cfg.uri = ws_url.c_str();')
        return source[start:end]
    unit = '''#include <cassert>
#include <cstdio>
#include <string>
#include "voice_endpoint.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1;
enum class CmdDisposition { kExecuted };
std::string std_url, flash_url;
int completion = -1, cleanup = 0;
void PublishCompletion(int, int result, CmdDisposition) { completion = result; }
void Std() {
    struct { const char* uri = nullptr; } cfg;
    struct { int gen = 1; } cmd;
    @@STD@@
    std_url = cfg.uri;
}
esp_err_t Flash() {
    struct { const char* uri = nullptr; } cfg;
    auto fail_after_audio_begin = [](const char*, int error) { ++cleanup; return error; };
    @@FLASH@@
    flash_url = cfg.uri;
    return ESP_OK;
}
int main() {
    Std(); const auto result = Flash();
    const std::string expected = EXPECTED;
    assert(std_url == expected && flash_url == expected);
    if (expected.empty()) { assert(completion == ESP_ERR_INVALID_ARG && result == ESP_ERR_INVALID_ARG && cleanup == 1); }
    else { assert(completion == -1 && result == ESP_OK && cleanup == 0); }
    std::puts("Both production WS entry points and CMake definitions: passed");
}
'''.replace('@@STD@@', block(stdpro)).replace('@@FLASH@@', block(flash))
    cases = [('', '', 'wss://wqn.helema.cn/api/esp32/realtime'),
             ('http://dev.local:3000/api/esp32', '', 'ws://dev.local:3000/api/esp32/realtime'),
             ('https://[2001:db8::1]:8443/prefix/api', '', 'wss://[2001:db8::1]:8443/api/esp32/realtime'),
             ('https://rest.example/api', 'wss://voice.example/custom?q=1', 'wss://voice.example/custom?q=1'),
             ('ftp://invalid/api', '', ''),
             ('https://rest.example/api', 'https://invalid/ws', '')]
    with tempfile.TemporaryDirectory(prefix='wqn-endpoint-') as tmp:
        tmp = Path(tmp)
        (tmp/'endpoint.cpp').write_text(unit)
        # Execute the actual configuration statements, recording definitions in
        # place of an IDF target. The compiler consumes precisely those values.
        (tmp/'config.cmake').write_text('''set(COMPONENT_LIB host)
function(target_compile_definitions target visibility)
    foreach(definition IN LISTS ARGN)
        file(APPEND "${OUTPUT}" "${definition}\\n")
    endforeach()
endfunction()
''' + config)
        for api, ws, expected in cases:
            flags = tmp/'definitions'
            flags.write_text('')
            subprocess.run(['cmake', '-DOUTPUT='+str(flags), '-DWQN_API_BASE='+api,
                '-DWQN_VOICE_WS_URL='+ws, '-P', str(tmp/'config.cmake')], check=True, timeout=10)
            definitions = ['-D'+line for line in flags.read_text().splitlines()]
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all', *definitions,
                '-DEXPECTED="'+expected+'"', '-I'+str(root/'main'), str(tmp/'endpoint.cpp'),
                '-o', str(tmp/'endpoint')], check=True, timeout=20)
            subprocess.run([str(tmp/'endpoint')], check=True, timeout=5)
    print('Endpoint configuration: 6 variants passed; Flash v1 unchanged; not network/HIL proof')


if __name__ == '__main__':
    main()
