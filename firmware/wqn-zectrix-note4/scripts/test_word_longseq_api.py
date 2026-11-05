#!/usr/bin/env python3
"""Host verification for the two long-sequence wqn_api HTTP wrappers.

Extracts the REAL production RegisterLocalWordStudySessionV1 and
SubmitWordStudyObservationBatchV1 function bodies from main/wqn_api.cpp and
compiles them against the REAL protocol codec (word_study.cpp / word_study.h /
v3.h) and the REAL SDK cJSON under ASan/UBSan. Only the ESP/HTTP/network ports
are stubbed, with a controllable fake HttpRequest. This proves:
  * v3 header / request-id passthrough and bounded HTTP helper usage,
  * only a true HTTP 401 clears the token (403/404/429/5xx/timeout keep it),
  * the exact expected input is passed to the parser so a fully validated
    receipt is the only ACK-able ESP_OK result,
  * a malformed receipt is never returned as success.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError(f'production boundary drift: {signature}')
    start = source.index(signature)
    body = source.index('{', start)
    depth = 0
    for end in range(body, len(source)):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError(f'unclosed function: {signature}')


CPP_PRELUDE = r'''
#include <cstdio>
#include <cstring>
#include <string>
#include "esp_err.h"
#include "cJSON.h"
#include "device_protocol/v3.h"
#include "device_protocol/word_study.h"

namespace v3 = wqn::protocol::v3;
namespace word = wqn::protocol::word_study_v1;
// wqn_api.cpp refers to the protocol tree through this alias.
namespace protocol = wqn::protocol;

#define ESP_RETURN_ON_ERROR(expr, ...) do { const esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); } while (0)

constexpr char kTag[] = "wqn_api";
constexpr int kHttpTimeoutMs = 10000;
constexpr int kWordSessionHttpTimeoutMs = 30000;

// ---- Controllable HTTP / service seams ----
struct FakeHttp {
    esp_err_t http_result = ESP_OK;
    int status_code = 200;
    std::string response_body;
    std::string seen_method, seen_url, seen_protocol, seen_request_id;
    bool post = false;
};
static FakeHttp g_http;
static bool g_token_cleared = false;
static bool g_token_validated = false;

static std::string BuildUrl(const std::string& path) { return "https://api" + path; }

static esp_err_t HttpRequest(
    const char* method, const std::string& url,
    const std::string* bearer_token, const std::string* request_body,
    int* status_code, std::string* response_body,
    const char* protocol = nullptr, const std::string* request_id = nullptr,
    int timeout_ms = kHttpTimeoutMs)
{
    (void)bearer_token; (void)request_body; (void)timeout_ms;
    g_http.seen_method = method;
    g_http.seen_url = url;
    g_http.seen_protocol = protocol ? protocol : "";
    g_http.seen_request_id = request_id ? *request_id : "";
    g_http.post = std::strcmp(method, "POST") == 0;
    if (g_http.http_result != ESP_OK) return g_http.http_result;
    *status_code = g_http.status_code;
    *response_body = g_http.response_body;
    return ESP_OK;
}

namespace wqn {
@@TOKEN_SHAPE@@
}
static esp_err_t WaitForNetworkReadyForHttps() { return ESP_OK; }
static esp_err_t ClearTokenOnUnauthorized(const char* op)
{
    (void)op;
    g_token_cleared = true;
    return ESP_ERR_INVALID_STATE;
}
@@HTTP_NORMALIZE@@

// ---- Extracted production wrappers ----
@@REGISTER@@
@@BATCH@@

// ---- Test driver ----
static int g_checks = 0, g_failures = 0;
static void Check(bool pass, const char* name)
{
    ++g_checks;
    if (!pass) { if (g_failures < 20) std::fprintf(stderr, "FAIL %s\n", name); ++g_failures; }
}

static const char* kSession = "33333333-3333-4333-8333-333333333333";
static const char* kReqId = "req_local_session_000001";
static const char* kBatchReqId = "req_observation_batch_01";
static const char* kDeck = "11111111-1111-4111-8111-111111111111";
static const char* kItem1 = "22222222-2222-4222-8222-222222222221";
static const char* kItem2 = "22222222-2222-4222-8222-222222222222";
static const char* kObs1 = "obs_longseq_request_0001";
static const char* kObs2 = "obs_longseq_request_0002";
static const char* kObsId1 = "44444444-4444-4444-8444-444444444444";
static const char* kObsId2 = "44444444-4444-4444-8444-444444444445";

static std::string Envelope(const std::string& data, const char* req_id)
{
    return std::string("{\"ok\":true,\"request_id\":\"") + req_id +
        "\",\"server_time_ms\":1762360000000,\"data\":" + data + "}";
}

static v3::RequestMetadata RegMeta()
{
    v3::RequestMetadata m;
    m.request_id = kReqId; m.boot_id = "boot_local_session_00001";
    m.firmware_version = "0.1.0-longseq";
    return m;
}

static word::LocalSessionRegistrationRequest RegRequest()
{
    word::LocalSessionRegistrationRequest r;
    r.metadata = RegMeta(); r.session_id = kSession;
    r.scope.deck_ids = {kDeck}; r.scope.include_mastered = true;
    r.seed = "longseq_seed"; r.start_index = 3507;
    word::PackSnapshot s;
    s.deck_id = kDeck; s.content_revision = 7; s.pack_revision = 7;
    s.sha256 = std::string(64, '7');
    r.snapshot = {s};
    word::SessionItem i1; i1.item_id = kItem1; i1.deck_id = kDeck; i1.ordinal = 0;
    word::SessionItem i2; i2.item_id = kItem2; i2.deck_id = kDeck; i2.ordinal = 1;
    r.items = {i1, i2};
    return r;
}

static word::ObservationBatchRequest BatchRequest()
{
    word::ObservationBatchRequest r;
    r.metadata = RegMeta(); r.metadata.request_id = kBatchReqId;
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

static std::string RegOkResponse()
{
    return Envelope(std::string("{\"session_id\":\"") + kSession +
                    "\",\"registered\":true,\"replayed\":false}", kReqId);
}
static std::string BatchOkResponse()
{
    return Envelope(
        std::string("{\"session_id\":\"") + kSession + "\",\"accepted\":["
        "{\"request_id\":\"" + kObs1 + "\",\"observation_id\":\"" + kObsId1 +
        "\",\"sequence\":0,\"replayed\":false},"
        "{\"request_id\":\"" + kObs2 + "\",\"observation_id\":\"" + kObsId2 +
        "\",\"sequence\":1,\"replayed\":false}],\"failure\":null}", kBatchReqId);
}

static void Reset()
{
    g_http = FakeHttp{};
    g_token_cleared = false;
    g_token_validated = false;
}

int main()
{
    const auto reg = RegRequest();
    const auto batch = BatchRequest();

    // Registration: 200 + golden -> ESP_OK, token kept, v3 header / id / url.
    Reset();
    g_http.response_body = RegOkResponse();
    {
        word::LocalSessionRegistrationData data;
        v3::Error error;
        esp_err_t rc = RegisterLocalWordStudySessionV1(std::string(64, 'a'), reg, &data, &error);
        Check(rc == ESP_OK, "reg-200-ok");
        Check(data.registered && data.session_id == kSession, "reg-200-fields");
        Check(!g_token_cleared, "reg-200-keeps-token");
        Check(g_http.post && g_http.seen_protocol == std::string("3"), "reg-post-v3-header");
        Check(g_http.seen_request_id == kReqId, "reg-request-id-header");
        Check(g_http.seen_url.find("/v3/words/sessions/local") != std::string::npos,
              "reg-url");
    }

    // 401 clears the token (the only status that does).
    Reset();
    g_http.status_code = 401;
    g_http.response_body = "{}";
    {
        word::LocalSessionRegistrationData data;
        v3::Error error;
        esp_err_t rc = RegisterLocalWordStudySessionV1(std::string(64, 'a'), reg, &data, &error);
        Check(rc != ESP_OK, "reg-401-fails");
        Check(g_token_cleared, "reg-401-clears-token");
    }

    // 403/404/429/5xx keep the token.
    for (int status : {403, 404, 429, 500, 503}) {
        Reset();
        g_http.status_code = status;
        g_http.response_body =
            "{\"ok\":false,\"request_id\":\"" + std::string(kReqId) +
            "\",\"server_time_ms\":1,\"error\":{\"code\":\"E\",\"retryable\":true}}";
        word::LocalSessionRegistrationData data;
        v3::Error error;
        esp_err_t rc = RegisterLocalWordStudySessionV1(std::string(64, 'a'), reg, &data, &error);
        Check(rc != ESP_OK, "reg-non401-fails");
        Check(!g_token_cleared, "reg-non401-keeps-token");
    }

    // Transport timeout keeps the token.
    Reset();
    g_http.http_result = ESP_ERR_TIMEOUT;
    {
        word::LocalSessionRegistrationData data;
        v3::Error error;
        esp_err_t rc = RegisterLocalWordStudySessionV1(std::string(64, 'a'), reg, &data, &error);
        Check(rc == ESP_ERR_TIMEOUT, "reg-timeout-propagates");
        Check(!g_token_cleared, "reg-timeout-keeps-token");
    }

    // Registration: server echoes a DIFFERENT session -> never an ACK.
    Reset();
    g_http.response_body = Envelope(
        "{\"session_id\":\"99999999-9999-4999-8999-999999999999\","
        "\"registered\":true,\"replayed\":false}", kReqId);
    {
        word::LocalSessionRegistrationData data;
        v3::Error error;
        esp_err_t rc = RegisterLocalWordStudySessionV1(std::string(64, 'a'), reg, &data, &error);
        Check(rc != ESP_OK, "reg-wrong-session-rejected");
        Check(!data.registered, "reg-wrong-session-no-ack");
    }

    // Batch: 200 + golden -> ESP_OK, token kept.
    Reset();
    g_http.response_body = BatchOkResponse();
    {
        word::ObservationBatchData data;
        v3::Error error;
        esp_err_t rc = SubmitWordStudyObservationBatchV1(std::string(64, 'a'), batch, &data, &error);
        Check(rc == ESP_OK, "batch-200-ok");
        Check(data.accepted.size() == 2 && !data.has_failure, "batch-200-fields");
        Check(!g_token_cleared, "batch-200-keeps-token");
        Check(g_http.seen_request_id == kBatchReqId, "batch-request-id-header");
        Check(g_http.seen_url.find("/v3/words/observations/batch") != std::string::npos,
              "batch-url");
    }

    // Batch: 401 clears the token.
    Reset();
    g_http.status_code = 401;
    {
        word::ObservationBatchData data;
        v3::Error error;
        esp_err_t rc = SubmitWordStudyObservationBatchV1(std::string(64, 'a'), batch, &data, &error);
        Check(rc != ESP_OK, "batch-401-fails");
        Check(g_token_cleared, "batch-401-clears-token");
    }

    // Batch: 403/429/5xx keep the token.
    for (int status : {403, 429, 500}) {
        Reset();
        g_http.status_code = status;
        g_http.response_body = "{}";
        word::ObservationBatchData data;
        v3::Error error;
        esp_err_t rc = SubmitWordStudyObservationBatchV1(std::string(64, 'a'), batch, &data, &error);
        Check(rc != ESP_OK, "batch-non401-fails");
        Check(!g_token_cleared, "batch-non401-keeps-token");
    }

    // Batch: malformed receipt (wrong prefix) -> never an ACK, token kept.
    Reset();
    g_http.response_body = BatchOkResponse();
    // corrupt the first accepted request_id so the prefix no longer matches
    {
        std::string bad = g_http.response_body;
        bad.replace(bad.find(kObs1), std::strlen(kObs1), "obs_unknown_request_01");
        g_http.response_body = bad;
        word::ObservationBatchData data;
        v3::Error error;
        esp_err_t rc = SubmitWordStudyObservationBatchV1(std::string(64, 'a'), batch, &data, &error);
        Check(rc != ESP_OK, "batch-malformed-rejected");
        Check(data.accepted.empty(), "batch-malformed-no-ack");
        Check(!g_token_cleared, "batch-malformed-keeps-token");
    }

    // Batch: timeout keeps the token.
    Reset();
    g_http.http_result = ESP_ERR_TIMEOUT;
    {
        word::ObservationBatchData data;
        v3::Error error;
        esp_err_t rc = SubmitWordStudyObservationBatchV1(std::string(64, 'a'), batch, &data, &error);
        Check(rc == ESP_ERR_TIMEOUT, "batch-timeout-propagates");
        Check(!g_token_cleared, "batch-timeout-keeps-token");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
'''


def main():
    root = Path(__file__).resolve().parents[1]
    api = (root / 'main/wqn_api.cpp').read_text()
    source_api = api
    register_fn = function(api, 'esp_err_t RegisterLocalWordStudySessionV1(')
    batch_fn = function(api, 'esp_err_t SubmitWordStudyObservationBatchV1(')
    register_fn = register_fn.replace('int* http_status,', 'int* http_status = nullptr,').replace('bool* transport_failure)', 'bool* transport_failure = nullptr)')
    batch_fn = batch_fn.replace('int* http_status,', 'int* http_status = nullptr,').replace('bool* transport_failure)', 'bool* transport_failure = nullptr)')
    token_src = (root / 'main/storage.cpp').read_text()
    token_guard = function(token_src, 'bool IsValidAccessToken(').replace('kAccessTokenLength', '64')
    normalize = function(source_api, 'constexpr bool IsTransientHttpStatus(') + '\n' + function(source_api, 'void NormalizeProtocolHttpError(')
    source = (CPP_PRELUDE.replace('@@REGISTER@@', register_fn).replace('@@BATCH@@', batch_fn)
              .replace('@@TOKEN_SHAPE@@', token_guard).replace('@@HTTP_NORMALIZE@@', normalize))

    schema_path = root / 'contracts/word-study-v1/word-study-v1.schema.json'
    digest = hashlib.sha256(schema_path.read_bytes()).hexdigest()
    cjson = Path(os.environ.get('IDF_PATH', '/home/unknow/esp/esp-idf-v5.5')) / 'components/json/cJSON'
    component = root / 'components/device_protocol'

    with tempfile.TemporaryDirectory(prefix='wqn-longseq-api-') as directory:
        tmp = Path(directory)
        (tmp / 'esp_err.h').write_text(
            '#pragma once\nusing esp_err_t = int;\n'
            'constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101,\n'
            '    ESP_ERR_INVALID_ARG=0x102, ESP_ERR_INVALID_STATE=0x103,\n'
            '    ESP_ERR_NOT_FOUND=0x105, ESP_ERR_NOT_SUPPORTED=0x106, ESP_ERR_TIMEOUT=0x107,\n'
            '    ESP_ERR_INVALID_RESPONSE=0x108, ESP_ERR_INVALID_SIZE=0x104;\n')
        (tmp / 'esp_check.h').write_text(
            '#pragma once\n#include "esp_err.h"\n'
            '#define ESP_RETURN_ON_ERROR(expr, ...) do { const esp_err_t e_ = (expr); '
            'if (e_ != ESP_OK) return e_; } while (0)\n')
        (tmp / 'test.cpp').write_text(source)
        defines = [
            f'-DWQN_WORD_STUDY_SCHEMA_SHA256="{digest}"',
            f'-DWQN_DEVICE_CONTROL_SCHEMA_SHA256="{digest}"',
        ]
        sanitizer = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-fno-omit-frame-pointer']
        binary = tmp / 'test'
        subprocess.run([
            'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *sanitizer,
            '-I' + str(tmp), '-I' + str(component / 'include'), '-I' + str(cjson),
            *defines,
            str(tmp / 'test.cpp'), str(component / 'word_study.cpp'),
            str(cjson / 'cJSON.c'), '-o', str(binary),
        ], check=True, timeout=120)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1')
        proc = subprocess.run([str(binary)], capture_output=True, text=True,
                              env=env, timeout=60)
        sys.stderr.buffer.write(proc.stderr.encode())
        print(proc.stdout.strip())
        if proc.returncode != 0:
            raise SystemExit(f'FAIL: wrapper host test exited {proc.returncode}')
        print('ALL LONG-SEQUENCE API WRAPPER HOST CHECKS PASSED')
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
