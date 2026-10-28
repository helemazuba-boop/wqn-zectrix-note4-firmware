#!/usr/bin/env python3
"""Run the vendored receiver against partial-read, stop and empty-frame ports."""
from pathlib import Path
import subprocess
import tempfile
from test_audio_runtime import function
root=Path(__file__).resolve().parents[1]
source=(root/'components/esp_websocket_client/esp_websocket_client.c').read_text()
pre=r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <arpa/inet.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,REQUESTED_STOP_BIT=1,WEBSOCKET_EVENT_DATA=1;
constexpr int WEBSOCKET_CONTROL_PAYLOAD_MAX=125,WEBSOCKET_STATE_CLOSING=5;
constexpr int WS_TRANSPORT_OPCODES_NONE=-1,WS_TRANSPORT_OPCODES_PING=9,WS_TRANSPORT_OPCODES_PONG=10;
constexpr int WS_TRANSPORT_OPCODES_CLOSE=8,WS_TRANSPORT_OPCODES_FIN=128;
constexpr char TAG[]="ws-host";
template<class... T>void Log(T...){}
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGD(...) Log(__VA_ARGS__)
#define ESP_LOGV(...) Log(__VA_ARGS__)
struct TlsError { int last_error=0,esp_tls_error_code=0,esp_tls_flags=0; };
using esp_tls_error_handle_t=TlsError*;
struct Transport { std::vector<int> reads;size_t at=0;int total=8,opcode=1;bool fin=true; } transport;
struct Config { int network_timeout_ms=100; } config;
struct Client { int payload_offset=0,payload_len=0,last_opcode=0,buffer_size=8,status_bits=0,state=0,close_status_code=0;
 bool last_fin=false,wait_for_pong_resp=true;char rx_buffer[8]{};Transport* transport;Config* config; };
using esp_websocket_client_handle_t=Client*;
int freed=0,dispatches=0;bool stop_after_dispatch=false;
int xEventGroupGetBits(int bits){return bits;}
int esp_websocket_new_buf(Client*,bool){return ESP_OK;}
void esp_websocket_free_buf(Client*,bool){++freed;}
int esp_transport_read(Transport* t,char*,int,int){assert(t->at<t->reads.size());return t->reads[t->at++];}
int esp_transport_ws_get_read_payload_len(Transport* t){return t->total;}
bool esp_transport_ws_get_fin_flag(Transport* t){return t->fin;}
int esp_transport_ws_get_read_opcode(Transport* t){return t->opcode;}
TlsError* esp_transport_get_error_handle(Transport*){return nullptr;}
const char* esp_err_to_name(int){return "port";}
template<class... T> void esp_websocket_client_error(Client*,T...){}
void esp_websocket_client_dispatch_event(Client* c,int,const char*,int){++dispatches;if(stop_after_dispatch)c->status_bits|=REQUESTED_STOP_BIT;}
int esp_transport_ws_send_raw(Transport*,int,const char*,int size,int){return size;}
'''
main=r'''
int main(){
 Client client;client.transport=&transport;client.config=&config;
 transport.reads={4,0};assert(esp_websocket_client_recv(&client)==ESP_FAIL);
 assert(transport.at==2&&dispatches==1&&freed==1);
 transport.at=0;transport.reads={4,4};dispatches=freed=0;
 assert(esp_websocket_client_recv(&client)==ESP_OK&&dispatches==2&&freed==1);
 transport.at=0;dispatches=freed=0;stop_after_dispatch=true;
 assert(esp_websocket_client_recv(&client)==ESP_FAIL&&transport.at==1&&freed==1);
 client.status_bits=0;stop_after_dispatch=false;transport.at=0;transport.reads={0};transport.total=0;
 transport.opcode=WS_TRANSPORT_OPCODES_NONE;dispatches=freed=0;
 assert(esp_websocket_client_recv(&client)==ESP_OK&&dispatches==0&&freed==1);
 transport.at=0;transport.opcode=1;dispatches=freed=0;
 assert(esp_websocket_client_recv(&client)==ESP_OK&&dispatches==1&&freed==1);
 std::puts("Vendored WS receive: stalled partial, stop, normal fragmented and empty-frame controls PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='ws-partial-') as directory:
 folder=Path(directory);unit=folder/'receiver.cpp';binary=folder/'receiver'
 unit.write_text(pre+function(source,'static esp_err_t esp_websocket_client_recv(')+main)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(unit),'-o',str(binary)],check=True,timeout=60)
 subprocess.run([str(binary)],check=True,timeout=10)
