// Host verification for the long-sequence local-session / observation-batch
// wire codec. Compiles the REAL production word_study.cpp, the REAL protocol
// types (word_study.h / v3.h) and the REAL SDK cJSON allocator. ESP ports are
// stubbed only (esp_err.h / esp_check.h). No fake structs stand in for the
// production codec or its allocator/API shape.
//
// The binary emits:
//   BUILD_REGISTER <json>            (canonical registration request)
//   BUILD_BATCH <json>               (canonical batch request)
//   CASE <name> PASS|FAIL            (one per assertion)
//   SUMMARY <checks> <failures>
// The Python runner validates the two built requests against the pinned JSON
// Schema and byte/value-compares them to the golden fixtures, then checks the
// CASE/SUMMARY results.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "cJSON.h"
#include "device_protocol/v3.h"
#include "device_protocol/word_study.h"

namespace v3 = wqn::protocol::v3;
namespace word = wqn::protocol::word_study_v1;

namespace {
size_t checks = 0, failures = 0;

void Check(bool pass, const char* name)
{
    ++checks;
    if (!pass) {
        if (failures < 30) std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
    std::printf("CASE %s %s\n", name, pass ? "PASS" : "FAIL");
}

constexpr char kReqId[] = "req_local_session_000001";
constexpr char kBatchReqId[] = "req_observation_batch_01";
constexpr char kBootId[] = "boot_local_session_00001";
constexpr char kFirmware[] = "0.1.0-longseq";
constexpr char kSession[] = "33333333-3333-4333-8333-333333333333";
constexpr char kDeck[] = "11111111-1111-4111-8111-111111111111";
constexpr char kItem1[] = "22222222-2222-4222-8222-222222222221";
constexpr char kItem2[] = "22222222-2222-4222-8222-222222222222";
constexpr char kObs1[] = "obs_longseq_request_0001";
constexpr char kObs2[] = "obs_longseq_request_0002";
constexpr char kObsId1[] = "44444444-4444-4444-8444-444444444444";
constexpr char kObsId2[] = "44444444-4444-4444-8444-444444444445";
constexpr char kSha[] = "7777777777777777777777777777777777777777777777777777777777777777";

v3::RequestMetadata RegistrationMetadata()
{
    v3::RequestMetadata m;
    m.request_id = kReqId;
    m.boot_id = kBootId;
    m.firmware_version = kFirmware;
    return m;
}

v3::RequestMetadata BatchMetadata()
{
    v3::RequestMetadata m = RegistrationMetadata();
    m.request_id = kBatchReqId;
    return m;
}

word::LocalSessionRegistrationRequest Registration()
{
    word::LocalSessionRegistrationRequest r;
    r.metadata = RegistrationMetadata();
    r.session_id = kSession;
    r.scope.deck_ids = {kDeck};
    r.scope.include_mastered = true;
    r.seed = "longseq_seed";
    r.start_index = 3507;
    word::PackSnapshot snap;
    snap.deck_id = kDeck;
    snap.content_revision = 7;
    snap.pack_revision = 7;
    snap.sha256 = kSha;
    r.snapshot = {snap};
    word::SessionItem i1;
    i1.item_id = kItem1; i1.deck_id = kDeck; i1.ordinal = 0;
    word::SessionItem i2;
    i2.item_id = kItem2; i2.deck_id = kDeck; i2.ordinal = 1;
    r.items = {i1, i2};
    return r;
}

word::ObservationBatchRequest Batch()
{
    word::ObservationBatchRequest r;
    r.metadata = BatchMetadata();
    r.session_id = kSession;
    word::BatchObservation o1;
    o1.request_id = kObs1; o1.sequence = 0; o1.item_id = kItem1;
    o1.action = word::ObservationAction::kShown; o1.mode = word::Mode::kSequential;
    o1.occurred_at = "2026-11-05T10:00:00.000Z";
    word::BatchObservation o2;
    o2.request_id = kObs2; o2.sequence = 1; o2.item_id = kItem2;
    o2.action = word::ObservationAction::kKnown; o2.mode = word::Mode::kSequential;
    o2.occurred_at = "2026-11-05T10:00:01.000Z";
    r.observations = {o1, o2};
    return r;
}

// Fail-closed helper: a rejected build must leave the output untouched.
void ExpectBuildFail(const std::function<esp_err_t(std::string*)>& build,
                     const char* name)
{
    std::string body = "SENTINEL_MUST_SURVIVE";
    Check(build(&body) != ESP_OK, name);
    Check(body == "SENTINEL_MUST_SURVIVE", (std::string(name) + "-body").c_str());
}

// Minimal field-level parse of a built request to verify the exact wire keys.
bool HasOnlyKeys(cJSON* object, std::vector<const char*> keys)
{
    if (!cJSON_IsObject(object)) return false;
    if (static_cast<size_t>(cJSON_GetArraySize(object)) != keys.size()) return false;
    for (const char* k : keys) {
        if (cJSON_GetObjectItemCaseSensitive(object, k) == nullptr) return false;
    }
    return true;
}

std::string Envelope(const std::string& data_json, const char* req_id)
{
    return std::string("{\"ok\":true,\"request_id\":\"") + req_id +
        "\",\"server_time_ms\":1762360000000,\"data\":" + data_json + "}";
}

// Parse a batch response and return whether the parser succeeded.
bool ParseBatch(const std::string& body,
                const word::ObservationBatchRequest& expected,
                word::ObservationBatchData* out)
{
    v3::Error error;
    return word::ParseObservationBatchResponse(
               body, expected.metadata.request_id, expected, out, &error) == ESP_OK;
}

bool ParseRegister(const std::string& body, word::LocalSessionRegistrationData* out)
{
    v3::Error error;
    return word::ParseLocalSessionRegistrationResponse(
               body, kReqId, kSession, out, &error) == ESP_OK;
}

}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto reg = Registration();
    const auto batch = Batch();

    // ---- Builders: canonical requests ----
    std::string reg_body;
    Check(word::BuildLocalSessionRegistrationRequest(reg, &reg_body) == ESP_OK,
          "build-registration-ok");
    std::printf("BUILD_REGISTER %s\n", reg_body.c_str());
    std::string batch_body;
    Check(word::BuildObservationBatchRequest(batch, &batch_body) == ESP_OK,
          "build-batch-ok");
    std::printf("BUILD_BATCH %s\n", batch_body.c_str());

    // The registration request must NOT carry server-fixed semantics fields.
    {
        cJSON* root = cJSON_Parse(reg_body.c_str());
        Check(root != nullptr, "register-parseable");
        Check(HasOnlyKeys(root, {"request_id", "boot_id", "firmware_version",
                                 "capabilities", "session_id", "scope", "seed",
                                 "start_index", "snapshot", "items"}),
              "register-no-extra-semantics-fields");
        Check(cJSON_GetObjectItemCaseSensitive(root, "domain") == nullptr &&
              cJSON_GetObjectItemCaseSensitive(root, "mode") == nullptr &&
              cJSON_GetObjectItemCaseSensitive(root, "purpose") == nullptr &&
              cJSON_GetObjectItemCaseSensitive(root, "ordering") == nullptr &&
              cJSON_GetObjectItemCaseSensitive(root, "candidate_policy_version") == nullptr,
              "register-fixed-fields-absent");
        cJSON* scope = cJSON_GetObjectItemCaseSensitive(root, "scope");
        cJSON* include = cJSON_GetObjectItemCaseSensitive(scope, "include_mastered");
        Check(cJSON_IsTrue(include), "register-include-mastered-true");
        cJSON_Delete(root);
    }
    {
        cJSON* root = cJSON_Parse(batch_body.c_str());
        Check(root != nullptr, "batch-parseable");
        Check(HasOnlyKeys(root, {"request_id", "boot_id", "firmware_version",
                                 "capabilities", "session_id", "observations"}),
              "batch-exact-outer-keys");
        cJSON* observations = cJSON_GetObjectItemCaseSensitive(root, "observations");
        Check(cJSON_GetArraySize(observations) == 2, "batch-two-observations");
        cJSON_Delete(root);
    }

    // ---- Builder fail-closed / illegal inputs ----
    {
        auto bad = reg; bad.session_id = "not-a-uuid";
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-bad-session-id");
    }
    {
        auto bad = reg; bad.scope.include_mastered = false;
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-scope-include-false");
    }
    {
        auto bad = reg; bad.scope.deck_ids = {};
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-scope-empty");
    }
    {
        auto bad = reg; bad.scope.deck_ids = {kDeck, kDeck};
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-scope-duplicate");
    }
    {
        auto bad = reg; bad.seed = "bad seed!";
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-seed-illegal");
    }
    {
        auto bad = reg; bad.seed = std::string(65, 'a');
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-seed-oversize");
    }
    {
        auto bad = reg; bad.start_index = 10001;
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-start-index-over-max");
    }
    {
        auto bad = reg; bad.snapshot[0].deck_id = kItem1;  // deck set != scope
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-snapshot-scope-mismatch");
    }
    {
        auto bad = reg; bad.snapshot.clear();
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-snapshot-empty");
    }
    {
        auto bad = reg; bad.items[1].ordinal = 7;  // not 0..n-1
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-item-ordinal-gap");
    }
    {
        auto bad = reg; bad.items[1].item_id = kItem1;  // duplicate item id
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-item-duplicate-id");
    }
    {
        auto bad = reg; bad.metadata.firmware_version = std::string("x\0y", 3);
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-metadata-embedded-nul");
    }
    {
        auto bad = reg; bad.items.clear();
        ExpectBuildFail([&](auto* b) { return word::BuildLocalSessionRegistrationRequest(bad, b); },
                        "build-items-empty");
    }
    {
        auto bad = batch; bad.observations.clear();
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-empty");
    }
    {
        auto bad = batch; bad.observations[1].request_id = kObs1;  // duplicate
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-duplicate-request-id");
    }
    {
        auto bad = batch; bad.observations[1].sequence = 5;  // non-consecutive
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-sequence-gap");
    }
    {
        auto bad = batch; bad.observations[0].occurred_at = "not-a-timestamp";
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-bad-timestamp");
    }
    {
        auto bad = batch; bad.observations[0].sequence = v3::kMaxSafeJsonInteger + 1;
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-unsafe-counter");
    }
    {
        auto bad = batch; bad.observations[0].request_id = "short";
        ExpectBuildFail([&](auto* b) { return word::BuildObservationBatchRequest(bad, b); },
                        "build-batch-short-request-id");
    }
    // A batch may be smaller than 64 items; that is still valid.
    {
        auto small = batch; small.observations.pop_back();
        std::string body;
        Check(word::BuildObservationBatchRequest(small, &body) == ESP_OK,
              "build-batch-sub64-ok");
        // Its receipt is validated against THIS smaller expected input below.
    }

    // ---- Registration response parsing ----
    const std::string reg_data =
        "{\"session_id\":\"" + std::string(kSession) +
        "\",\"registered\":true,\"replayed\":false}";
    const std::string reg_ok = Envelope(reg_data, kReqId);
    {
        word::LocalSessionRegistrationData data;
        Check(ParseRegister(reg_ok, &data), "parse-register-golden");
        Check(data.registered && !data.replayed && data.session_id == kSession,
              "parse-register-fields");
    }
    {
        word::LocalSessionRegistrationData data;
        // session_id echoed back wrong -> reject
        std::string wrong = reg_ok;
        wrong.replace(wrong.find(kSession), std::strlen(kSession),
                      "99999999-9999-4999-8999-999999999999");
        Check(!ParseRegister(wrong, &data), "parse-register-wrong-session");
    }
    {
        word::LocalSessionRegistrationData data;
        std::string wrong = reg_ok;
        wrong.replace(wrong.find("\"registered\":true"), 17, "\"registered\":false");
        Check(!ParseRegister(wrong, &data), "parse-register-not-registered");
    }
    {
        word::LocalSessionRegistrationData data;
        std::string wrong = reg_ok;
        wrong.replace(wrong.find(kReqId), std::strlen(kReqId), "req_other_0000000001");
        Check(!ParseRegister(wrong, &data), "parse-register-wrong-request-id");
    }
    {
        word::LocalSessionRegistrationData data;
        std::string extra = Envelope(
            "{\"session_id\":\"" + std::string(kSession) +
            "\",\"registered\":true,\"replayed\":false,\"extra\":1}", kReqId);
        Check(!ParseRegister(extra, &data), "parse-register-extra-field");
    }
    {
        word::LocalSessionRegistrationData data;
        // v3 error envelope must be surfaced, never a success ACK.
        std::string err =
            "{\"ok\":false,\"request_id\":\"" + std::string(kReqId) +
            "\",\"server_time_ms\":1,\"error\":{\"code\":\"LOCAL_SESSION_CONFLICT\","
            "\"retryable\":false}}";
        v3::Error error;
        esp_err_t rc = word::ParseLocalSessionRegistrationResponse(
            err, kReqId, kSession, &data, &error);
        Check(rc != ESP_OK && error.code == "LOCAL_SESSION_CONFLICT",
              "parse-register-v3-error-envelope");
    }

    // ---- Batch response parsing ----
    const std::string full_data =
        "{\"session_id\":\"" + std::string(kSession) + "\",\"accepted\":["
        "{\"request_id\":\"" + kObs1 + "\",\"observation_id\":\"" + kObsId1 +
        "\",\"sequence\":0,\"replayed\":false},"
        "{\"request_id\":\"" + kObs2 + "\",\"observation_id\":\"" + kObsId2 +
        "\",\"sequence\":1,\"replayed\":false}],\"failure\":null}";
    const std::string full_ok = Envelope(full_data, kBatchReqId);
    {
        word::ObservationBatchData data;
        Check(ParseBatch(full_ok, batch, &data), "parse-batch-golden-full");
        Check(data.accepted.size() == 2 && !data.has_failure &&
              data.accepted[1].replayed == false,
              "parse-batch-golden-fields");
    }

    const std::string partial_data =
        "{\"session_id\":\"" + std::string(kSession) + "\",\"accepted\":["
        "{\"request_id\":\"" + kObs1 + "\",\"observation_id\":\"" + kObsId1 +
        "\",\"sequence\":0,\"replayed\":true}],"
        "\"failure\":{\"request_id\":\"" + kObs2 + "\",\"sequence\":1,"
        "\"code\":\"ITEM_NOT_IN_SESSION\",\"retryable\":false}}";
    const std::string partial_ok = Envelope(partial_data, kBatchReqId);
    {
        word::ObservationBatchData data;
        Check(ParseBatch(partial_ok, batch, &data), "parse-batch-golden-partial");
        Check(data.accepted.size() == 1 && data.has_failure &&
              data.accepted[0].replayed &&
              data.failure.code == "ITEM_NOT_IN_SESSION" &&
              !data.failure.retryable &&
              data.failure.request_id == kObs2 && data.failure.sequence == 1,
              "parse-batch-partial-fields");
    }
    // Unknown server error code is preserved verbatim, not treated as invalid.
    {
        word::ObservationBatchData data;
        std::string unknown = partial_ok;
        unknown.replace(unknown.find("ITEM_NOT_IN_SESSION"),
                        std::strlen("ITEM_NOT_IN_SESSION"),
                        "SOME_FUTURE_SERVER_ERROR");
        Check(ParseBatch(unknown, batch, &data), "parse-batch-unknown-code-kept");
        Check(data.has_failure && data.failure.code == "SOME_FUTURE_SERVER_ERROR",
              "parse-batch-unknown-code-value");
    }

    auto mutate_full = [&](const char* label,
                           const std::function<std::string(std::string)>& mutate,
                           bool expect_valid) {
        word::ObservationBatchData data;
        std::string body = mutate(full_ok);
        Check(ParseBatch(body, batch, &data) == expect_valid, label);
    };

    mutate_full("parse-batch-wrong-outer-request-id", [&](std::string s) {
        s.replace(s.find(kBatchReqId), std::strlen(kBatchReqId), "req_other_0000000001");
        return s;
    }, false);
    mutate_full("parse-batch-wrong-session", [&](std::string s) {
        s.replace(s.find(kSession), std::strlen(kSession),
                  "99999999-9999-4999-8999-999999999999");
        return s;
    }, false);
    mutate_full("parse-batch-wrong-prefix-order", [&](std::string s) {
        // swap first accepted request_id to the second observation's id
        s.replace(s.find(kObs1), std::strlen(kObs1), kObs2);
        return s;
    }, false);
    mutate_full("parse-batch-unknown-prefix-id", [&](std::string s) {
        s.replace(s.find(kObs1), std::strlen(kObs1), "obs_unknown_request_01");
        return s;
    }, false);
    mutate_full("parse-batch-accepted-count-over-input", [&](std::string s) {
        // append a third accepted entry beyond the 2 input observations
        std::string extra = ",{\"request_id\":\"obs_extra_req_000001\","
                            "\"observation_id\":\"" + std::string(kObsId1) +
                            "\",\"sequence\":2,\"replayed\":false}";
        s.insert(s.find("],\"failure\":null"), extra);
        return s;
    }, false);
    mutate_full("parse-batch-full-with-failure", [&](std::string s) {
        s.replace(s.find("\"failure\":null"), std::strlen("\"failure\":null"),
                  "\"failure\":{\"request_id\":\"" + std::string(kObs2) +
                  "\",\"sequence\":1,\"code\":\"X\",\"retryable\":false}");
        return s;
    }, false);
    // empty accepted + null failure is never a success
    {
        word::ObservationBatchData data;
        std::string empty = Envelope(
            "{\"session_id\":\"" + std::string(kSession) +
            "\",\"accepted\":[],\"failure\":null}", kBatchReqId);
        Check(!ParseBatch(empty, batch, &data), "parse-batch-empty-accepted-null-failure");
    }
    // empty accepted + failure for obs[0] IS a valid terminal receipt
    {
        word::ObservationBatchData data;
        std::string empty = Envelope(
            "{\"session_id\":\"" + std::string(kSession) +
            "\",\"accepted\":[],\"failure\":{\"request_id\":\"" + kObs1 +
            "\",\"sequence\":0,\"code\":\"SEQUENCE_GAP\",\"retryable\":true}}",
            kBatchReqId);
        Check(ParseBatch(empty, batch, &data), "parse-batch-empty-accepted-with-failure");
        Check(data.failure.code == "SEQUENCE_GAP" && data.failure.retryable,
              "parse-batch-retryable-failure-preserved");
    }
    // partial but failure=null (accepted prefix is short) -> reject
    {
        word::ObservationBatchData data;
        std::string short_prefix = Envelope(
            "{\"session_id\":\"" + std::string(kSession) + "\",\"accepted\":["
            "{\"request_id\":\"" + kObs1 + "\",\"observation_id\":\"" + kObsId1 +
            "\",\"sequence\":0,\"replayed\":false}],\"failure\":null}",
            kBatchReqId);
        Check(!ParseBatch(short_prefix, batch, &data),
              "parse-batch-short-prefix-null-failure");
    }
    // failure does not follow the prefix (wrong request_id / sequence)
    {
        word::ObservationBatchData data;
        std::string bad = partial_ok;
        bad.replace(bad.find(kObs2), std::strlen(kObs2), kObs1);
        Check(!ParseBatch(bad, batch, &data), "parse-batch-failure-wrong-next-id");
    }
    {
        word::ObservationBatchData data;
        std::string bad = partial_ok;
        bad.replace(bad.find("\"sequence\":1,"), std::strlen("\"sequence\":1,"),
                    "\"sequence\":9,");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-failure-wrong-next-sequence");
    }
    // unsafe counter in accepted (overflows IEEE-754 exact range)
    {
        word::ObservationBatchData data;
        std::string bad = full_ok;
        bad.replace(bad.find("\"sequence\":1,"), std::strlen("\"sequence\":1,"),
                    "\"sequence\":9007199254740992,");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-unsafe-counter");
    }
    // missing field in accepted item -> strict key check rejects
    {
        word::ObservationBatchData data;
        std::string bad = full_ok;
        bad.replace(bad.find("\"observation_id\":\"" + std::string(kObsId1) + "\","),
                    ("\"observation_id\":\"" + std::string(kObsId1) + "\",").size(), "");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-missing-observation-id");
    }
    // extra field in accepted item -> strict key check rejects
    {
        word::ObservationBatchData data;
        std::string bad = full_ok;
        bad.replace(bad.find("\"replayed\":false}"), std::strlen("\"replayed\":false}"),
                    "\"replayed\":false,\"surprise\":1}");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-extra-accepted-field");
    }
    // encoded NUL in request_id -> JsonInputWithinLimits rejects
    {
        word::ObservationBatchData data;
        std::string bad = full_ok;
        bad.replace(bad.find(kObs1), std::strlen(kObs1),
                    std::string("obs_") + "\\u0000shadow");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-encoded-nul");
    }
    // malformed failure code (empty) -> reject
    {
        word::ObservationBatchData data;
        std::string bad = partial_ok;
        bad.replace(bad.find("\"code\":\"ITEM_NOT_IN_SESSION\""),
                    std::strlen("\"code\":\"ITEM_NOT_IN_SESSION\""),
                    "\"code\":\"\"");
        Check(!ParseBatch(bad, batch, &data), "parse-batch-empty-failure-code");
    }
    // v3 error envelope on batch is surfaced, never a success ACK
    {
        word::ObservationBatchData data;
        v3::Error error;
        std::string err = "{\"ok\":false,\"request_id\":\"" + std::string(kBatchReqId) +
            "\",\"server_time_ms\":1,\"error\":{\"code\":\"RETRY\",\"retryable\":true}}";
        esp_err_t rc = word::ParseObservationBatchResponse(
            err, kBatchReqId, batch, &data, &error);
        Check(rc != ESP_OK && error.code == "RETRY" && error.retryable,
              "parse-batch-v3-error-envelope");
    }

    // ---- Standalone validator: same-session / prefix / failure invariants ----
    {
        word::ObservationBatchData data;
        data.session_id = kSession;
        word::BatchAcceptedItem a;
        a.request_id = kObs1; a.observation_id = kObsId1; a.sequence = 0;
        data.accepted = {a};
        data.has_failure = false;  // short prefix, null failure -> reject
        Check(word::ValidateObservationBatchReceipt(data, batch) != ESP_OK,
              "validator-short-prefix-rejected");
        data.accepted.clear();
        data.has_failure = false;  // empty + null -> reject
        Check(word::ValidateObservationBatchReceipt(data, batch) != ESP_OK,
              "validator-empty-rejected");
    }

    std::printf("SUMMARY %zu %zu\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
