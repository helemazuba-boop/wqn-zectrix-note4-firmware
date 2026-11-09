#!/usr/bin/env python3
"""Run the installed SDK read loop and production timeout helper with I/O ports."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=root/'main/wqn_api_stream.cpp')
    p.add_argument('--mutation',choices=('full-read-budget',))
    a=p.parse_args()
    source=a.source.read_text();helper=function(source,'bool ApplyRemainingTimeout(')
    if a.mutation:
        old='std::strcmp(ctx->phase, "read") == 0';assert helper.count(old)==1
        helper=helper.replace(old,'false')
    sdk=Path(os.environ.get('IDF_PATH','/home/unknow/esp/esp-idf-v5.5'))
    reader=function((sdk/'components/esp_http_client/esp_http_client.c').read_text(),'int esp_http_client_read(')
    header='''#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
typedef struct { int raw_len; char *raw_data,*data,*output_ptr; } Buffer;
typedef Buffer esp_http_buffer_t;
typedef struct { Buffer *buffer; bool is_chunked; int64_t data_process,content_length; } Response;
typedef struct Client { Response *response; bool is_chunk_complete; int buffer_size_rx,timeout_ms; void *transport,*parser,*parser_settings; } Client;
typedef Client* esp_http_client_handle_t;
extern int64_t clock_us;
extern int socket_calls,set_timeout_error;
void ConfigureCallback(void);
int esp_http_client_read(Client*,char*,int);
int esp_http_client_set_timeout_ms(Client*,int);
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 0x7007
#ifdef __cplusplus
}
#endif
'''
    ports='''#include "port.h"
#include <errno.h>
#include <string.h>
#include <inttypes.h>
typedef int esp_log_level_t;
#define ESP_LOG_WARN 2
#define ESP_LOG_DEBUG 1
#define HTTP_EVENT_ERROR 3
#define ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN -1
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT 0
const char* TAG="sdk-port";
void HostLog(const char* tag,const char* format,...){(void)tag;(void)format;}
void LevelLog(int level,const char* tag,const char* format,...){(void)level;(void)tag;(void)format;}
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_LOG_LEVEL(...) LevelLog(__VA_ARGS__)
int64_t clock_us=0;
int socket_calls=0,set_timeout_error=0;
void esp_http_client_cached_buf_cleanup(Buffer* b){b->raw_len=0;}
bool esp_http_client_is_complete_data_received(Client* c){return c->is_chunk_complete;}
const char* esp_err_to_name(int e){(void)e;return "port";}
int esp_transport_translate_error(int e){return e;}
void* esp_transport_get_error_handle(void* t){return t;}
void http_dispatch_event(Client* c,int e,void* p,int n){(void)c;(void)e;(void)p;(void)n;}
void http_dispatch_event_to_event_loop(int e,void* p,int n){(void)e;(void)p;(void)n;}
int esp_http_client_set_timeout_ms(Client* c,int ms){if(set_timeout_error)return set_timeout_error;c->timeout_ms=ms;return ESP_OK;}
int esp_transport_read(void* transport,char* buffer,int bytes,int timeout){
 (void)transport;(void)bytes;
 if(socket_calls++==0){memset(buffer,'x',20);return 20;}
 clock_us+=(int64_t)timeout*1000;return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
}
int http_parser_execute(void* parser,void* settings,const char* data,int size){
 (void)settings;(void)data;Client* c=parser;
 c->response->buffer->raw_len+=size;c->response->data_process+=size;
 ConfigureCallback();return size;
}
'''
    program='''#include "port.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
struct StreamContext { int64_t deadline_us=120000000;const char* phase="read";bool fatal=false;std::string error_code,error_message; };
int64_t esp_timer_get_time(){return clock_us;}
bool CheckDeadline(StreamContext* c){if(clock_us>=c->deadline_us){c->fatal=true;return false;}return true;}
@@HELPER@@
StreamContext context;
Client client;
extern "C" void ConfigureCallback(){ApplyRemainingTimeout(&context,&client);}
int checks=0,failures=0;
void Check(bool v,const char* label){++checks;failures+=!v;std::printf("%s sdk-poll: %s\\n",v?"PASS":"FAIL",label);}
int main(){
 char raw[1024]={},out[1024]={};Buffer buffer{0,nullptr,raw,nullptr};Response response{&buffer,true,0,0};
 client={&response,false,1024,500,nullptr,nullptr,nullptr};client.parser=&client;
 const int count=esp_http_client_read(&client,out,sizeof(out));
 Check(count==20&&socket_calls==2,"actual SDK read continues after data callback to fill the caller buffer");
 Check(clock_us<=500000&&client.timeout_ms<=500,"callback never widens the caller read poll to the full turn budget");
 Check(clock_us<context.deadline_us,"ASR terminal handoff can resume before the full request budget expires");
 for(const char* phase:{"open","headers","upload"}){clock_us=0;context={};context.phase=phase;
  Check(ApplyRemainingTimeout(&context,&client)&&client.timeout_ms==120000,"startup/upload phase retains the absolute request budget");}
 clock_us=0;context={};context.deadline_us=200000;
 Check(ApplyRemainingTimeout(&context,&client)&&client.timeout_ms==200,"read poll still obeys a shorter remaining request budget");
 context={};set_timeout_error=ESP_FAIL;
 Check(!ApplyRemainingTimeout(&context,&client)&&context.fatal,"timeout setup failure is explicit and cannot continue as healthy I/O");
 std::printf("SDK stream poll: %d checks, %d failures (installed SDK read + production helper; parser/TCP/clock ports; NOT HIL)\\n",checks,failures);
 return failures?1:0;
}
'''.replace('@@HELPER@@',helper)
    with tempfile.TemporaryDirectory(prefix='wqn-sdk-poll-') as name:
        d=Path(name);(d/'port.h').write_text(header);(d/'sdk.c').write_text(ports+'\n'+reader);(d/'main.cpp').write_text(program)
        subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-c',str(d/'sdk.c'),'-o',str(d/'sdk.o')],check=True,timeout=30)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(d/'main.cpp'),str(d/'sdk.o'),'-o',str(d/'test')],check=True,timeout=30)
        return subprocess.run([str(d/'test')],timeout=10).returncode


if __name__=='__main__':raise SystemExit(main())
