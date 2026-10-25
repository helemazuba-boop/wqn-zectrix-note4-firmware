#!/usr/bin/env python3
"""Exercise production HTTP SSE and WS owner failure paths. Host ports, not HIL."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_audio_runtime import function


PORTS = {
    'esp_check.h': '#pragma once\n',
    'esp_log.h': '''#pragma once
void HostLog(const char*, const char*, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(tag, ...) HostLog(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) HostLog(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) HostLog(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) HostLog(tag, __VA_ARGS__)
''',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'esp_mac.h': '''#pragma once
#include <cstdint>
#include "esp_err.h"
constexpr int ESP_MAC_WIFI_STA = 0;
esp_err_t esp_read_mac(uint8_t*, int);
''',
    'esp_crt_bundle.h': '#pragma once\ninline int esp_crt_bundle_attach(void*) { return 0; }\n',
    'esp_http_client.h': '''#pragma once
#include <cstdint>
#include "esp_err.h"
struct HostClient;
using esp_http_client_handle_t = HostClient*;
enum esp_http_client_event_id_t { HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED };
struct esp_http_client_event_t {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void* user_data;
    void* data;
    int data_len;
};
constexpr int HTTP_METHOD_POST = 1;
struct esp_http_client_config_t {
    const char* url;
    int method, timeout_ms;
    int (*crt_bundle_attach)(void*);
    int buffer_size, buffer_size_tx;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
    void* user_data;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char*, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_get_errno(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char*, int);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', choices=('http-sticky', 'ws-sticky', 'ws-continue',
                        'reset-sticky', 'no-deadline', 'renew-budget', 'no-data-deadline',
                        'late-terminal', 'early-status', 'fixed-host'))
    parser.add_argument('--before', help='Use production sources from a git revision')
    args = parser.parse_args()

    def source(name):
        if args.before:
            return subprocess.check_output(['git', 'show', f'{args.before}:firmware/wqn-zectrix-note4/main/{name}'],
                                           cwd=root, text=True)
        return (root/'main'/name).read_text()

    http = source('wqn_api_stream.cpp')
    ws = source('stdpro_ws_transport.cpp')
    sse_h = source('sse_chunk.h')
    mutations = {
        'http-sticky': ('if (ctx->parser.overflowed())', 'if (false)'),
        'no-deadline': ('ctx->deadline_us - esp_timer_get_time() < 1000', 'false'),
        'renew-budget': ('if (!CheckDeadline(ctx)) {',
                         'ctx->deadline_us = esp_timer_get_time() + 100000; if (!CheckDeadline(ctx)) {'),
        'no-data-deadline': ('!CheckDeadline(ctx) || ctx->fatal', 'ctx->fatal'),
        'late-terminal': ('if (ctx->terminal_event_seen) {\n          break;', 'if (false) {\n          break;'),
        'early-status': ('CaptureHttpStatus(ctx, evt->client);', '/* status ignored in buffered body */'),
    }
    if args.mutation in mutations:
        needle, replacement = mutations[args.mutation]
        if http.count(needle) != 1:
            raise SystemExit('FAIL: HTTP mutation boundary drift')
        http = http.replace(needle, replacement)
    if args.mutation == 'no-data-deadline':
        http = http.replace('while (CheckDeadline(ctx)) {', 'while (true) {')
    if args.mutation == 'ws-sticky':
        ws = ws.replace('if (g_sse_parser.overflowed())', 'if (false)')
    if args.mutation == 'ws-continue':
        ws = ws.replace('TeardownFailedTurn();\n          return;',
                        'xTaskNotify(g_transport_task, kEvtBitResetRequired, eSetBits);\n          continue;')
    if args.mutation == 'reset-sticky':
        ws = ws.replace('g_sse_parser.reset();', 'g_sse_parser.clear();')
    ws_unit = (root/'scripts/testdata/stream_transport_ws.cpp').read_text()
    types = (ws[ws.index('enum class ConnectionState'):ws.index('struct PcmBlock')] +
             ws[ws.index('enum class CmdDisposition'):ws.index('struct CtrlCmd {')] +
             ws[ws.index('enum class TeardownReason'):ws.index('struct EmergencyAbortTarget')])
    ws_unit = ws_unit.replace('@@TYPES@@', types)
    # Include the actual teardown: FINAL ambiguity and terminal ledger decisions
    # are production behavior, not a replacement that just increments a count.
    ws_body = '\n'.join(function(ws, signature) for signature in (
        'void DoDestroyWs()', 'void TeardownFailedTurn(', 'void ProcessTextStream()',
        'void* AllocPrefersPsram('))
    ws_unit = ws_unit.replace('@@PRODUCTION@@', ws_body)
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))/'components/json/cJSON'
    if not (cjson/'cJSON.c').is_file():
        raise SystemExit('FAIL: activate project ESP-IDF for cJSON')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    schema_flags = []
    for domain, schema in (('DEVICE_CONTROL', 'device-control-v3'), ('NOTE_STUDY', 'note-study-v1'),
                           ('PROBLEM_STUDY', 'problem-study-v1'), ('WORD_STUDY', 'word-study-v1')):
        paths = list((root/'contracts').rglob(schema+'.schema.json'))
        if len(paths) != 1:
            raise SystemExit('FAIL: schema path drift: '+schema)
        digest = hashlib.sha256(paths[0].read_bytes()).hexdigest()
        schema_flags.append(f'-DWQN_{domain}_SCHEMA_SHA256="{digest}"')
    with tempfile.TemporaryDirectory(prefix='wqn-stream-') as tmp:
        tmp = Path(tmp)
        for name, content in PORTS.items():
            (tmp/name).write_text(content)
        (tmp/'wqn_api_stream.cpp').write_text(http)
        (tmp/'sse_chunk.h').write_text(sse_h)
        (tmp/'sse_chunk.cpp').write_text(source('sse_chunk.cpp'))
        # Pure endpoint implementation is also compiled with real config defaults.
        endpoint = (root/'main/voice_endpoint.h').read_text()
        if args.mutation == 'fixed-host':
            endpoint = endpoint.replace('return scheme + authority + ws_path;',
                                        'return "wss://wqn.helema.cn" + ws_path;')
        (tmp/'voice_endpoint.h').write_text(endpoint)
        (tmp/'http.cpp').write_text((root/'scripts/testdata/stream_transport_http.cpp').read_text())
        (tmp/'ws.cpp').write_text(ws_unit)
        for unit in ('http', 'ws'):
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                *schema_flags,
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                '-I'+str(tmp), '-I'+str(root/'scripts/testdata/sync_host'), '-I'+str(root/'main'),
                '-I'+str(root/'components/device_protocol/include'), '-I'+str(cjson),
                str(tmp/(unit+'.cpp')), str(tmp/'sse_chunk.cpp'), str(cjson/'cJSON.c'),
                '-o', str(tmp/unit)], check=True, timeout=45)
            result = subprocess.run([str(tmp/unit)], timeout=15)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
