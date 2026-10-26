#!/usr/bin/env python3
"""Test real boot-schema preservation and authorized reset decisions.

Exercise write-through and staged-commit NVS visibility.
This does not prove NOR power-loss recovery, filesystem integrity or device HIL.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


PORTS = r'''#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using nvs_handle_t = uint32_t;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 258;
constexpr int ESP_ERR_INVALID_STATE = 259, ESP_ERR_INVALID_CRC = 265;
constexpr int ESP_ERR_NOT_FOUND = 261;
constexpr int ESP_ERR_NVS_NOT_FOUND = 4354, ESP_ERR_NVS_NOT_INITIALIZED = 4353;
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 4365, ESP_ERR_NVS_NEW_VERSION_FOUND = 4368;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
enum nvs_type_t { NVS_TYPE_U32 = 4, NVS_TYPE_LEGACY = 65,
                  NVS_TYPE_BLOB = 66, NVS_TYPE_ANY = 255 };
struct nvs_stats_t { size_t used_entries = 0, namespace_count = 0; };
struct nvs_entry_info_t {
    char namespace_name[16] = {};
    char key[16] = {};
    nvs_type_t type = NVS_TYPE_ANY;
};
struct HostIterator;
using nvs_iterator_t = HostIterator*;
struct esp_partition_t { size_t size = 0; };
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 130;
struct esp_vfs_spiffs_conf_t {
    const char* base_path = nullptr;
    const char* partition_label = nullptr;
    size_t max_files = 0;
    bool format_if_mount_failed = false;
};
void HostLog(const char*, const char*, ...);
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGE(...) HostLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const int error = (expr); \
    if (error != ESP_OK) return error; } while (false)
const char* esp_err_to_name(int);
esp_err_t nvs_flash_init();
esp_err_t nvs_flash_deinit();
esp_err_t nvs_flash_erase();
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_get_stats(const char*, nvs_stats_t*);
esp_err_t nvs_get_used_entry_count(nvs_handle_t, size_t*);
esp_err_t nvs_entry_find(const char*, const char*, nvs_type_t, nvs_iterator_t*);
esp_err_t nvs_entry_next(nvs_iterator_t*);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*);
void nvs_release_iterator(nvs_iterator_t);
const esp_partition_t* esp_partition_find_first(int, int, const char*);
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
bool esp_spiffs_mounted(const char*);
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t*);
esp_err_t esp_vfs_spiffs_unregister(const char*);
esp_err_t esp_spiffs_info(const char*, size_t*, size_t*);
esp_err_t esp_spiffs_format(const char*);
void esp_fill_random(void*, size_t);
bool HostAcquireLease();
void HostReleaseLease();
'''

SLEEP = r'''#pragma once
#include "schema_ports.h"
namespace wqn::runtime {
enum class SleepBlocker { kStorage };
class SleepLease {
    bool held_;
    explicit SleepLease(bool held) : held_(held) {}
public:
    static SleepLease TryAcquire(SleepBlocker, const char*, const char*, int) {
        return SleepLease(HostAcquireLease());
    }
    ~SleepLease() { if (held_) HostReleaseLease(); }
    SleepLease(const SleepLease&) = delete;
    SleepLease& operator=(const SleepLease&) = delete;
    explicit operator bool() const { return held_; }
};
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--mutation', choices=('accept-stale-schema',
                                              'ignore-reset-error', 'auto-format',
                                              'ignore-intent', 'allow-future-reset',
                                              'nonempty-is-fresh', 'drop-intent-early'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = args.source or root / 'main/runtime/storage_schema.cpp'
    body = source.read_text()
    mutations = {
        'accept-stale-schema': (
            'if (!pending && IsCurrentSchema(generation_found, generation, reset_id_size))',
            'if (!pending)'),
        'ignore-reset-error': ('if (reset_result != ESP_OK)', 'if (false)'),
        'auto-format': ('config.format_if_mount_failed = false;',
                        'config.format_if_mount_failed = true;'),
        'ignore-intent': (
            'if (!pending && IsCurrentSchema(generation_found, generation, reset_id_size))',
            'if (IsCurrentSchema(generation_found, generation, reset_id_size))'),
        'allow-future-reset': (
            'if (generation_found && generation > kStorageSchemaGeneration)', 'if (false)'),
        'nonempty-is-fresh': ('if (bytes[i] != 0xff)', 'if (false)'),
        'drop-intent-early': (
            'ESP_RETURN_ON_ERROR(ClearBusinessNvs(), kTag, "clear authorized NVS data");',
            'ESP_RETURN_ON_ERROR(ClearResetIntent(), kTag, "incorrect early clear");\n'
            '    ESP_RETURN_ON_ERROR(ClearBusinessNvs(), kTag, "clear authorized NVS data");'),
    }
    if args.mutation:
        old, new = mutations[args.mutation]
        if body.count(old) != 1:
            raise ValueError('schema mutation boundary drift')
        body = body.replace(old, new)
        if args.mutation == 'drop-intent-early':
            body = body.replace('return ClearResetIntent();', 'return ESP_OK;')
    compiler = shutil.which('g++')
    if not compiler:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-schema-') as temporary:
        directory = Path(temporary)
        (directory / 'runtime').mkdir()
        (directory / 'schema_ports.h').write_text(PORTS)
        for name in ('esp_err.h', 'esp_check.h', 'esp_log.h', 'esp_random.h',
                     'esp_spiffs.h', 'esp_partition.h', 'nvs.h', 'nvs_flash.h'):
            (directory / name).write_text('#pragma once\n#include "schema_ports.h"\n')
        (directory / 'runtime/sleep_coordinator.h').write_text(SLEEP)
        production = source
        if args.mutation:
            production = directory / 'storage_schema.cpp'
            production.write_text(body)
        binary = directory / 'schema'
        subprocess.run([
            compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(directory), '-I', str(root / 'main'),
            str(root / 'scripts/testdata/storage_schema.cpp'), str(production),
            '-o', str(binary)], check=True, timeout=30)
        return subprocess.run([str(binary)], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
