#!/usr/bin/env python3
"""Transport lifetime notifications with source-backed WebSocket stop ordering.

Actual SDK dispatch/register/abort/stop/destroy and task exit code; RTOS,
event-loop execution, allocation and network are ports. No network or HIL.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_audio_runtime import function
from test_display_recovery import structure


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before')
    parser.add_argument('--mutation',choices=('keep-backlog','ignore-client','ignore-generation','trust-raw-bits'))
    args=parser.parse_args()
    ws=(subprocess.check_output(['git','show',f'{args.before}:firmware/wqn-zectrix-note4/main/stdpro_ws_transport.cpp'],cwd=root,text=True)
        if args.before else (root/'main/stdpro_ws_transport.cpp').read_text())
    sdk=(root/'components/esp_websocket_client/esp_websocket_client.c').read_text()
    mailbox=(root/'main/ws_event_mailbox.h').read_text()
    if args.mutation:
        changes={
            'keep-backlog':('header','        pending_ = 0;','        /* retained backlog mutation */'),
            'ignore-generation':('header','return generation != 0 && generation == active_generation_;',
                                  '(void)generation; return active_generation_ != 0;'),
            'ignore-client':('source','event->client != context->client','false'),
            'trust-raw-bits':('source','bits = (bits & ~kWsCallbackEventBits) | TakeWsEvents();','bits |= TakeWsEvents();'),
        }
        target,old,new=changes[args.mutation]
        text=mailbox if target=='header' else ws
        expected=2 if args.mutation=='keep-backlog' else 1
        assert text.count(old)==expected,'mutation boundary drift'
        text=text.replace(old,new,1)
        if target=='header':mailbox=text
        else:ws=text
    public=(root/'main/stdpro_ws_transport.h').read_text()
    types='\n'.join(structure(ws,s) for s in ('enum class ConnectionState :','enum class TurnState :',
        'struct PcmBlock {','enum class CtrlCmdType :','enum class CmdDisposition :','struct CtrlCmd {',
        'struct CtrlCompletion {','enum class TeardownReason :','struct EmergencyAbortTarget {'))
    enums=public[public.index('enum class FinalHandoffResult'):public.index('// request_id is diagnostic')]
    constants=re.search(r'constexpr size_t kPcmFramesPerBlock[^;]+;',ws).group()+'\n'+ '\n'.join(
        re.findall(r'constexpr uint32_t kEvtBit\w+\s*=[^;]+;',ws))
    callback_state=''
    if 'struct WsCallbackContext {' in ws:
        callback_state=ws[ws.index('struct WsCallbackContext {'):ws.index('uint32_t g_active_conn_op_gen =')]
    execute=function(ws,'void ExecuteCtrlCmdSingleStep(')
    execute=execute[:execute.index('        case CtrlCmdType::kStartTurn:')]+ '\n    default: std::abort();\n    }\n}\n'
    task=function(ws,'void VoiceWsTransportTask(')
    start=task.index('        if (xTaskNotifyWait(')
    end=task.index('        // Evaluate asynchronous connecting state deadline',start)
    root_step='void OwnerStep() {uint32_t notified_bits=0;TickType_t wait_ticks=0;\n'+task[start:end]+'\n}\n'
    recv=function(sdk,'static esp_err_t esp_websocket_client_recv(')
    stop_guard=function(recv,'if (xEventGroupGetBits(client->status_bits) & REQUESTED_STOP_BIT)')
    stopped_recv='int esp_websocket_client_recv(esp_websocket_client_handle_t client) {\n'+stop_guard+'\nreturn ESP_OK;\n}'
    sdk_task=function(sdk,'static void esp_websocket_client_task(')
    read_failure=function(sdk_task,'if (esp_websocket_client_recv(client) == ESP_FAIL)')
    finish=sdk_task[sdk_task.index('esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_FINISH, NULL, 0);'):]
    sdk_code='\n'.join(function(sdk,s) for s in ('static esp_err_t esp_websocket_client_dispatch_event(',
        'static esp_err_t esp_websocket_client_abort_connection(', 'static esp_err_t stop_wait_task(',
        'esp_err_t esp_websocket_client_stop(', 'esp_err_t esp_websocket_client_destroy(',
        'esp_err_t esp_websocket_register_events('))
    replacements={
        '@@TYPES@@':types,'@@CONSTANTS@@':constants,'@@PUBLIC_ENUMS@@':enums,
        '@@CALLBACK_STATE@@':callback_state,
        '@@SDK@@':sdk_code,'@@STOPPED_RECV@@':stopped_recv,
        '@@WORKER_READ@@':read_failure,'@@WORKER_FINISH@@':finish,
        '@@TRANSPORT@@':'\n'.join(function(ws,s) for s in ('void DoDestroyWs()',
            'void TeardownFailedTurn(', 'void WebsocketEventHandler(', 'void HandleAllNotificationBits(')),
        '@@CONNECT@@':execute,'@@STEP@@':root_step,
    }
    fixture=(root/'scripts/testdata/ws_event_lifetime.cpp').read_text()
    for marker,body in replacements.items():
        assert fixture.count(marker)==1,marker
        fixture=fixture.replace(marker,body)
    with tempfile.TemporaryDirectory(prefix='wqn-ws-lifetime-') as directory:
        tmp=Path(directory);unit=tmp/'test.cpp';unit.write_text(fixture)
        (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=257,ESP_ERR_INVALID_ARG=258,ESP_ERR_INVALID_STATE=259,ESP_ERR_TIMEOUT=263;\n')
        (tmp/'ws_event_mailbox.h').write_text(mailbox)
        binary=tmp/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined',
            '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie',
            '-DHAS_CALLBACK_CONTEXT='+str(int(bool(callback_state))),
            '-I'+str(tmp),'-I'+str(root/'main'),str(unit),'-o',str(binary)],check=True,timeout=60)
        return subprocess.run([str(binary)],timeout=30,env=dict(os.environ,
            ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')).returncode


if __name__=='__main__':raise SystemExit(main())
