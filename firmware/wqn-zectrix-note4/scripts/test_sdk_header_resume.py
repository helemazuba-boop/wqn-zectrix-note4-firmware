#!/usr/bin/env python3
"""Exercise installed SDK header fetch/callback and real HTTP parser across EAGAIN.

Only transport, client layout, header notification and logging are host ports.
No network request or hardware is used.
"""
import os
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function


def main():
    sdk = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5'))
    client = (sdk/'components/esp_http_client/esp_http_client.c').read_text()
    bodies = '\n'.join(function(client, signature) for signature in (
        'static int http_on_headers_complete(',
        'int64_t esp_http_client_fetch_headers(',
    ))
    ports = r'''
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "http_parser.h"
#define HTTP_METHOD_HEAD HTTP_HEAD
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT 0
enum { HTTP_STATE_REQ_COMPLETE_HEADER=1, HTTP_STATE_REQ_COMPLETE_DATA,
       HTTP_STATE_RES_COMPLETE_HEADER, HTTP_STATE_RES_ON_DATA_START };
typedef struct { int len; char* data; } esp_http_buffer_t;
typedef struct { esp_http_buffer_t* buffer; int status_code; uint32_t data_offset;
                 int64_t content_length,data_process; bool is_chunked; } Response;
typedef struct { int state,buffer_size_rx,timeout_ms; Response* response;
                 void* transport; http_parser* parser; http_parser_settings* parser_settings;
                 struct { int method; } connection_info; } Client;
typedef Client* esp_http_client_handle_t;
static const char* TAG="SDK-header-port";
static void HostLog(const char* tag,const char* fmt,...) { (void)tag;(void)fmt; }
#define ESP_LOGD(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
static void http_on_header_event(Client* c) { (void)c; }
static const char* packets[8];
static int packet_count,packet_cursor;
static int esp_transport_read(void* t,char* out,int cap,int timeout) {
    (void)t;(void)timeout;
    if(packet_cursor>=packet_count) return -1;
    const char* p=packets[packet_cursor++];
    if(!p) return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    const int n=(int)strlen(p);
    if(n>cap) return -1;
    memcpy(out,p,n);return n;
}
'''
    program = r'''
static int checks,failures;
static void Check(bool ok,const char* text) {
    ++checks;if(!ok) { ++failures;fprintf(stderr,"FAIL SDK header: %s\n",text); }
}
int main(void) {
    char data[512];esp_http_buffer_t buffer={0,data};Response response={0};
    http_parser parser;http_parser_settings settings={0};
    Client client={0};client.response=&response;response.buffer=&buffer;
    client.buffer_size_rx=sizeof(data);client.timeout_ms=500;client.parser=&parser;
    client.parser_settings=&settings;client.connection_info.method=HTTP_POST;
    settings.on_headers_complete=http_on_headers_complete;
    http_parser_init(&parser,HTTP_RESPONSE);parser.data=&client;
    client.state=HTTP_STATE_REQ_COMPLETE_HEADER;
    packets[0]="HTTP/1.1 200 OK\r\nContent-Len";packets[1]=NULL;
    packets[2]="gth: 3\r\nContent-Type: application/json\r\n\r\n";
    packet_count=3;
    Check(esp_http_client_fetch_headers(&client)==-ESP_ERR_HTTP_EAGAIN,
          "partial header followed by socket timeout is temporary waiting");
    Check(response.status_code==-1&&parser.nread>0,
          "actual parser retains its partial header before the next call");
    Check(esp_http_client_fetch_headers(&client)==3&&response.status_code==200,
          "same parser/client completes the split content length and status");
    Check(client.state==HTTP_STATE_RES_ON_DATA_START&&packet_cursor==3,
          "header resume reaches body state without resubmitting a request");
    http_parser_init(&parser,HTTP_RESPONSE);parser.data=&client;
    client.state=HTTP_STATE_REQ_COMPLETE_HEADER;packet_cursor=0;packet_count=1;
    packets[0]="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    response.is_chunked=false;
    Check(esp_http_client_fetch_headers(&client)==0&&response.status_code==200&&response.is_chunked,
          "a valid chunked response needs no positive content length");
    http_parser_init(&parser,HTTP_RESPONSE);parser.data=&client;
    client.state=HTTP_STATE_REQ_COMPLETE_HEADER;packet_cursor=packet_count=0;
    Check(esp_http_client_fetch_headers(&client)==ESP_FAIL,
          "permanent connection closure is not temporary waiting");
    client.state=0;
    Check(esp_http_client_fetch_headers(&client)==ESP_FAIL,
          "an unopened client cannot resume headers");
    printf("SDK header resume: %d checks, %d failures (installed SDK fetch/callback/parser; host transport; NOT HIL)\n",checks,failures);
    return failures?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='wqn-sdk-headers-') as temp:
        directory = Path(temp)
        unit = directory/'headers.c'
        unit.write_text(ports+'\n'+bodies+'\n'+program)
        binary = directory/'test'
        parser_dir = sdk/'components/http_parser'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(parser_dir), str(unit), str(parser_dir/'http_parser.c'),
                        '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=10).returncode


if __name__ == '__main__':
    raise SystemExit(main())
