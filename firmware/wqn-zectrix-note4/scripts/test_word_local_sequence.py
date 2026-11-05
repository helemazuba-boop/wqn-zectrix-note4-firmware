#!/usr/bin/env python3
"""Compile and run the REAL main/word_local_session.cpp pure module on the host.

Generates minimal ESP/IDF stub headers (same technique as
test_word_snapshot_codec.py) and compiles a byte-for-byte copy of the production
translation unit (relocated into the temp build dir so its quoted includes
resolve against the stubs) together with the fixture and a faithful extract of
the REAL ExtendPersistedWordSessionWithPage / WordSessionSnapshotMatches.
No copied implementation of the module under test, no SPIFFS/NVS, no HIL.
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
    'wqn_api.h': '''#pragma once
#include <string>
namespace wqn { struct WqnWordPackManifest{}; struct WqnWordPackManifestItem{}; struct WqnWordEntry{}; }
''',
    'outbox_suspend_reason.h': '''#pragma once
#include <cstdint>
namespace wqn { enum class OutboxSuspendReason : uint8_t { kProtocol=1 }; }
''',
    'device_protocol/v3.h': '''#pragma once
#include <cstdint>
namespace wqn::protocol::v3 {
inline constexpr uint64_t kMaxSafeJsonInteger=9007199254740991ULL;
struct RequestMetadata{}; struct Error{};
}
''',
    # Minimal WordPackIndex view: identical field layout/names to the real
    # word_pack.h, but without dragging wqn_api.h/cJSON.  The production module
    # only reads these fields.
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
}

# Production helper still needed at link time (real simple switch).
POLICY_STUB = '''#include "device_protocol/word_study.h"
namespace wqn::protocol::word_study_v1 {
const char* CandidatePolicyVersionName(Ordering o){
  switch(o){
    case Ordering::kSequential: return "sequential_v1";
    case Ordering::kGuidedRandomV1: return "guided_random_v1";
    case Ordering::kLexicographic: return "lexicographic_v1";
    case Ordering::kDueQueueV1: return "due_queue_v1";
    case Ordering::kNewIntakeV1: return "new_intake_v1";
    case Ordering::kPureRandomV1: return "pure_random_v1";
    case Ordering::kMistakeWordsV1: return "mistake_words_v1";
  }
  return "";
}
}
'''


def extract(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('production boundary drift: ' + start)
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]


def main():
    root = Path(__file__).resolve().parents[1]
    gpp = shutil.which('g++')
    if not gpp:
        raise SystemExit('FAIL: g++ unavailable')
    store = (root / 'main/word_study_store.cpp').read_text(encoding='utf-8')
    # The REAL extender + its pure snapshot comparator, lifted verbatim from
    # word_study_store.cpp (the online cloud runner uses the same function).
    extend = (
        extract(store, 'bool WordSessionSnapshotMatches(',
                '\nesp_err_t ExtendPersistedWordSessionWithPage(') +
        extract(store, 'esp_err_t ExtendPersistedWordSessionWithPage(',
                '\nesp_err_t LoadPersistedWordSession(')
    )
    with tempfile.TemporaryDirectory(prefix='wqn-local-seq-') as tmp:
        d = Path(tmp)
        (d / 'device_protocol').mkdir()
        for name, body in STUBS.items():
            (d / name).write_text(body, encoding='utf-8')
        shutil.copy(root / 'components/device_protocol/include/device_protocol/word_study.h',
                    d / 'device_protocol/word_study.h')
        # Relocate the REAL production header + translation unit + the real
        # word_study_store.h (structs) into the stub include dir.  Byte-for-byte
        # copies: the code under test is the production code.
        for name in ('word_local_session.h', 'word_local_session.cpp',
                     'word_study_store.h'):
            shutil.copy(root / 'main' / name, d / name)
        (d / 'policy.cpp').write_text(POLICY_STUB, encoding='utf-8')
        (d / 'extend.cpp').write_text(
            '#include "word_study_store.h"\n'
            '#include <algorithm>\n#include <cstdio>\n#include <cstring>\n'
            '#include <utility>\n'
            'namespace wqn {\n' + extend + '\n}\n',
            encoding='utf-8')
        fixture = (root / 'scripts/testdata/word_local_sequence.cpp').read_text(
            encoding='utf-8')
        (d / 'fixture.cpp').write_text(fixture, encoding='utf-8')
        cmd = [
            gpp, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(d),
            '-DWQN_WORD_STUDY_SCHEMA_SHA256="host-fixture"',
            str(d / 'word_local_session.cpp'),
            str(d / 'policy.cpp'), str(d / 'extend.cpp'),
            str(d / 'fixture.cpp'), '-o', str(d / 'test'),
        ]
        subprocess.run(cmd, check=True, timeout=60)
        return subprocess.run([str(d / 'test')], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
