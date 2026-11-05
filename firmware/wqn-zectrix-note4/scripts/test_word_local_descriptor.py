#!/usr/bin/env python3
"""Compile and run the REAL word_study_store.cpp descriptor codec + file +
StorageService seam on the host.

Generates stub ESP/IDF headers, compiles the FULL production translation units
(word_study_store.cpp + word_local_session.cpp) with the real public headers,
and link-time --wrap redirects /storage/* to a temp dir. StorageService
transactions run inline (the seam), so the 4 durable APIs are exercised end to
end on real libc files. No SPIFFS/power-loss/concurrency HIL proof.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

STUBS = {
    'esp_err.h': '''#pragma once
using esp_err_t = int;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x106
#define ESP_ERR_INVALID_RESPONSE 0x107
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10A
inline const char* esp_err_to_name(esp_err_t){return "stub";}
''',
    'esp_heap_caps.h': '''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
inline void* heap_caps_malloc(size_t n, int){return malloc(n);}
inline void heap_caps_free(void* p){free(p);}
inline size_t heap_caps_get_free_size(int){return 1u<<20;}
inline size_t heap_caps_get_largest_free_block(int){return 1u<<20;}
''',
    'esp_log.h': '''#pragma once
''',
    'esp_check.h': '''#pragma once
''',
    'esp_rom_crc.h': '''#pragma once
#include <cstdint>
#include <cstddef>
inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* data, uint32_t size){
  crc = ~crc;
  for(uint32_t i=0;i<size;++i){crc^=data[i];for(int b=0;b<8;++b)crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);}
  return ~crc;
}
''',
    'esp_timer.h': '''#pragma once
#include <cstdint>
inline int64_t esp_timer_get_time(){return 0;}
''',
    'esp_spiffs.h': '''#pragma once
''',
    'wqn_api.h': '''#pragma once
#include <string>
namespace wqn { struct WqnWordPackManifest{}; struct WqnWordPackManifestItem{}; struct WqnWordEntry{}; }
''',
    'device_protocol/v3.h': '''#pragma once
#include <cstdint>
namespace wqn::protocol::v3 {
inline constexpr uint64_t kMaxSafeJsonInteger=9007199254740991ULL;
struct RequestMetadata{}; struct Error{};
}
''',
    'word_pack.h': '''#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
namespace wqn {
template<typename T> struct PsramAllocator {
  using value_type=T; PsramAllocator()=default;
  template<typename U> PsramAllocator(const PsramAllocator<U>&){}
  T* allocate(std::size_t n){return n?static_cast<T*>(malloc(n*sizeof(T))):nullptr;}
  void deallocate(T* p,std::size_t){free(p);}
};
template<typename A,typename B> bool operator==(const PsramAllocator<A>&,const PsramAllocator<B>&){return true;}
template<typename A,typename B> bool operator!=(const PsramAllocator<A>&,const PsramAllocator<B>&){return false;}
struct WordPackIndexEntry {
  char word_id[37]; char deck_id[37]; char word[81]; char pack_stem[28];
  uint32_t file_offset; uint32_t deck_order; int32_t sort_index;
};
struct WordPackIdentity {
  char deck_id[37]={}; uint64_t content_revision=0; uint64_t pack_revision=0;
  char sha256[65]={};
};
struct WordPackIndex {
  bool mounted=false; bool has_manifest=false; bool truncated=false;
  bool pack_error=false; std::string status_message;
  size_t pack_count=0; size_t pack_bytes=0; uint64_t manifest_revision=0;
  std::vector<WordPackIndexEntry,PsramAllocator<WordPackIndexEntry>> entries;
  std::vector<WordPackIdentity,PsramAllocator<WordPackIdentity>> pack_identities;
};
}
''',
    'storage.h': '''#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
namespace wqn {
uint32_t GetDeckScopeGeneration();
esp_err_t SaveWordSessionCursorNvs(const char* key, const void* record, size_t size);
esp_err_t LoadWordSessionCursorNvs(const char* key, void* record, size_t size, bool* found);
esp_err_t ClearWordSessionCursorNvs(const char* key);
}
''',
    'runtime/sleep_coordinator.h': '''#pragma once
#include <cstdint>
namespace wqn::runtime {
enum class SleepBlocker : uint8_t { kStorage };
class SleepLease {
 public:
  SleepLease()=default;
  SleepLease(SleepLease&& o) noexcept : ok_(o.ok_){o.ok_=false;}
  SleepLease& operator=(SleepLease&& o) noexcept {ok_=o.ok_;o.ok_=false;return *this;}
  explicit operator bool() const {return ok_;}
  void Reset(){ok_=false;}
  static SleepLease TryAcquire(SleepBlocker,const char*,const char*,int);
  static SleepLease TryAcquireStoragePreparation(uint32_t,const char*,const char*,int);
  bool ok_=false;
};
}
''',
    'services/storage_service.h': '''#pragma once
#include <cstdint>
#include "esp_err.h"
namespace wqn::services {
using StorageTransaction = esp_err_t (*)(void* context);
using StorageContextCleanup = void (*)(void* context);
esp_err_t StartStorageService();
esp_err_t ExecuteStorageTransactionNamed(StorageTransaction,void*,const char*);
esp_err_t ExecuteForegroundStorageTransaction(StorageTransaction,void*,const char*);
esp_err_t ExecuteOwnedStorageTransactionUntil(StorageTransaction,void*,StorageContextCleanup,const char*,int64_t);
bool IsStorageServiceTask();
}
''',
}

# ESP_LOGx / ESP_RETURN_ON_ERROR macros are injected at the top of the compile
# unit via -include so the real .cpp does not need to be edited.
MACRO_HEADER = '''#pragma once
#include <cstdio>
inline void WqnFixtureLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) WqnFixtureLog(__VA_ARGS__)
#define ESP_LOGD(...) WqnFixtureLog(__VA_ARGS__)
#define ESP_LOGW(...) WqnFixtureLog(__VA_ARGS__)
#define ESP_LOGE(...) WqnFixtureLog(__VA_ARGS__)
#define ESP_RETURN_ON_ERROR(expr, ...) do { \\
    const esp_err_t wqn_fixture_err = (expr); \\
    if (wqn_fixture_err != ESP_OK) return wqn_fixture_err; \\
} while (false)
'''


def main(fixture_source=None):
    root = Path(__file__).resolve().parents[1]
    gpp = shutil.which('g++')
    if not gpp:
        raise SystemExit('FAIL: g++ unavailable')
    with tempfile.TemporaryDirectory(prefix='wqn-local-desc-') as tmp:
        d = Path(tmp)
        (d / 'device_protocol').mkdir()
        (d / 'runtime').mkdir()
        (d / 'services').mkdir()
        (d / 'storage').mkdir()
        for name, body in STUBS.items():
            (d / name).write_text(body, encoding='utf-8')
        (d / 'wqn_macros.h').write_text(MACRO_HEADER, encoding='utf-8')
        # Real protocol + reason header.
        shutil.copy(root / 'components/device_protocol/include/device_protocol/word_study.h',
                    d / 'device_protocol/word_study.h')
        shutil.copy(root / 'components/device_protocol/include/device_protocol/v3.h',
                    d / 'device_protocol/v3.h')
        shutil.copy(root / 'main/outbox_suspend_reason.h', d / 'outbox_suspend_reason.h')
        # Real public headers + FULL production translation units (copied so
        # their quoted includes resolve against the generated stubs).
        for name in ('word_study_store.h', 'word_local_session.h',
                     'word_study_store.cpp', 'word_local_session.cpp'):
            shutil.copy(root / 'main' / name, d / name)
        fixture = fixture_source if fixture_source is not None else (
            root / 'scripts/testdata/word_local_descriptor.cpp').read_text(encoding='utf-8')
        (d / 'fixture.cpp').write_text(fixture, encoding='utf-8')
        # CandidatePolicyVersionName (real simple switch) for the pure module.
        (d / 'policy.cpp').write_text('''#include "device_protocol/word_study.h"
namespace wqn::protocol::word_study_v1 {
const char* CandidatePolicyVersionName(Ordering o){
  switch(o){
    case Ordering::kSequential:return "sequential_v1";
    case Ordering::kGuidedRandomV1:return "guided_random_v1";
    case Ordering::kLexicographic:return "lexicographic_v1";
    case Ordering::kDueQueueV1:return "due_queue_v1";
    case Ordering::kNewIntakeV1:return "new_intake_v1";
    case Ordering::kPureRandomV1:return "pure_random_v1";
    case Ordering::kMistakeWordsV1:return "mistake_words_v1";
  } return "";
}}
''', encoding='utf-8')
        cmd = [
            gpp, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-include', str(d / 'wqn_macros.h'),
            '-I' + str(d), '-I' + str(root / 'main'),
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host-fixture"',
            '-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="host-fixture"',
            '-DWORD_LOCAL_DESCRIPTOR_HOST_SEAM',
            str(d / 'word_study_store.cpp'),
            str(d / 'word_local_session.cpp'),
            str(d / 'policy.cpp'),
            str(d / 'fixture.cpp'),
            '-Wl,--wrap=fopen,--wrap=rename,--wrap=remove,--wrap=stat,--wrap=opendir',
            '-o', str(d / 'test'),
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
        if proc.returncode != 0:
            print(proc.stdout[-4000:])
            print(proc.stderr[-6000:])
            return 1
        result = subprocess.run([str(d / 'test'), str(d / 'storage')],
                                timeout=120)
        return result.returncode




if __name__ == '__main__':
    raise SystemExit(main())
