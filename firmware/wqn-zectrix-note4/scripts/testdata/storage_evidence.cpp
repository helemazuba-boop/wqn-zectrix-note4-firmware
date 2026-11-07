// Real collector TU + libc files, typed NVS ports and OpenSSL SHA adapter.
// No flash, physical serial, credentials or business services are accessed.
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "runtime/storage_evidence.h"
#include "storage.h"
#include "nvs.h"
#include "esp_heap_caps.h"

std::string root, scenario, current_name;
bool owner=true, mounted=true, temp_lease=false, pack_lease=false, cancel=false, namespace_present=true;
bool mutated=false, ready=false;
uint32_t generation=7;
int64_t now=1000000;
int open_error=0, write_opens=0, forbidden_keys=0, external_outputs=0, source_io=0, yields=0;
std::map<void*,std::pair<size_t,unsigned>> allocations;
std::map<int,std::string> descriptors;
struct Value { nvs_type_t type; std::vector<uint8_t> bytes; int error=0; };
std::map<std::string,Value> values;
bool Allowed(const char* key) {
    static const std::vector<std::string> names={"cur_seq","cur_rnd","cur_dic","cur_rev","cur_int","cur_shf",
        "cur_mis","word_deck_gen","word_deck_chg","word_deck","word_seq_idx"};
    return std::find(names.begin(),names.end(),key)!=names.end();
}
void Destination(const void* pointer) {
    const auto at=reinterpret_cast<uintptr_t>(pointer);
    for(const auto& pair:allocations) {
        const auto start=reinterpret_cast<uintptr_t>(pair.first);
        if(at>=start && at<start+pair.second.first && (pair.second.second&MALLOC_CAP_SPIRAM))++external_outputs;
    }
}
void* heap_caps_malloc(size_t size,unsigned caps) {
    if(scenario=="oom")return nullptr;
    void* p=std::malloc(size);if(p)allocations[p]={size,caps};return p;
}
void* heap_caps_calloc(size_t count,size_t size,unsigned caps) {
    void* p=heap_caps_malloc(count*size,caps);if(p)std::memset(p,0,count*size);return p;
}
void heap_caps_free(void* p) {allocations.erase(p);std::free(p);}
namespace wqn::services {bool IsStorageServiceTask(){return owner;}}
namespace wqn::runtime {
bool HasActiveStorageTempFileLease(){return temp_lease;}
bool HasActivePackStreamPath(){return pack_lease;}
}
namespace wqn {
uint32_t GetDeckScopeGeneration(){return generation;}
bool IsWordDeckScopeReady(){return ready;}
}
bool esp_spiffs_mounted(const char* label){return mounted && std::strcmp(label,"storage")==0;}
int64_t esp_timer_get_time(){now+=1000;return now;}
void vTaskDelay(unsigned){
    ++yields;
    if(scenario=="deadline-during-yield")now=500000000;
    if(scenario=="cancel-during-yield")cancel=true;
}

esp_err_t nvs_open(const char* name,int mode,nvs_handle_t* out) {
    ++source_io;
    if(mode!=NVS_READONLY)++write_opens;
    if(std::strcmp(name,"evidence_test")!=0)return ESP_FAIL;
    if(open_error)return open_error;
    if(!namespace_present)return ESP_ERR_NVS_NOT_FOUND;
    *out=1;return ESP_OK;
}
void nvs_close(nvs_handle_t){}
esp_err_t nvs_find_key(nvs_handle_t,const char* key,nvs_type_t* type) {
    ++source_io;Destination(type);if(!Allowed(key))++forbidden_keys;
    const auto found=values.find(key);if(found==values.end())return ESP_ERR_NVS_NOT_FOUND;
    *type=found->second.type;return ESP_OK;
}
template<typename T> int Get(const char* key,T* out,nvs_type_t type) {
    ++source_io;Destination(out);if(!Allowed(key))++forbidden_keys;
    const auto found=values.find(key);if(found==values.end())return ESP_ERR_NVS_NOT_FOUND;
    if(found->second.type!=type)return ESP_ERR_NVS_TYPE_MISMATCH;
    if(found->second.error)return found->second.error;
    uint64_t bits=0;for(size_t i=0;i<sizeof(T);++i)bits|=uint64_t(found->second.bytes.at(i))<<(8*i);
    *out=static_cast<T>(bits);return ESP_OK;
}
#define GETTER(name,type,tag) esp_err_t nvs_get_##name(nvs_handle_t,const char* key,type* out){return Get(key,out,tag);}
GETTER(u8,uint8_t,NVS_TYPE_U8) GETTER(i8,int8_t,NVS_TYPE_I8)
GETTER(u16,uint16_t,NVS_TYPE_U16) GETTER(i16,int16_t,NVS_TYPE_I16)
GETTER(u32,uint32_t,NVS_TYPE_U32) GETTER(i32,int32_t,NVS_TYPE_I32)
GETTER(u64,uint64_t,NVS_TYPE_U64) GETTER(i64,int64_t,NVS_TYPE_I64)
esp_err_t Buffer(const char* key,void* out,size_t* size,nvs_type_t type) {
    ++source_io;Destination(out);Destination(size);if(!Allowed(key))++forbidden_keys;
    const auto found=values.find(key);if(found==values.end())return ESP_ERR_NVS_NOT_FOUND;
    const auto& value=found->second;
    if(value.type!=type)return ESP_ERR_NVS_TYPE_MISMATCH;
    if(value.error)return value.error;
    if(!out){*size=value.bytes.size();return ESP_OK;}
    if(*size<value.bytes.size())return ESP_ERR_INVALID_SIZE;
    if(!value.bytes.empty())std::memcpy(out,value.bytes.data(),value.bytes.size());
    *size=value.bytes.size();return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size){return Buffer(key,out,size,NVS_TYPE_BLOB);}
esp_err_t nvs_get_str(nvs_handle_t,const char* key,char* out,size_t* size){return Buffer(key,out,size,NVS_TYPE_STR);}
extern "C" {
int __real_open(const char*,int,...);
int __real_close(int);
ssize_t __real_read(int,void*,size_t);
int __real_lstat(const char*,struct stat*);
DIR* __real_opendir(const char*);
}
std::string Map(const char* path) {
    if(std::strcmp(path,"/storage")==0)return root;
    if(std::strncmp(path,"/storage/",9)==0)return root+(path+8);
    return path;
}
extern "C" int __wrap_open(const char* path,int flags,...) {
    const auto mapped=Map(path);
    if(mapped!=path){++source_io;if((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC)))++write_opens;}
    const int fd=__real_open(mapped.c_str(),flags,0600);
    if(fd>=0 && mapped!=path)descriptors[fd]=mapped;
    return fd;
}
extern "C" int __wrap_close(int fd){descriptors.erase(fd);return __real_close(fd);}
extern "C" ssize_t __wrap_read(int fd,void* out,size_t size) {
    if(descriptors.count(fd)){++source_io;Destination(out);}
    if(scenario=="read-error" && descriptors.count(fd) && descriptors[fd]==root+"/wout.v1") {
        errno=EIO;return -1;
    }
    return __real_read(fd,out,size);
}
extern "C" int __wrap_lstat(const char* path,struct stat* out) {
    ++source_io;Destination(out);return __real_lstat(Map(path).c_str(),out);
}
extern "C" DIR* __wrap_opendir(const char* path){++source_io;return __real_opendir(Map(path).c_str());}

uint64_t Number(const uint8_t* p,size_t size){uint64_t n=0;for(size_t i=0;i<size;++i)n|=uint64_t(p[i])<<(8*i);return n;}
std::ofstream output;
bool Cancel(void*){return cancel;}
esp_err_t Emit(const uint8_t* data,size_t size,void*) {
    const unsigned kind=data[4];
    if(scenario=="sink-error" && kind==3)return ESP_FAIL;
    output.write(reinterpret_cast<const char*>(data),size);
    if(kind==2)current_name.assign(reinterpret_cast<const char*>(data+11+18),data[11+17]);
    if(kind==3 && scenario=="cancel-mid")cancel=true;
    if(kind==4 && !mutated) {
        if(scenario=="drift-file" && current_name=="wout.v1") {
            std::fstream file(root+"/wout.v1",std::ios::in|std::ios::out|std::ios::binary);
            file.put('X');mutated=true;  // Same length, intentionally unchanged stat precision.
        }
        if(scenario=="drift-inventory" && current_name=="wout.v1") {
            std::ofstream file(root+"/wl-new.future");file<<"retained";mutated=true;
        }
        if(scenario=="drift-nvs" && current_name=="cur_seq") {
            values["cur_seq"].bytes[0]^=0x40;mutated=true;
        }
        if(scenario=="drift-context" && current_name=="cur_seq") {++generation;mutated=true;}
    }
    if(scenario=="late-sink-error" && kind==5)return ESP_FAIL;
    return output.good()?ESP_OK:ESP_FAIL;
}
void Num(const char* key,nvs_type_t type,uint64_t n,size_t size) {
    Value v{type,{}};for(size_t i=0;i<size;++i)v.bytes.push_back(n>>(8*i));values[key]=v;
}
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    root=argv[1];scenario=argv[2];output.open(argv[3],std::ios::binary);
    values["cur_seq"]={NVS_TYPE_BLOB,std::vector<uint8_t>(65,0xA5)};
    Num("cur_rnd",NVS_TYPE_I8,UINT64_C(0xfb),1);
    Num("cur_dic",NVS_TYPE_U8,255,1);
    Num("cur_rev",NVS_TYPE_I16,UINT64_C(0xfed4),2);
    Num("cur_int",NVS_TYPE_U16,65535,2);
    Num("cur_shf",NVS_TYPE_I32,UINT64_C(0xfffe1dc0),4);
    Num("cur_mis",NVS_TYPE_U32,4000000000,4);
    Num("word_deck_gen",NVS_TYPE_U64,(UINT64_C(1)<<60)+7,8);
    values["word_deck_chg"]={NVS_TYPE_STR,{'b','a','d',0,'t','a','i','l',0}};
    const std::string uuid="11111111-2222-3333-4444-555555555555";
    values["word_deck"]={NVS_TYPE_STR,{uuid.begin(),uuid.end()}};values["word_deck"].bytes.push_back(0);
    Num("word_seq_idx",NVS_TYPE_I64,UINT64_C(0xf000000000000001),8);
    values["wifi_creds"]={NVS_TYPE_BLOB,{'D','O','_','N','O','T','_','E','X','P','O','R','T','_','C','R','E','D','E','N','T','I','A','L'}};
    if(scenario=="off-owner")owner=false;
    if(scenario=="unmounted")mounted=false;
    if(scenario=="temp-lease")temp_lease=true;
    if(scenario=="pack-lease")pack_lease=true;
    if(scenario=="cancel-early")cancel=true;
    if(scenario=="namespace-missing")namespace_present=false;
    if(scenario=="namespace-error")open_error=ESP_FAIL;
    if(scenario=="nvs-error")values["cur_seq"].error=ESP_FAIL;
    if(scenario=="nvs-oversize")values["cur_seq"].bytes.resize(65537);
    if(scenario=="nvs-unsupported")values["cur_seq"].type=NVS_TYPE_ANY;
    if(scenario=="nvs-empty")values["cur_seq"].bytes.clear();
    if(scenario=="key-missing")values.erase("cur_seq");
    if(scenario=="manual-preservation")wqn::runtime::EnterStorageEvidenceBootPreservation();
    else if(scenario!="not-mode")wqn::runtime::EnterStorageEvidenceBootRecovery(ESP_ERR_INVALID_CRC,3);
    wqn::runtime::StorageEvidenceIdentity identity;
    identity.capture_id.fill(0x11);identity.mac.fill(0x22);identity.firmware_elf_sha256.fill(0x33);
    wqn::runtime::StorageEvidenceSink sink{Emit,nullptr,Cancel,nullptr,500000000};
    if(scenario=="deadline")sink.deadline_us=1;
    wqn::runtime::StorageEvidenceSummary summary;
    const int error=wqn::runtime::CaptureStorageEvidenceOnOwner(identity,sink,&summary);
    output.close();
    const bool success=scenario=="normal" || scenario=="namespace-missing" || scenario=="key-missing" || scenario=="nvs-empty";
    const bool early=scenario=="manual-preservation" || scenario=="not-mode" || scenario=="off-owner" || scenario=="unmounted" ||
        scenario=="temp-lease" || scenario=="pack-lease" || scenario=="cancel-early" || scenario=="deadline" ||
        scenario=="deadline-during-yield" || scenario=="cancel-during-yield";
    const bool expected=success?(error==ESP_OK && summary.capture_complete):(error!=ESP_OK && !summary.capture_complete);
    const bool readonly=write_opens==0 && forbidden_keys==0 && external_outputs==0 && allocations.empty();
    const bool no_early_io=!early || (source_io==0 && summary.frames==0);
    const bool passed=expected && readonly && no_early_io;
    std::printf("{\"passed\":%s,\"expected_outcome\":%s,\"readonly\":%s,\"early_no_io\":%s,"
        "\"error\":%d,\"frames\":%u,\"entities\":%u,\"bytes\":%llu,\"complete\":%s,\"yields\":%d}\n",
        passed?"true":"false",expected?"true":"false",readonly?"true":"false",no_early_io?"true":"false",
        error,summary.frames,summary.entities,(unsigned long long)summary.data_bytes,summary.capture_complete?"true":"false",yields);
    return passed?0:1;
}
