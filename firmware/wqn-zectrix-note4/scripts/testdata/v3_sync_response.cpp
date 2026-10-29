#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "device_protocol/v3.h"

bool observing=false;
size_t cpp_allocations=0,largest_cpp_allocation=0;
void* operator new(std::size_t size)
{
    if (observing) { ++cpp_allocations; largest_cpp_allocation=std::max(largest_cpp_allocation,size); }
    if (void* memory=std::malloc(size==0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory,std::size_t) noexcept { std::free(memory); }

struct TestCase {
    const char* name;
    std::string wire;
    bool valid,no_ids_allocation;
    std::vector<std::string> ids;
    int mistakes;
    size_t targets;
};
const TestCase cases[]={
@@CASES@@
};
size_t checks=0,failures=0;
void Check(bool pass,const char* name,const char* requirement)
{
    ++checks;
    if (!pass) { if (failures<20) std::fprintf(stderr,"FAIL %s: %s\n",name,requirement); ++failures; }
}
bool Empty(const wqn::protocol::v3::SyncData& data)
{
    return data.config_revision==0 && data.sync_cursor==0 && data.auto_sync_interval_minutes==0 &&
        data.todo_count==0 && data.word_due_count==0 && data.word_mistake_count==-1 &&
        data.due_problem_ids.empty() && data.content_targets.empty();
}
int main()
{
    size_t empty_allocations=0,empty_largest=0;
    for (size_t index=0;index<sizeof(cases)/sizeof(cases[0]);++index) {
        const auto& test=cases[index];
        wqn::protocol::v3::SyncData data;
        data.config_revision=999; data.sync_cursor=999; data.auto_sync_interval_minutes=240;
        data.todo_count=data.word_due_count=data.word_mistake_count=99;
        data.due_problem_ids={"old-id-must-not-survive"};
        data.content_targets.push_back({wqn::protocol::v3::SyncContentKind::kProblems,999,"old"});
        wqn::protocol::v3::Error error{"OLD_ERROR",true,99};
        const std::string expected="req_sync_000000001";
        cpp_allocations=largest_cpp_allocation=0; observing=true;
        const auto result=wqn::protocol::v3::ParseSyncResponse(test.wire,expected,&data,&error);
        observing=false;
        if (index==0) { empty_allocations=cpp_allocations; empty_largest=largest_cpp_allocation; }
        Check(result==(test.valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE),test.name,"wire acceptance matches count/UUID/policy");
        Check(error.code.empty() && !error.retryable && error.retry_after_ms==0,test.name,"stale error is cleared");
        if (test.valid) {
            Check(data.config_revision==7 && data.sync_cursor==43 && data.auto_sync_interval_minutes==60 &&
                  data.todo_count==2 && data.word_due_count==5 && data.word_mistake_count==test.mistakes,
                  test.name,"successful scalar data is unchanged");
            Check(data.due_problem_ids==test.ids && data.content_targets.size()==test.targets,
                  test.name,"all IDs preserve exact bytes/order and content targets survive");
        } else {
            Check(Empty(data),test.name,"failed response publishes no partial cursor, summary or target");
            Check(data.due_problem_ids.capacity()==0 && data.content_targets.capacity()==0,
                  test.name,"failed candidate releases output vector allocations");
        }
        if (test.no_ids_allocation) {
            Check(cpp_allocations<=empty_allocations && largest_cpp_allocation<=empty_largest,
                  test.name,"invalid due collection allocates no extra C++ ID/vector storage");
        }
    }
    std::printf("V3 sync response: %zu checks, %zu failures; %zu wire cases; empty envelope C++ allocations=%zu/max=%zu (real parser/cJSON; NOT HIL)\n",
        checks,failures,sizeof(cases)/sizeof(cases[0]),empty_allocations,empty_largest);
    return failures==0 ? 0 : 1;
}
