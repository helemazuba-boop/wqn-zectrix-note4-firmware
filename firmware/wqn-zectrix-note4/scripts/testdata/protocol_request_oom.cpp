// Exercise the real request encoders and SDK cJSON, not a copied implementation.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "cJSON.h"
#include "device_protocol/v3.h"
#include "device_protocol/word_study.h"
#include "device_protocol/note_study.h"
#include "device_protocol/problem_study.h"

namespace v3 = wqn::protocol::v3;
namespace word = wqn::protocol::word_study_v1;
namespace note = wqn::protocol::note_study_v1;
namespace problem = wqn::protocol::problem_study_v1;

namespace {
size_t attempts = 0, fail_at = 0, live = 0;
bool sustained = false, injected = false;
size_t checks = 0, failures = 0;
constexpr char kSentinel[] = "previous-output-must-survive";
constexpr char kId[] = "11111111-1111-4111-8111-111111111111";
constexpr char kOtherId[] = "22222222-2222-4222-8222-222222222222";

void* Allocate(size_t bytes)
{
    ++attempts;
    if (fail_at != 0 && (attempts == fail_at || (sustained && attempts >= fail_at))) {
        injected = true;
        return nullptr;
    }
    void* p = std::malloc(bytes);
    if (p != nullptr) ++live;
    return p;
}

void Release(void* p)
{
    if (p != nullptr) {
        if (live == 0) std::abort();
        --live;
        std::free(p);
    }
}

void Check(bool pass, const char* name, size_t point = 0)
{
    ++checks;
    if (!pass) {
        if (failures < 12) {
            std::fprintf(stderr, "FAIL %s at allocation %zu (%s)\n", name, point,
                         sustained ? "sustained" : "one-shot");
        }
        ++failures;
    }
}

struct Case {
    const char* name;
    const char* schema;
    const char* definition;
    std::function<esp_err_t(std::string*)> build;
};

v3::RequestMetadata Metadata()
{
    v3::RequestMetadata m;
    m.request_id = "request_0123456789abcdef";
    m.boot_id = "boot_0123456789abcdef";
    m.firmware_version = "test-oom-1";
    m.config_revision = v3::kMaxSafeJsonInteger;
    m.sync_cursor = 123;
    m.limit = 100;
    return m;
}

std::vector<Case> Cases()
{
    const auto m = Metadata();
    word::CreateSessionRequest wc;
    wc.metadata = m;
    wc.scope.deck_ids = {kId, kOtherId};
    wc.scope.include_mastered = true;
    wc.seed = "seed_123";
    wc.start_index = 123;
    wc.new_word_limit = 20;
    word::ObservationRequest wo;
    wo.metadata = m;
    wo.session_id = kId;
    wo.item_id = kOtherId;
    wo.sequence = v3::kMaxSafeJsonInteger;
    wo.action = word::ObservationAction::kKnown;
    wo.occurred_at = "2026-10-29T01:00:00Z";
    word::CandidatePageRequest wp;
    wp.metadata = m;
    wp.cursor = "9007199254740991";
    wp.limit = 100;
    note::CreateSessionRequest nc;
    nc.metadata = m;
    nc.scope.notebook_ids = {kId, kOtherId};
    nc.scope.include_archived = true;
    nc.seed = "seed_456";
    note::ObservationRequest no;
    no.metadata = m;
    no.session_id = kId;
    no.item_id = kOtherId;
    no.sequence = v3::kMaxSafeJsonInteger;
    no.action = note::ObservationAction::kReadCompleted;
    no.occurred_at = wo.occurred_at;
    note::CandidatePageRequest np;
    np.metadata = m;
    np.cursor = wp.cursor;
    np.limit = wp.limit;
    problem::ObservationRequest po;
    po.metadata = m;
    po.problem_id = kId;
    po.action = problem::ReviewAction::kCorrect;
    po.occurred_at = wo.occurred_at;
    const std::string snapshot(64, 'a');
    std::vector<Case> cases = {
        {"bootstrap", "device-control-v3", "bootstrapRequest", [=](auto* b) { return v3::BuildBootstrapRequest(m, b); }},
        {"sync", "device-control-v3", "syncRequest", [=](auto* b) { return v3::BuildSyncRequest(m, 15, b); }},
        {"claim-start", "device-control-v3", "claimStartRequest", [=](auto* b) { return v3::BuildClaimStartRequest(m, "AA:BB:CC:DD:EE:FF", std::string(87, 'A'), b); }},
        {"claim-poll", "device-control-v3", "claimPollRequest", [=](auto* b) { return v3::BuildClaimPollRequest(m, kId, b); }},
        {"word-session", "word-study-v1", "createSessionRequest", [=](auto* b) { return word::BuildCreateSessionRequest(wc, b); }},
        {"word-observation", "word-study-v1", "observationRequest", [=](auto* b) { return word::BuildObservationRequest(wo, b); }},
        {"word-candidate", "word-study-v1", "candidatePageRequest", [=](auto* b) { return word::BuildCandidatePageRequest(wp, b); }},
        {"word-manifest", "word-study-v1", "manifestRequest", [=](auto* b) { return word::BuildManifestRequest(m, 123, 100, b); }},
        {"note-session", "note-study-v1", "createSessionRequest", [=](auto* b) { return note::BuildCreateSessionRequest(nc, b); }},
        {"note-observation", "note-study-v1", "observationRequest", [=](auto* b) { return note::BuildObservationRequest(no, b); }},
        {"note-candidate", "note-study-v1", "candidatePageRequest", [=](auto* b) { return note::BuildCandidatePageRequest(np, b); }},
        {"note-manifest", "note-study-v1", "manifestRequest", [=](auto* b) { return note::BuildManifestRequest(m, 123, 100, b, snapshot); }},
        {"problem-manifest", "problem-study-v1", "manifestRequest", [=](auto* b) { return problem::BuildManifestRequest(m, 123, 100, b, snapshot); }},
        {"problem-observation", "problem-study-v1", "observationRequest", [=](auto* b) { return problem::BuildObservationRequest(po, b); }},
    };
    wc.scope.deck_ids.clear();
    wc.scope.include_mastered = false;
    wc.seed.clear();
    wc.start_index = -1;
    wc.new_word_limit = 0;
    nc.scope.notebook_ids.clear();
    nc.scope.include_archived = false;
    nc.seed.clear();
    cases.push_back({"word-session-defaults", "word-study-v1", "createSessionRequest", [=](auto* b) { return word::BuildCreateSessionRequest(wc, b); }});
    cases.push_back({"note-session-defaults", "note-study-v1", "createSessionRequest", [=](auto* b) { return note::BuildCreateSessionRequest(nc, b); }});
    cases.push_back({"note-manifest-defaults", "note-study-v1", "manifestRequest", [=](auto* b) { return note::BuildManifestRequest(m, 0, 1, b); }});
    cases.push_back({"problem-manifest-defaults", "problem-study-v1", "manifestRequest", [=](auto* b) { return problem::BuildManifestRequest(m, 0, 1, b); }});
    return cases;
}

void InvalidInputs()
{
    auto m = Metadata();
    m.config_revision = v3::kMaxSafeJsonInteger + 1;
    std::vector<std::function<esp_err_t(std::string*)>> cases = {
        [=](auto* b) { return v3::BuildBootstrapRequest(m, b); },
        [=](auto* b) { return v3::BuildSyncRequest(m, 15, b); },
        [=](auto* b) { return v3::BuildSyncRequest(Metadata(), 14, b); },
        [=](auto* b) { return v3::BuildClaimStartRequest(Metadata(), "short", "", b); },
        [=](auto* b) { return v3::BuildClaimPollRequest(Metadata(), "", b); },
        [=](auto* b) { return word::BuildManifestRequest(Metadata(), 0, 0, b); },
        [=](auto* b) { return note::BuildManifestRequest(Metadata(), 0, 1, b, "invalid"); },
        [=](auto* b) { return problem::BuildManifestRequest(Metadata(), 0, 1, b, "invalid"); },
    };
    word::CreateSessionRequest wc;
    wc.metadata = Metadata();
    wc.scope.deck_ids = {kId, "invalid"}; // failure after root/scope/one item are owned
    note::CreateSessionRequest nc;
    nc.metadata = Metadata();
    nc.scope.notebook_ids = wc.scope.deck_ids;
    cases.push_back([=](auto* b) { return word::BuildCreateSessionRequest(wc, b); });
    cases.push_back([=](auto* b) { return note::BuildCreateSessionRequest(nc, b); });
    for (const auto& build : cases) {
        std::string body = kSentinel;
        const size_t before = live;
        Check(build(&body) == ESP_ERR_INVALID_ARG, "invalid input result");
        Check(body == kSentinel, "invalid input output");
        Check(live == before, "invalid input cleanup");
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool normal_only = argc == 2 && std::string(argv[1]) == "--normal-json";
    cJSON_Hooks hooks{Allocate, Release};
    cJSON_InitHooks(&hooks);
    for (const Case& c : Cases()) {
        std::string body = kSentinel;
        attempts = 0;
        const size_t before = live;
        Check(c.build(&body) == ESP_OK, c.name);
        const size_t count = attempts;
        Check(live == before, "successful request cleanup");
        if (normal_only) {
            std::printf("{\"name\":\"%s\",\"schema\":\"%s\",\"definition\":\"%s\",\"payload\":%s}\n",
                        c.name, c.schema, c.definition, body.c_str());
            continue;
        }
        Check(c.build(nullptr) == ESP_ERR_INVALID_ARG, "null output");
        for (bool persistent : {false, true}) {
            sustained = persistent;
            for (size_t point = 1; point <= count; ++point) {
                body = kSentinel;
                attempts = 0;
                injected = false;
                fail_at = point;
                const size_t live_before = live;
                const esp_err_t result = c.build(&body);
                fail_at = 0;
                Check(injected, "allocation was reached", point);
                Check(result == ESP_ERR_NO_MEM, c.name, point);
                Check(body == kSentinel, "failed request changed output", point);
                Check(live == live_before, "orphaned allocation", point);
            }
        }
        std::printf("%s: %zu allocation points x 2 failure modes\n", c.name, count);
    }
    if (!normal_only) {
        InvalidInputs();
        Check(live == 0, "final cJSON allocation balance");
        std::printf("%zu checks, %zu failures, %zu live cJSON allocations\n", checks, failures, live);
    }
    return failures == 0 ? 0 : 1;
}
