#!/usr/bin/env python3
"""Run the entire production Agent client with real SSE/cJSON and virtual HTTP.

SDK I/O/time are ports. No model, business API or physical device calls.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


PORTS = {
    'freertos/FreeRTOS.h': '#pragma once\n#include <cstdint>\nusing TickType_t=uint32_t;\n#define pdMS_TO_TICKS(x) (x)\n',
    'freertos/task.h': '#pragma once\n#include "freertos/FreeRTOS.h"\nvoid vTaskDelay(TickType_t);\n',
    'esp_err.h': '''#pragma once
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=257, ESP_ERR_INVALID_ARG=258;
constexpr int ESP_ERR_INVALID_STATE=259, ESP_ERR_INVALID_SIZE=260, ESP_ERR_NOT_FOUND=261;
constexpr int ESP_ERR_NOT_SUPPORTED=262, ESP_ERR_TIMEOUT=263, ESP_ERR_INVALID_RESPONSE=264;
inline const char* esp_err_to_name(int) { return "host"; }
''',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    # The event stream checks the connectivity service directly; the host build
    # has no netif, so it is stubbed as permanently online.
    'services/connectivity_service.h': '#pragma once\nnamespace wqn { namespace services {\ninline bool IsConnectivityOnline() { return true; }\n} }\n',
    'esp_crt_bundle.h': '#pragma once\ninline int esp_crt_bundle_attach(void*) { return 0; }\n',
    'esp_heap_caps.h': '''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_DMA=2;
inline size_t heap_caps_get_free_size(int) { return 123456; }
inline size_t heap_caps_get_largest_free_block(int) { return 123456; }
''',
    'esp_log.h': '''#pragma once
void HostLog(const char*, const char*, ...);
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
''',
    'esp_http_client.h': '''#pragma once
#include <cstdint>
#include "esp_err.h"
struct HostClient;
using esp_http_client_handle_t=HostClient*;
using esp_http_client_method_t=int;
constexpr int HTTP_METHOD_GET=0, HTTP_METHOD_POST=1, ESP_ERR_HTTP_EAGAIN=0x7007;
struct esp_http_client_config_t {
 const char* url; esp_http_client_method_t method; int timeout_ms;
 int (*crt_bundle_attach)(void*); int buffer_size, buffer_size_tx;
};
HostClient* esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_set_header(HostClient*,const char*,const char*);
int esp_http_client_set_timeout_ms(HostClient*,int);
int esp_http_client_open(HostClient*,int);
int esp_http_client_write(HostClient*,const char*,int);
int64_t esp_http_client_fetch_headers(HostClient*);
int esp_http_client_get_status_code(HostClient*);
int esp_http_client_read(HostClient*,char*,int);
int esp_http_client_close(HostClient*);
int esp_http_client_cleanup(HostClient*);
''',
}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', help='compile the client from a git revision')
    parser.add_argument('--mutation', choices=('no-deadline','ignore-waiting','failed-reply-clears',
                                               'ignore-reply-session','unvalidated-progress','wire-unbounded-budget','no-header-wait'))
    args = parser.parse_args()
    code = (subprocess.check_output(['git', 'show', f'{args.before}:firmware/wqn-zectrix-note4/main/opencode_client.cpp'],
                                   cwd=root, text=True) if args.before else (root/'main/opencode_client.cpp').read_text())
    policy = (root/'main/agent_run_progress.h').read_text()
    session = (root/'main/opencode_session.cpp').read_text()
    owner = (root/'scripts/testdata/agent_run_owner.inc').read_text()
    start = session.index('enum class WorkerCommand :')
    owner = owner.replace('@@COMMAND@@', session[start:session.index('\n};', start)+3])
    bodies = '\n'.join(function(session, signature) for signature in (
        'bool AgentRunInFlightLocked(', 'void RefreshAgentRunLeaseLocked()\n{',
        'void SetUserVisibleActivityLocked(', 'std::string AgentStreamFailureMessage(',
        'void SetErrorLocked(',
        'bool RunConflictsWithInFlightRun(', 'void DiscardOutboundReplies()\n{',
        'void RunPrompt(', 'void FinishCommand('))
    owner = owner.replace('@@OWNER@@', bodies)
    fixture = (root/'scripts/testdata/agent_run_progress.cpp').read_text().replace('@@OWNER_PROBE@@', owner)
    if args.mutation:
        mutations = {
            'no-deadline': ('if (!enabled_ || !permission_id_.empty() || !question_id_.empty())',
                            'if (true || !enabled_ || !permission_id_.empty() || !question_id_.empty())'),
            'ignore-waiting': ('if (!enabled_ || !permission_id_.empty() || !question_id_.empty())', 'if (!enabled_)'),
            'failed-reply-clears': ('if (!enabled_ || !succeeded)', 'if (!enabled_ || !(succeeded || true))'),
            'ignore-reply-session': ('owner == pending_owner', '(owner == pending_owner || true)'),
            'unvalidated-progress': ('if (DispatchAgentEvent(event_name, event_data, request.callback, request.callback_ctx, &event))',
                                     'if (DispatchAgentEvent(event_name, event_data, request.callback, request.callback_ctx, &event) || true)'),
            'wire-unbounded-budget': ('kAgentStreamIdleTimeoutMs * 1000LL',
                                      'kAgentStreamIdleTimeoutMs * 1000000000LL'),
            'no-header-wait': ('if (fetched != -ESP_ERR_HTTP_EAGAIN)', 'if (fetched < 0)'),
        }
        needle, replacement = mutations[args.mutation]
        client_mutation = args.mutation in ('unvalidated-progress', 'wire-unbounded-budget', 'no-header-wait')
        target = code if client_mutation else policy
        if target.count(needle) != 1:
            raise SystemExit('FAIL: Agent progress mutation boundary drift')
        target = target.replace(needle,replacement)
        if client_mutation: code = target
        else: policy = target
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))/'components/json/cJSON'
    macros = []
    for domain, name in [('DEVICE_CONTROL','device-control-v3'),('WORD_STUDY','word-study-v1'),
                         ('NOTE_STUDY','note-study-v1'),('PROBLEM_STUDY','problem-study-v1')]:
        digest = hashlib.sha256((root/f'contracts/{name}/{name}.schema.json').read_bytes()).hexdigest()
        macros.append(f'-DWQN_{domain}_SCHEMA_SHA256="{digest}"')
    with tempfile.TemporaryDirectory(prefix='wqn-agent-progress-') as directory:
        tmp = Path(directory)
        for name, content in PORTS.items():
            port = tmp/name
            port.parent.mkdir(parents=True, exist_ok=True)
            port.write_text(content)
        (tmp/'opencode_client.cpp').write_text(code)
        (tmp/'agent_run_progress.h').write_text(policy)
        (tmp/'test.cpp').write_text(fixture)
        binary = tmp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                        '-DCONFIG_WQN_AGENT_ENABLE=1', *macros,
                        '-I'+str(tmp),'-I'+str(root/'main'),
                        '-I'+str(root/'components/device_protocol/include'),'-I'+str(cjson),
                        str(tmp/'opencode_client.cpp'),str(tmp/'test.cpp'),
                        str(root/'main/sse_chunk.cpp'),str(cjson/'cJSON.c'),'-o',str(binary)],
                       check=True,timeout=60)
        env = dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        return subprocess.run([str(binary)],env=env,timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
