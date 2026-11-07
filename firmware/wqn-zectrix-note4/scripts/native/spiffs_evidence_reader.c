// Read-only SDK SPIFFS reader for a pinned offline image. No host path from
// an image is opened: only the inherited O_RDONLY fd and stdout are used.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "spiffs.h"
#include "spiffs_nucleus.h"

#if !SPIFFS_READ_ONLY
#error The offline reader requires SDK SPIFFS_READ_ONLY
#endif
#if SPIFFS_OBJ_NAME_LEN != 32 || SPIFFS_OBJ_META_LEN != 4
#error Unsupported evidence-reader disk format
#endif
_Static_assert(sizeof(spiffs_page_ix)==2,"disk page indices remain 16 bit");
_Static_assert(_Alignof(spiffs_page_ix)==1,"host disk indices require byte-aligned loads");
_Static_assert(sizeof(spiffs_page_header)==5,"disk page header layout differs");
_Static_assert(offsetof(spiffs_page_object_ix_header,name)==13,"disk name offset differs");
_Static_assert(sizeof(spiffs_page_object_ix_header)==49,"disk object index layout differs");

static int source_fd;
static uint32_t source_offset, source_bytes;
static unsigned reads, write_attempts, erase_attempts, denied;
void spiffs_api_lock(struct spiffs_t *fs) { (void)fs; }
void spiffs_api_unlock(struct spiffs_t *fs) { (void)fs; }

static int error(const char *phase, int code) {
    fprintf(stderr,"SPIFFS_EVIDENCE_%s:%d\n",phase,code); return 1;
}
static int sandbox(int fd) {
#if defined(__x86_64__)
#define HOST_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define HOST_ARCH AUDIT_ARCH_AARCH64
#else
    (void)fd; errno=ENOTSUP; return -1;
#endif
#ifdef HOST_ARCH
#define ALLOW(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(n),0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    const struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,HOST_ARCH,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        // Writes only to evidence stdout/error stderr; never the source fd.
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_write,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,1,2,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,2,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_pread64,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(unsigned)fd,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        ALLOW(__NR_brk), ALLOW(__NR_mmap), ALLOW(__NR_munmap), ALLOW(__NR_mprotect),
        ALLOW(__NR_close), ALLOW(__NR_futex), ALLOW(__NR_clock_gettime),
        ALLOW(__NR_rt_sigreturn), ALLOW(__NR_rt_sigprocmask), ALLOW(__NR_rt_sigaction),
        ALLOW(__NR_exit), ALLOW(__NR_exit_group),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
    };
    struct sock_fprog program={(unsigned short)(sizeof(code)/sizeof(code[0])),(struct sock_filter*)code};
    return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program) ? -1 : 0;
#undef ALLOW
#endif
}
static int check_sandbox(void) {
    errno=0;
    if (pwrite(source_fd,"x",1,0)!=-1 || errno!=EPERM) return -1;
    ++denied;
    errno=0;
    if (syscall(SYS_openat,AT_FDCWD,"/dev/null",O_WRONLY,0)!=-1 || errno!=EPERM) return -1;
    ++denied;
    errno=0;
    if (socket(AF_INET,SOCK_STREAM,0)!=-1 || errno!=EPERM) return -1;
    ++denied;
    return 0;
}
static s32_t read_flash(spiffs *fs,u32_t address,u32_t size,u8_t *buffer) {
    (void)fs;++reads;
    if(address>source_bytes || size>source_bytes-address)return SPIFFS_ERR_NOT_READABLE;
    size_t done=0;
    while(done<size){
        ssize_t n=pread(source_fd,buffer+done,size-done,(off_t)source_offset+address+done);
        if(n<=0)return SPIFFS_ERR_NOT_READABLE;
        done+=(size_t)n;
    }
    return SPIFFS_OK;
}
static s32_t refuse_write(spiffs *fs,u32_t address,u32_t size,u8_t *data) {
    (void)fs;(void)address;(void)size;(void)data;++write_attempts;return SPIFFS_ERR_RO_NOT_IMPL;
}
static s32_t refuse_erase(spiffs *fs,u32_t address,u32_t size) {
    (void)fs;(void)address;(void)size;++erase_attempts;return SPIFFS_ERR_RO_NOT_IMPL;
}
static int bytes(const void *data,size_t size){return fwrite(data,1,size,stdout)==size?0:-1;}
static int number(uint32_t value){
    unsigned char data[4]={value,value>>8,value>>16,value>>24};return bytes(data,4);
}
static int parse(const char *text,uint32_t *value) {
    if(*text<'0'||*text>'9')return -1;
    char *tail;errno=0;unsigned long long n=strtoull(text,&tail,10);
    if(errno || *tail || n>UINT32_MAX)return -1;
    *value=(uint32_t)n;return 0;
}
int main(int argc,char **argv) {
    uint32_t fd=0;
    if(argc!=4 || parse(argv[1],&fd) || parse(argv[2],&source_offset) || parse(argv[3],&source_bytes) ||
       fd<3 || fd>INT_MAX || source_bytes==0 || source_bytes>16U*1024*1024 || source_bytes%4096 || source_offset%4096)
        return error("ARGUMENT",-1);
    source_fd=(int)fd;struct stat st;
    if(fstat(source_fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0 ||
       (uint64_t)source_offset+source_bytes>(uint64_t)st.st_size ||
       (fcntl(source_fd,F_GETFL)&O_ACCMODE)!=O_RDONLY)return error("SOURCE",-1);
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    if(sandbox(source_fd) || check_sandbox())return error("SANDBOX",errno);
    spiffs fs;memset(&fs,0,sizeof(fs));
    spiffs_config cfg;memset(&cfg,0,sizeof(cfg));
    cfg.phys_size=source_bytes;cfg.phys_addr=0;cfg.phys_erase_block=4096;
    cfg.log_block_size=4096;cfg.log_page_size=CONFIG_SPIFFS_PAGE_SIZE;
    cfg.hal_read_f=read_flash;cfg.hal_write_f=refuse_write;cfg.hal_erase_f=refuse_erase;
    u8_t work[2*CONFIG_SPIFFS_PAGE_SIZE], descriptors[4096];
    s32_t status=SPIFFS_mount(&fs,&cfg,work,descriptors,sizeof(descriptors),NULL,0,NULL);
    if(status!=SPIFFS_OK)return error("MOUNT",status);
    // SDK RO build must reject mutation before calling any media callback.
    status=SPIFFS_creat(&fs,"/__reader_ro_selftest",0);
    if(status!=SPIFFS_ERR_RO_NOT_IMPL || write_attempts || erase_attempts)return error("READONLY",status);
    SPIFFS_clearerr(&fs);
    if(bytes("WQSP1\n",6))return error("OUTPUT",-1);
    spiffs_DIR directory;struct spiffs_dirent entry;
    if(SPIFFS_opendir(&fs,"/",&directory)==NULL)return error("DIRECTORY",SPIFFS_errno(&fs));
    uint8_t buffer[4096], seen[8192]={0};uint32_t count=0,total=0;
    while(SPIFFS_readdir(&directory,&entry)!=NULL){
        if(++count>32767 || entry.obj_id==0 || entry.type!=SPIFFS_TYPE_FILE || entry.size>16U*1024*1024 ||
           entry.size>32U*1024*1024-total || (seen[entry.obj_id/8]&(1U<<(entry.obj_id%8))))return error("INVENTORY",-1);
        seen[entry.obj_id/8]|=1U<<(entry.obj_id%8);total+=entry.size;
        spiffs_page_object_ix_header original;
        if(read_flash(&fs,(uint32_t)entry.pix*CONFIG_SPIFFS_PAGE_SIZE,sizeof(original),(u8_t*)&original)!=SPIFFS_OK ||
           original.p_hdr.obj_id!=(entry.obj_id|SPIFFS_OBJ_ID_IX_FLAG) || original.p_hdr.span_ix!=0 ||
           original.type!=entry.type || (original.size==SPIFFS_UNDEFINED_LEN?0:original.size)!=entry.size)
            return error("RAW_HEADER",-1);
        spiffs_file file=SPIFFS_open_by_dirent(&fs,&entry,SPIFFS_RDONLY,0);
        if(file<0)return error("OPEN",SPIFFS_errno(&fs));
        spiffs_stat actual;
        if(SPIFFS_fstat(&fs,file,&actual)!=SPIFFS_OK || actual.size!=entry.size || actual.obj_id!=entry.obj_id)
            return error("IDENTITY",-1);
        uint8_t kind=1;
        if(bytes(&kind,1)||number(count)||number(entry.obj_id)||number(entry.size)||number(entry.pix)||
           bytes(original.name,sizeof(original.name))||bytes(original.meta,sizeof(original.meta)))return error("OUTPUT",-1);
        uint32_t remaining=entry.size;
        while(remaining){
            s32_t n=SPIFFS_read(&fs,file,buffer,remaining>sizeof(buffer)?sizeof(buffer):remaining);
            if(n<=0 || (uint32_t)n>remaining)return error("READ",n);
            if(bytes(buffer,(size_t)n))return error("OUTPUT",-1);
            remaining-=(uint32_t)n;
        }
        if(SPIFFS_read(&fs,file,buffer,1)!=0 || SPIFFS_close(&fs,file)!=SPIFFS_OK)return error("FILE_END",-1);
        SPIFFS_clearerr(&fs);
    }
    if(SPIFFS_errno(&fs)!=SPIFFS_OK || SPIFFS_closedir(&directory)!=SPIFFS_OK)return error("DIRECTORY_END",SPIFFS_errno(&fs));
    SPIFFS_unmount(&fs);
    if(write_attempts || erase_attempts)return error("MEDIA_WRITE",-1);
    uint8_t end=0;
    if(bytes(&end,1)||number(count)||number(reads)||number(write_attempts)||number(erase_attempts)||number(denied))return error("OUTPUT",-1);
    return 0;
}
