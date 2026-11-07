"""Unmodified SDK NVS/CRC in a read-only, resource-limited Linux child."""
import os
from pathlib import Path
import re
import resource
import subprocess

import raw_flash_import as raw
import spiffs_evidence_native as process_guard

CPP=('nvs_page.cpp','nvs_types.cpp','nvs_item_hash_list.cpp','nvs_pagemanager.cpp','nvs_storage.cpp')


def sdk_sources(idf):
    idf=Path(idf)
    paths=list((idf/'components/nvs_flash').rglob('*.hpp'))
    paths+=list((idf/'components/nvs_flash/include').glob('*.h'))
    paths+=list((idf/'components/nvs_flash/private_include').glob('*.h'))
    paths+=[idf/'components/nvs_flash/src'/name for name in CPP]
    paths+=[idf/'components/nvs_flash/src/intrusive_list.h',idf/'components/esp_rom/linux/esp_rom_crc.c',
            idf/'components/esp_rom/include/esp_rom_crc.h',idf/'components/esp_common/include/esp_err.h',
            idf/'components/esp_common/include/esp_compiler.h']
    return {str(path.relative_to(idf)):raw.hash_file(path) for path in sorted(set(paths))}


def build(idf,output,expected_sources=None,sanitize=False):
    idf=Path(idf);output=Path(output)
    pins=sdk_sources(idf)
    if expected_sources is not None and expected_sources!=pins:raise ValueError('NVS SDK source fingerprint differs')
    output.mkdir(mode=0o700)
    stubs={'sdkconfig.h':'#pragma once\n', 'esp_heap_caps.h':'#pragma once\n',
        'esp_attr.h':'#pragma once\n#define IDF_DEPRECATED(x) __attribute__((deprecated(x)))\n',
        'esp_log.h':'#pragma once\n#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGV(...) ((void)0)\n',
        'spi_flash_mmap.h':'#pragma once\n#define SPI_FLASH_SEC_SIZE 4096\n#define ESP_ERR_FLASH_OP_FAIL (ESP_ERR_FLASH_BASE+1)\n',
        'esp_partition.h':'#pragma once\n#include <stdint.h>\nstatic inline uint32_t esp_partition_get_main_flash_sector_size(void){return 4096;}\n'}
    for name,data in stubs.items():raw.wire.private(output/name,data.encode())
    flags=['-O1','-g','-fno-omit-frame-pointer','-fno-pie','-ffunction-sections','-fdata-sections','-DNO_DEBUG_STORAGE',
        '-I'+str(output),'-I'+str(idf/'components/nvs_flash/src'),'-I'+str(idf/'components/nvs_flash/include'),
        '-I'+str(idf/'components/nvs_flash/private_include'),'-I'+str(idf/'components/esp_common/include'),
        '-I'+str(idf/'components/esp_rom/include')]
    if sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    env=dict(PATH=os.defpath,LC_ALL='C');objects=[]
    for name in CPP:
        obj=output/(name+'.o')
        subprocess.run(['g++','-std=c++17',*flags,'-Wall','-Wextra','-Werror','-Wno-unused-parameter',
            '-Wno-unused-variable','-c',str(idf/'components/nvs_flash/src'/name),'-o',str(obj)],check=True,timeout=30,env=env)
        objects.append(obj)
    crc=output/'crc.o'
    subprocess.run(['gcc','-std=c11',*flags,'-c',str(idf/'components/esp_rom/linux/esp_rom_crc.c'),'-o',str(crc)],check=True,timeout=30,env=env)
    helper=Path(__file__).with_name('native')/'nvs_context_reader.cpp';obj=output/'helper.o'
    subprocess.run(['g++','-std=c++17',*flags,'-Wall','-Wextra','-Werror','-c',str(helper),'-o',str(obj)],check=True,timeout=30,env=env)
    binary=output/'reader'
    subprocess.run(['g++',*flags,'-no-pie','-Wl,--gc-sections',str(obj),str(crc),*map(str,objects),'-o',str(binary)],check=True,timeout=30,env=env)
    value=dict(format='wqn-nvs-context-reader-build-v1',sdk_sources=pins,helper_sha256=raw.hash_file(helper),
        wrapper_sha256=raw.hash_file(Path(__file__)),binary_sha256=raw.hash_file(binary),
        stubs={name:raw.hash_file(output/name) for name in stubs},sanitized=sanitize,readonly_partition=True)
    pin=raw.private_json(output/'build.json',value);raw.wire.sync_dir(output)
    return dict(status='nvs_reader_built',build_manifest_sha256=pin)


def binary(directory,pin):
    directory=Path(directory);manifest=raw.load(directory/'build.json',pin)
    if not isinstance(manifest,dict) or manifest.get('format')!='wqn-nvs-context-reader-build-v1' or manifest.get('readonly_partition') is not True:
        raise ValueError('invalid NVS reader manifest')
    for key,path in [('helper_sha256',Path(__file__).with_name('native')/'nvs_context_reader.cpp'),
                     ('wrapper_sha256',Path(__file__)),('binary_sha256',directory/'reader')]:
        if manifest.get(key)!=raw.hash_file(path):raise ValueError('NVS reader source/binary differs')
    if not isinstance(manifest.get('stubs'),dict) or any('/' in name or name in ('.','..') or raw.hash_file(directory/name)!=digest for name,digest in manifest['stubs'].items()):
        raise ValueError('NVS reader host adapter differs')
    return directory/'reader'


def run(reader,source,output,sanitize=False,timeout=30):
    fd=os.open(output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    env=dict(PATH=os.defpath,LC_ALL='C')
    if sanitize:env.update(ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
    def limits():
        process_guard.limits(sanitize)
        resource.setrlimit(resource.RLIMIT_FSIZE,(1024*1024,1024*1024))
    with os.fdopen(fd,'wb') as stream:
        child=subprocess.Popen([str(reader),str(source.fileno())],stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.PIPE,
            pass_fds=(source.fileno(),),env=env,preexec_fn=limits,start_new_session=True)
        try:_,errors=child.communicate(timeout=timeout)
        except BaseException:child.kill();child.communicate();raise
        stream.flush();os.fsync(stream.fileno())
    if errors or child.returncode:
        if child.returncode==1 and re.fullmatch(rb'NVS_CONTEXT_[A-Z_]+:-?[0-9]+\n',errors):
            raise ValueError('NVS reader refused source: '+errors.decode('ascii'))
        raw.wire.private(Path(str(output)+'.stderr'),errors[:65536])
        raise process_guard.NativeExecutionError('NVS child runtime fault; private diagnostics retained')
    return raw.load(output,limit=1024*1024)
