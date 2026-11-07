"""Unmodified SDK NVS/CRC objects for production-function host fixtures.

This is a test build adapter, not a device NVS implementation or media reader.
The calling C++ fixture supplies single-threaded public-handle dispatch. SDK
Page/Storage logic and CRC stay unchanged; no SDK-wide locks or RTOS are modeled.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

CPP = ('nvs_page.cpp', 'nvs_types.cpp', 'nvs_item_hash_list.cpp',
       'nvs_pagemanager.cpp', 'nvs_storage.cpp')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sdk_sources(idf):
    idf = Path(idf)
    paths = list((idf / 'components/nvs_flash').rglob('*.hpp'))
    paths += list((idf / 'components/nvs_flash/include').glob('*.h'))
    paths += list((idf / 'components/nvs_flash/private_include').glob('*.h'))
    paths += [idf / 'components/nvs_flash/src' / name for name in CPP]
    paths += [idf / 'components/nvs_flash/src' / name for name in
              ('intrusive_list.h', 'nvs_api.cpp', 'nvs_handle_simple.cpp')]
    paths += [idf / 'components/esp_rom/linux/esp_rom_crc.c',
              idf / 'components/esp_rom/include/esp_rom_crc.h',
              idf / 'components/esp_common/include/esp_err.h',
              idf / 'components/esp_common/include/esp_compiler.h']
    return {str(path.relative_to(idf)): digest(path) for path in sorted(set(paths))}


def build(idf, directory):
    idf, directory = Path(idf), Path(directory)
    sources = sdk_sources(idf)
    directory.mkdir(mode=0o700)
    stubs = {
        'sdkconfig.h': '#pragma once\n',
        'esp_heap_caps.h': '#pragma once\n',
        'esp_attr.h': '#pragma once\n#define IDF_DEPRECATED(x) __attribute__((deprecated(x)))\n',
        'esp_log.h': ('#pragma once\n' + ''.join(
            f'#define ESP_LOG{level}(...) ((void)0)\n' for level in 'EWIDV')),
        'spi_flash_mmap.h': '#pragma once\n#define SPI_FLASH_SEC_SIZE 4096\n#define ESP_ERR_FLASH_OP_FAIL (ESP_ERR_FLASH_BASE+1)\n',
        'esp_partition.h': '#pragma once\n#include <stdint.h>\nstatic inline uint32_t esp_partition_get_main_flash_sector_size(void){return 4096;}\n',
    }
    for name, content in stubs.items():
        (directory / name).write_text(content)
    flags = ['-O1', '-g', '-fno-omit-frame-pointer', '-fno-pie',
             '-ffunction-sections', '-fdata-sections', '-DNO_DEBUG_STORAGE',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I' + str(directory), '-I' + str(idf / 'components/nvs_flash/src'),
             '-I' + str(idf / 'components/nvs_flash/include'),
             '-I' + str(idf / 'components/nvs_flash/private_include'),
             '-I' + str(idf / 'components/esp_common/include'),
             '-I' + str(idf / 'components/esp_rom/include')]
    env = dict(PATH=os.defpath, LC_ALL='C')
    for name in CPP:
        subprocess.run(['g++', '-std=c++17', *flags, '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-variable', '-c',
                        str(idf / 'components/nvs_flash/src' / name),
                        '-o', str(directory / (name + '.o'))],
                       check=True, timeout=30, env=env)
    subprocess.run(['gcc', '-std=c11', *flags, '-c',
                    str(idf / 'components/esp_rom/linux/esp_rom_crc.c'),
                    '-o', str(directory / 'crc.o')], check=True, timeout=30, env=env)
    if sdk_sources(idf) != sources:
        raise RuntimeError('SDK source changed during fixture build')
    (directory / 'build.json').write_text(json.dumps(dict(
        format='wqn-nvs-sdk-fixture-v1', sdk_sources=sources,
        adapter_source_sha256=digest(Path(__file__)),
        host_stubs={name: digest(directory / name) for name in stubs},
        object_sha256={name: digest(directory / name) for name in
                       ['crc.o', *(name + '.o' for name in CPP)]},
        ASan_UBSan=True, device_or_concurrency_proof=False), indent=2) + '\n')
