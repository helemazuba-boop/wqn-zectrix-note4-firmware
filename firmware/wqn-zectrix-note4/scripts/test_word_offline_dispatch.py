#!/usr/bin/env python3
"""Check the production radio-admission policy; no WiFi/RTOS/HIL claim."""
from pathlib import Path
import subprocess
import tempfile
import argparse

def function(source, marker):
    start=source.index(marker)
    at=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[at]=='{')-(source[at]=='}')
        at+=1
    return source[start:at]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation',choices=['force-network'])
    args=parser.parse_args()
    source = (ROOT / 'main/ui/cloud_runner.cpp').read_text()
    helper = function(source, 'bool JobNeedsConnectivityDemand(')
    if args.mutation:
        if 'return false;' not in helper:
            raise ValueError('admission boundary changed')
        helper=helper.replace('return false;','return true;')
    review_helper=function((ROOT/'main/ui/word_cloud.cpp').read_text(), 'bool CanOfferLocalWalkAfterReviewFailure(')
    cloud=(ROOT/'main/ui/word_cloud.cpp').read_text()
    a=cloud.index('        const bool offer_local_walk =')
    projection=cloud[a:cloud.index('        if (result.result != ESP_OK) {',a)]
    wire = (ROOT / 'main/ui/ui_internal.h').read_text()
    # Real WordCloudOp + request shape. Unused fields are not copied into the
    # fixture; the production policy reads only these scalar admission inputs.
    enum = wire[wire.index('enum class WordCloudOp {'):wire.index('\nstruct WordCloudRequest {')]
    unit = '''#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
namespace wqn::protocol::word_study_v1 {
enum class Mode {kSequential,kRandom,kDictionary,kReview,kIntake,kShuffle,kMistakes};
}
enum class CloudDomain {kWord,kWordBulk,kNote,kTodo,kProblem};
''' + enum + '''
struct Request {WordCloudOp op;bool resume_existing=false,local_walk=false;unsigned char study_mode=0;};
struct CloudJob {CloudDomain domain=CloudDomain::kWord;Request word;};
''' + helper + '''
using WordCloudRequest=Request;
constexpr int ESP_OK=0;
struct WordCloudResult {int result=-1,session_compact_result=0,session_persist_result=0,persisted_session=0;bool auth_required=false,review_queue_unavailable=false;struct {std::string code;bool retryable=false;} protocol_error;};
''' + review_helper + '''
namespace wqn {
enum class WordAppMode {kHome,kReviewComplete};
struct WordAppState {struct {bool cancel_start_requested=false;} session;struct {bool queue_unavailable=false;} review;WordAppMode mode=WordAppMode::kHome;};
bool ApplyWordSessionStartResult(WordAppState* state,int,int,int,int){state->session.cancel_start_requested=false;state->mode=WordAppMode::kHome;return true;}
}
struct State {wqn::WordAppState word_app;};
bool ApplyProjection(State* state,WordCloudResult result) {
''' + projection + '''
 return true;
}

int main(){
 int checks=0,failed=0;
 auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL: %s\\n",why);}};
 using Mode=wqn::protocol::word_study_v1::Mode;
 for(auto mode:{Mode::kSequential,Mode::kShuffle}) {
  CloudJob job;job.word.op=WordCloudOp::kStartSession;job.word.study_mode=static_cast<unsigned char>(mode);
  check(!JobNeedsConnectivityDemand(job),"local start does not request WiFi");
  check(JobNeedsConnectivityDemand(job,true)==(mode==Mode::kShuffle),"optional online snapshot work retains its caller radio lease");
  job.word.op=WordCloudOp::kFetchSessionPage;job.word.local_walk=true;
  check(!JobNeedsConnectivityDemand(job),"local page does not request WiFi");
 }
 CloudJob job;job.word.op=WordCloudOp::kStartSession;job.word.resume_existing=true;
 check(!JobNeedsConnectivityDemand(job),"cached resume does not request WiFi");
 job.word.resume_existing=false;job.word.study_mode=static_cast<unsigned char>(Mode::kReview);
 check(JobNeedsConnectivityDemand(job),"online review keeps radio admission");
 job.word.op=WordCloudOp::kFetchSessionPage;job.word.study_mode=static_cast<unsigned char>(Mode::kShuffle);
 check(JobNeedsConnectivityDemand(job),"legacy remote shuffle page still requests WiFi");
 job.word.op=WordCloudOp::kPackSync;
 check(JobNeedsConnectivityDemand(job),"content and bitmap refresh retain explicit network admission");
 job.word.op=WordCloudOp::kClearSession;
 check(!JobNeedsConnectivityDemand(job),"local clear does not request WiFi");
 job.domain=CloudDomain::kNote;
 check(JobNeedsConnectivityDemand(job),"other domains keep their existing policy");
 WordCloudRequest review;review.op=WordCloudOp::kStartSession;review.study_mode=static_cast<unsigned char>(Mode::kReview);
 WordCloudResult error;
 check(CanOfferLocalWalkAfterReviewFailure(review,error),"WiFi online but transport failed still offers local sequential action");
 error.protocol_error.code="WORD_STUDY_UNAVAILABLE";error.protocol_error.retryable=true;
 check(CanOfferLocalWalkAfterReviewFailure(review,error),"retryable cloud outage still offers local action");
 error.auth_required=true;
 check(!CanOfferLocalWalkAfterReviewFailure(review,error),"pairing failure is not a completed review");
 error.auth_required=false;error.protocol_error.code="UNAUTHORIZED";
 check(!CanOfferLocalWalkAfterReviewFailure(review,error),"real unauthorized response stays unauthorized");
 error.protocol_error.code="INVALID_REQUEST";error.protocol_error.retryable=false;
 check(!CanOfferLocalWalkAfterReviewFailure(review,error),"semantic request rejection keeps its error");
 error.result=ESP_OK;error.protocol_error.code.clear();
 check(!CanOfferLocalWalkAfterReviewFailure(review,error),"healthy review is never marked unavailable");
 State ui;WordCloudResult failed_review;failed_review.review_queue_unavailable=true;
 ui.word_app.session.cancel_start_requested=true;
 ApplyProjection(&ui,failed_review);
 check(ui.word_app.mode==wqn::WordAppMode::kHome,"late unavailable result cannot reopen a cancelled review");
 ui.word_app.session.cancel_start_requested=false;
 ApplyProjection(&ui,failed_review);
 check(ui.word_app.mode==wqn::WordAppMode::kReviewComplete&&ui.word_app.review.queue_unavailable,"owned unavailable review exposes the existing local walk action");
 failed_review.review_queue_unavailable=false;
 ApplyProjection(&ui,failed_review);
 check(ui.word_app.mode==wqn::WordAppMode::kHome,"ordinary failure does not become a local completion page");
 std::printf("Offline word dispatch: %d checks, %d failures (NOT HIL)\\n",checks,failed);
 return failed?1:0;
}
'''
    unit = '#include <initializer_list>\n' + unit
    with tempfile.TemporaryDirectory(prefix='wqn-word-dispatch-') as temporary:
        directory = Path(temporary)
        path, binary = directory / 'policy.cpp', directory / 'policy'
        path.write_text(unit)
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
