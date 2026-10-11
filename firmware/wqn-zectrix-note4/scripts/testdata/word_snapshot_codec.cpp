#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "device_protocol/word_study.h"
namespace wqn {
template<typename T> using WordStorePsramAllocator=std::allocator<T>;
enum class WordPresentationPhase : uint8_t { kFront=0,kBack=1 };
@@STRUCTS@@
}
@@LIMITS@@
@@CODEC@@
int passes=0,failures=0;
void Check(bool okay,const char* name) { std::printf("%s: %s\n",okay?"PASS":"FAIL",name); okay?++passes:++failures; }
int main() {
    using S=wqn::PersistedWordSession; using namespace wqn::protocol::word_study_v1;
    S original; original.active=true; original.remote.session_id="original-SID";
    original.remote.items.resize(1); original.remote.deck_ids.resize(1); original.remote.snapshot.resize(1);
    original.remote.items[0].ordinal=7;
    S copy=original;
    Check(SameEncodedSessionSnapshot(original,copy),"identical complete snapshots compare equal through the real codec");
    const std::vector<std::function<void(S&)>> mutations={
        [](S& s){s.active=false;}, [](S& s){s.paused=true;},
        [](S& s){s.phase=wqn::WordPresentationPhase::kBack;}, [](S& s){s.position=1;},
        [](S& s){s.start_index=8;}, [](S& s){s.deck_scope_generation=8;},
        [](S& s){s.remote.mode=Mode::kReview;}, [](S& s){s.remote.purpose=Purpose::kLookup;},
        [](S& s){s.remote.ordering=Ordering::kLexicographic;}, [](S& s){s.remote.include_mastered=true;},
        [](S& s){s.remote.has_more=true;}, [](S& s){s.remote.optional_count=99;},
        [](S& s){s.remote.next_sequence=8;}, [](S& s){s.remote.session_id="different-SID";},
        [](S& s){s.remote.seed="different-seed";}, [](S& s){s.remote.cursor="different-cursor";},
        [](S& s){s.remote.progress_revision=8;},
        [](S& s){s.remote.deck_ids[0].value[0]='d';}, [](S& s){s.remote.deck_ids.emplace_back();},
        [](S& s){s.remote.snapshot[0].deck_id[0]='d';},
        [](S& s){s.remote.snapshot[0].content_revision=8;}, [](S& s){s.remote.snapshot[0].pack_revision=8;},
        [](S& s){s.remote.snapshot[0].sha256[0]='a';}, [](S& s){s.remote.snapshot.emplace_back();},
        [](S& s){s.remote.items[0].item_id[0]='w';}, [](S& s){s.remote.items[0].deck_id[0]='d';},
        [](S& s){s.remote.items[0].ordinal=8;}, [](S& s){s.remote.items.emplace_back();}
    };
    for(const auto& mutate:mutations) { copy=original; mutate(copy);
        Check(!SameEncodedSessionSnapshot(original,copy),"each encoded field independently prevents false snapshot coalescing"); }
    copy=original; copy.position=2;
    Check(!SameEncodedSessionSnapshot(copy,copy),"invalid cursor is not identical durable data even compared with itself");
    copy=original; copy.remote.cursor.assign(257,'a');
    Check(!SameEncodedSessionSnapshot(copy,copy),"over-limit cursor cannot become a coalesced success");
    copy=original; copy.remote.items.resize(kMaxSessionItems+1);
    Check(!SameEncodedSessionSnapshot(copy,copy),"over-limit candidate window cannot become a coalesced success");
    std::printf("%d PASS / %d FAIL (real snapshot codec and structs; NOT flash HIL)\n",passes,failures);
    return failures?1:0;
}
