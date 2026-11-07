// Offline NVS interpretation using the actual SDK. The source fd is read-only,
// exactly 16 KiB and closed before the SDK runs. The SDK sees an immutable RAM
// partition; every attempted repair/write/erase is counted and refused.
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <set>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include "nvs_storage.hpp"

namespace {
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__,"NVS host requires little endian");
static_assert(sizeof(nvs::Item)==32 && offsetof(nvs::Item,key)==8 && offsetof(nvs::Item,data)==24,
    "NVS entry layout differs");
constexpr size_t kBytes=16384, kPages=4;
constexpr const char* kKeys[]={"cur_seq","cur_rnd","cur_dic","cur_rev","cur_int","cur_shf","cur_mis",
    "word_deck_gen","word_deck_chg","word_deck","word_seq_idx"};
constexpr nvs::ItemType kTypes[]={nvs::ItemType::BLOB,nvs::ItemType::BLOB,nvs::ItemType::BLOB,
    nvs::ItemType::BLOB,nvs::ItemType::BLOB,nvs::ItemType::BLOB,nvs::ItemType::BLOB,
    nvs::ItemType::U64,nvs::ItemType::SZ,nvs::ItemType::SZ,nvs::ItemType::U64};
struct Entry { nvs::Item item; size_t page, index; };
struct Value { esp_err_t error=ESP_ERR_NVS_NOT_FOUND; bool ambiguous=false; unsigned parents=0;
    std::vector<uint8_t> bytes; std::vector<Entry> entries; };

class Partition final : public nvs::Partition {
public:
    std::array<uint8_t,kBytes> data{};
    unsigned reads=0,writes=0,erases=0;
    const char* get_partition_name() override { return "nvs"; }
    uint32_t get_address() override { return 0; }
    uint32_t get_size() override { return kBytes; }
    bool get_readonly() override { return true; }
    esp_err_t read(size_t at,void* out,size_t size) override {
        ++reads;
        if(at>data.size() || size>data.size()-at)return ESP_ERR_INVALID_SIZE;
        std::memcpy(out,data.data()+at,size);return ESP_OK;
    }
    esp_err_t read_raw(size_t at,void* out,size_t size) override { return read(at,out,size); }
    esp_err_t write(size_t,const void*,size_t) override { ++writes;return ESP_ERR_NVS_READ_ONLY; }
    esp_err_t write_raw(size_t at,const void* in,size_t size) override { return write(at,in,size); }
    esp_err_t erase_range(size_t,size_t) override { ++erases;return ESP_ERR_NVS_READ_ONLY; }
};

// UBSan's first virtual-type lookup may consult process mappings. Validate the
// immutable RAM adapter before seccomp; subsequent SDK calls use the same base
// type/vptr cache. Do not weaken the filter or disable virtual-type checks.
__attribute__((noipa)) bool readonly_adapter(nvs::Partition* partition) {
    uint8_t byte=0;
    return partition->get_readonly() && partition->get_size()==kBytes && partition->read_raw(0,&byte,1)==ESP_OK;
}

int fail(const char* phase,int code) { std::fprintf(stderr,"NVS_CONTEXT_%s:%d\n",phase,code);return 1; }
int sandbox() {
#if defined(__x86_64__)
    constexpr unsigned arch=AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr unsigned arch=AUDIT_ARCH_AARCH64;
#else
    errno=ENOTSUP;return -1;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
#define ALLOW(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(n),0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    const sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,arch,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_write,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,1,2,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,2,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        ALLOW(__NR_brk),ALLOW(__NR_mmap),ALLOW(__NR_munmap),ALLOW(__NR_mprotect),ALLOW(__NR_close),
        ALLOW(__NR_futex),ALLOW(__NR_clock_gettime),ALLOW(__NR_rt_sigreturn),ALLOW(__NR_rt_sigaction),
        ALLOW(__NR_rt_sigprocmask),ALLOW(__NR_exit),ALLOW(__NR_exit_group),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(code)/sizeof(code[0])),const_cast<sock_filter*>(code)};
    return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program) ? -1 : 0;
#undef ALLOW
#endif
}
bool key(const nvs::Item& item,const char* wanted) {
    const auto* end=static_cast<const char*>(std::memchr(item.key,0,sizeof(item.key)));
    return end!=nullptr && std::string(item.key,end)==wanted;
}
void hex(const uint8_t* data,size_t bytes) { for(size_t i=0;i<bytes;++i)std::printf("%02x",data[i]); }
}

int main(int argc,char** argv) {
    if(argc!=2 || argv[1][0]<'0' || argv[1][0]>'9')return fail("ARGUMENT",-1);
    char* tail=nullptr;errno=0;long incoming=std::strtol(argv[1],&tail,10);
    if(errno || *tail || incoming<3 || incoming>INT32_MAX)return fail("ARGUMENT",-1);
    const int fd=static_cast<int>(incoming);struct stat st{};
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=kBytes ||
        (fcntl(fd,F_GETFL)&O_ACCMODE)!=O_RDONLY)return fail("SOURCE",-1);
    Partition partition;
    for(size_t at=0;at<kBytes;){
        ssize_t n=pread(fd,partition.data.data()+at,kBytes-at,static_cast<off_t>(at));
        if(n<=0)return fail("SOURCE",-1);
        at+=static_cast<size_t>(n);
    }
    close(fd);setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
    if(!readonly_adapter(&partition))return fail("ADAPTER",-1);
    if(sandbox())return fail("SANDBOX",errno);
    unsigned denials=0;
    errno=0;if(pwrite(fd,"x",1,0)!=-1 || errno!=EPERM)return fail("SANDBOX_WRITE",errno);++denials;
    errno=0;if(syscall(SYS_openat,AT_FDCWD,"/dev/null",O_WRONLY,0)!=-1 || errno!=EPERM)return fail("SANDBOX_OPEN",errno);++denials;
    errno=0;if(socket(AF_INET,SOCK_STREAM,0)!=-1 || errno!=EPERM)return fail("SANDBOX_NETWORK",errno);++denials;

    // SDK may normally discard corrupt pages or pick the first duplicate.
    // Record these ambiguities before asking Storage for interpreted values.
    unsigned flags=0;std::set<uint32_t> sequences;
    std::array<nvs::Page,kPages> pages;
    std::array<esp_err_t,kPages> loads{};
    std::vector<Entry> entries;
    for(size_t p=0;p<kPages;++p){
        loads[p]=pages[p].load(&partition,p);
        auto state=pages[p].state();
        if(loads[p]!=ESP_OK || (state!=nvs::Page::PageState::UNINITIALIZED &&
            state!=nvs::Page::PageState::ACTIVE && state!=nvs::Page::PageState::FULL))flags|=1;
        uint32_t seq=0;
        if(pages[p].getSeqNumber(seq)==ESP_OK && !sequences.insert(seq).second)flags|=2;
        // ILLEGAL bitmap entries must not silently disappear. Check all slots,
        // including data continuations and the two reserved bitmap bits.
        for(size_t i=0;i<126;++i){
            unsigned bits=(partition.data[p*4096+32+i/4]>>(2*(i%4)))&3;
            if(bits==1)flags|=4;
            if(bits==3){
                const auto* begin=partition.data.data()+p*4096+64+i*32;
                if(std::any_of(begin,begin+32,[](uint8_t byte){return byte!=255;}))flags|=4;
            }
        }
        if(state!=nvs::Page::PageState::UNINITIALIZED){
            const auto* header=partition.data.data()+p*4096;
            if((header[8]!=0xfe && header[8]!=0xff) ||
                std::any_of(header+9,header+28,[](uint8_t byte){return byte!=255;}))flags|=1;
        }
        if((partition.data[p*4096+63]&0xf0)!=0xf0)flags|=4;
        size_t at=0;nvs::Item item;
        esp_err_t err;
        while((err=pages[p].findItem(nvs::Page::NS_ANY,nvs::ItemType::ANY,nullptr,at,item))==ESP_OK){
            if(item.span==0 || item.span>126-at || entries.size()>=504)return fail("INVENTORY",-1);
            entries.push_back(Entry{item,p,at});at+=item.span;
        }
        if(err!=ESP_ERR_NVS_NOT_FOUND)flags|=8;
    }
    std::set<std::string> namespace_names;std::set<unsigned> namespace_ids;
    unsigned wanted_count=0;uint8_t ns=0;
    for(const auto& entry:entries)if(entry.item.nsIndex==0){
        const auto& item=entry.item;
        if(item.datatype!=nvs::ItemType::U8 || std::memchr(item.key,0,sizeof(item.key))==nullptr ||
            item.data[0]==0 || item.data[0]==255){flags|=16;continue;}
        if(!namespace_names.insert(item.key).second || !namespace_ids.insert(item.data[0]).second)flags|=16;
        if(key(item,"wqn")){++wanted_count;ns=item.data[0];}
    }
    for(const auto& entry:entries){
        if(entry.item.key[0]==0 || std::memchr(entry.item.key,0,sizeof(entry.item.key))==nullptr ||
            (entry.item.nsIndex!=0 && namespace_ids.count(entry.item.nsIndex)==0))flags|=16;
    }
    nvs::Storage storage(&partition);
    esp_err_t initialized=storage.init(0,kPages);
    uint8_t opened=0;
    esp_err_t namespace_error=initialized==ESP_OK?storage.createOrOpenNamespace("wqn",false,opened):initialized;
    if(wanted_count>1 || (namespace_error==ESP_OK && (wanted_count!=1 || opened!=ns)) ||
       (namespace_error==ESP_ERR_NVS_NOT_FOUND && wanted_count!=0))flags|=16;
    // A namespace alias makes the owner of selected key names unknowable.
    // Keep only the raw partition, never serialize another namespace's values.
    if((flags&16)!=0)namespace_error=ESP_ERR_NVS_INVALID_STATE;
    std::array<Value,11> values;
    for(size_t i=0;i<values.size();++i){
        auto& value=values[i];std::set<unsigned> chunks;
        if(wanted_count==1 && (flags&16)==0)for(const auto& entry:entries)if(entry.item.nsIndex==ns && key(entry.item,kKeys[i])){
            value.entries.push_back(entry);
            if(entry.item.datatype==nvs::ItemType::BLOB_DATA){
                if(!chunks.insert(entry.item.chunkIndex).second)value.ambiguous=true;
            }else ++value.parents;
        }
        if(value.parents>1 || (!value.entries.empty() && value.parents!=1))value.ambiguous=true;
        if(value.ambiguous)flags|=32;
        value.error=namespace_error;
        if(namespace_error!=ESP_OK)continue;
        if(value.ambiguous){value.error=ESP_ERR_NVS_INVALID_STATE;continue;}
        size_t size=8;
        value.error=kTypes[i]==nvs::ItemType::U64?ESP_OK:storage.getItemDataSize(ns,kTypes[i],kKeys[i],size);
        if(value.error==ESP_OK){
            if(size>kBytes){value.error=ESP_ERR_INVALID_SIZE;continue;}
            value.bytes.resize(size);
            value.error=storage.readItem(ns,kTypes[i],kKeys[i],value.bytes.data(),size);
            if(value.error!=ESP_OK)value.bytes.clear();
        }
        if((value.error==ESP_ERR_NVS_NOT_FOUND)!=(value.entries.empty()))flags|=64;
    }
    if(partition.writes || partition.erases)flags|=128;
    std::printf("{\"format\":\"wqn-native-nvs-context-v1\",\"flags\":%u,\"sdk_init_error\":%d,\"namespace_error\":%d,\"read_callbacks\":%u,\"write_attempts\":%u,\"erase_attempts\":%u,\"sandbox_denials\":%u,\"pages\":[",
        flags,initialized,namespace_error,partition.reads,partition.writes,partition.erases,denials);
    for(size_t p=0;p<kPages;++p){uint32_t seq=UINT32_MAX;pages[p].getSeqNumber(seq);
        std::printf("%s{\"index\":%zu,\"state\":%u,\"sequence\":%u,\"load_error\":%d}",p?",":"",p,static_cast<uint32_t>(pages[p].state()),seq,loads[p]);}
    std::printf("],\"values\":[");
    for(size_t i=0;i<values.size();++i){const auto& value=values[i];
        std::printf("%s{\"key\":\"%s\",\"sdk_error\":%d,\"parents\":%u,\"ambiguous\":%s,\"raw_hex\":\"",i?",":"",kKeys[i],value.error,value.parents,value.ambiguous?"true":"false");
        hex(value.bytes.data(),value.bytes.size());std::printf("\",\"locations\":[");
        for(size_t j=0;j<value.entries.size();++j){const auto& entry=value.entries[j];
            std::printf("%s{\"page\":%zu,\"entry\":%zu,\"type\":%u,\"chunk\":%u}",j?",":"",entry.page,entry.index,static_cast<unsigned>(entry.item.datatype),entry.item.chunkIndex);}
        std::printf("]}");
    }
    std::printf("]}\n");return std::ferror(stdout)?fail("OUTPUT",-1):0;
}
