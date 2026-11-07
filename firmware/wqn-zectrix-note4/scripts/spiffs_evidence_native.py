"""Build and run the SDK's read-only SPIFFS core in a constrained Linux child.

SPIFFS_READ_ONLY=1 removes mutation code. The packed disk page-index type is
given byte alignment/alias semantics for host C loads without changing its
two-byte width or disk layout. The shared SDK is never edited.
"""
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import subprocess

FILES=('include/spiffs_config.h','spiffs/src/spiffs.h','spiffs/src/spiffs_nucleus.h',
       'spiffs/src/spiffs_cache.c','spiffs/src/spiffs_check.c','spiffs/src/spiffs_gc.c',
       'spiffs/src/spiffs_hydrogen.c','spiffs/src/spiffs_nucleus.c')
FORMAT=dict(page_bytes=256,block_bytes=4096,name_bytes=32,metadata_bytes=4,magic=True,magic_length=True)
ADAPTATIONS=dict(read_only=True,page_index='uint16_byte_aligned_may_alias',cache=False,page_check=True)


class NativeExecutionError(RuntimeError):
    """An isolated crash/runtime fault is distinct from a controlled refusal."""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sdk_sources(idf):
    base=Path(idf)/'components/spiffs'
    return {name:digest(base/name) for name in FILES}


def build(idf, directory, expected_sources, sanitize=False):
    base=Path(idf)/'components/spiffs';directory=Path(directory)
    if sdk_sources(idf)!=expected_sources:
        raise ValueError('SDK SPIFFS source fingerprint differs from pinned profile')
    directory.mkdir(mode=0o700)
    header=(base/'include/spiffs_config.h').read_text()
    old='#define SPIFFS_READ_ONLY                        0'
    if header.count(old)!=1:
        raise ValueError('SDK read-only adaptation boundary differs')
    header=header.replace(old,old[:-1]+'1')
    index='typedef u16_t spiffs_page_ix;'
    if header.count(index)!=1:raise ValueError('SDK packed-index adaptation boundary differs')
    header=header.replace(index,'typedef u16_t spiffs_page_ix __attribute__((aligned(1), may_alias));')
    (directory/'spiffs_config.h').write_text(header)
    (directory/'sdkconfig.h').write_text('''#pragma once
#define CONFIG_SPIFFS_PAGE_SIZE 256
#define CONFIG_SPIFFS_OBJ_NAME_LEN 32
#define CONFIG_SPIFFS_META_LENGTH 4
#define CONFIG_SPIFFS_USE_MAGIC 1
#define CONFIG_SPIFFS_USE_MAGIC_LENGTH 1
#define CONFIG_SPIFFS_PAGE_CHECK 1
#define CONFIG_SPIFFS_GC_MAX_RUNS 10
''')
    (directory/'esp_log.h').write_text('#pragma once\n')
    (directory/'esp_assert.h').write_text('#pragma once\n#define ESP_STATIC_ASSERT _Static_assert\n')
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-g','-fno-omit-frame-pointer','-fno-pie',
           '-I'+str(directory),'-I'+str(base/'spiffs/src')]
    if sanitize: flags += ['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    env=dict(PATH=os.defpath,LC_ALL='C')
    objects=[]
    for name in FILES:
        if not name.endswith('.c'): continue
        obj=directory/(Path(name).stem+'.o')
        # Read-only compilation makes upstream writable-only locals unused.
        subprocess.run(['gcc',*flags,'-Wall','-Wextra','-Werror','-Wno-unused-parameter',
            '-Wno-unused-variable','-Wno-unused-function','-Wno-sign-compare','-c',str(base/name),'-o',str(obj)],
            check=True,timeout=30,env=env,stdout=subprocess.DEVNULL)
        objects.append(obj)
    helper=Path(__file__).with_name('native')/'spiffs_evidence_reader.c'
    own=directory/'reader.o'
    subprocess.run(['gcc',*flags,'-Wall','-Wextra','-Werror','-c',str(helper),'-o',str(own)],check=True,timeout=30,env=env)
    binary=directory/'reader'
    subprocess.run(['gcc',*flags,'-no-pie',str(own),*map(str,objects),'-o',str(binary)],check=True,timeout=30,env=env)
    return binary


def limits(sanitize=False):
    if not sanitize: resource.setrlimit(resource.RLIMIT_AS,(512*1024*1024,512*1024*1024))
    resource.setrlimit(resource.RLIMIT_CPU,(20,20))
    resource.setrlimit(resource.RLIMIT_FSIZE,(40*1024*1024,40*1024*1024))
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))


def run(binary, source, offset, size, output, timeout=30, sanitize=False):
    # A fresh private output file bounds the stream on disk rather than using
    # unbounded communicate() buffering. Native seccomp forbids opening paths,
    # network and source mutation; the source fd is independently O_RDONLY.
    descriptor=os.open(output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    env=dict(PATH=os.defpath,LC_ALL='C')
    if sanitize: env.update(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
    process=None
    try:
        with os.fdopen(descriptor,'wb') as stream:
            process=subprocess.Popen([str(binary),str(source.fileno()),str(offset),str(size)],
                pass_fds=(source.fileno(),),stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.PIPE,
                env=env,preexec_fn=lambda: limits(sanitize),start_new_session=True)
            try:
                _,errors=process.communicate(timeout=timeout)
            except BaseException:
                process.kill();process.communicate();raise
            stream.flush();os.fsync(stream.fileno())
        if process.returncode!=0 and (process.returncode!=1 or re.fullmatch(rb'SPIFFS_EVIDENCE_[A-Z_]+:-?[0-9]+\n',errors) is None):
            log=Path(str(output)+'.stderr')
            fd=os.open(log,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
            with os.fdopen(fd,'wb') as stream:
                stream.write(errors[:65536]);stream.flush();os.fsync(stream.fileno())
            raise NativeExecutionError('isolated SPIFFS reader crashed or reported an unexpected runtime fault; private diagnostics retained')
        if process.returncode!=0:
            raise ValueError('read-only SPIFFS child refused image: '+errors[:4096].decode('ascii','replace'))
        if errors:
            raise NativeExecutionError('SPIFFS child reported unexpected diagnostic')
        if Path(output).stat().st_size>40*1024*1024:
            raise ValueError('SPIFFS child output budget exceeded')
    except BaseException:
        # Keep even a partial child stream as evidence; never call a repair API.
        raise
