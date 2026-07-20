#include "contract_fixtures.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "device_protocol/v3.h"
#include "device_protocol/word_study.h"
#include "esp_log.h"
#include "text_render.h"
#include "ui/confirm_gesture_arbiter.h"
#include "ui/status_control.h"
#include "wqn_api.h"
#include "wqn_api_stream_internal.h"

namespace {

constexpr char kTag[] = "wqn_contract";

const char kV3Bootstrap[] = R"json({
  "ok": true,
  "request_id": "req_bootstrap_0001",
  "server_time_ms": 1784426400000,
  "data": {
    "device_id": "22222222-2222-4222-8222-222222222222",
    "config_revision": 7,
    "sync_cursor": 42,
    "media_protocols": {
      "ai_sse": "v2-streaming",
      "flash": "wqn-flash-v2"
    }
  }
})json";

const char kV3Sync[] = R"json({
  "ok": true,
  "request_id": "req_sync_000000001",
  "server_time_ms": 1784426400000,
  "data": {
    "config_revision": 7,
    "sync_cursor": 43,
    "configuration": { "auto_sync_interval_minutes": 60 },
    "summaries": {
      "due_problem_ids": ["33333333-3333-4333-8333-333333333333"],
      "todo_count": 2,
      "word_due_count": 5
    },
    "content_manifest": []
  }
})json";

const char kV3UnsafeCounter[] = R"json({
  "ok": true,
  "request_id": "req_bootstrap_0002",
  "server_time_ms": 1784426400000,
  "data": {
    "device_id": "22222222-2222-4222-8222-222222222222",
    "config_revision": 9007199254740992,
    "sync_cursor": 42,
    "media_protocols": {
      "ai_sse": "v2-streaming",
      "flash": "wqn-flash-v2"
    }
  }
})json";

const char kV3Error[] = R"json({
  "ok": false,
  "request_id": "req_sync_000000002",
  "error": {
    "code": "TEMPORARILY_UNAVAILABLE",
    "retryable": true,
    "retry_after_ms": 10000
  }
})json";

const char kWordSessionV1[] = R"json({
  "ok": true,
  "request_id": "req_word_session_0001",
  "server_time_ms": 1784512800000,
  "data": {
    "session_id": "22222222-2222-4222-8222-222222222222",
    "domain": "word",
    "mode": "random",
    "purpose": "study",
    "ordering": "guided_random_v1",
    "candidate_policy_version": "guided_random_v1",
    "seed": "seed_contract_001",
    "scope": {
      "deck_ids": ["11111111-1111-4111-8111-111111111111"],
      "include_mastered": false
    },
    "optional_count": 20,
    "next_sequence": 0,
    "progress_revision": 17,
    "snapshot": [{
      "deck_id": "11111111-1111-4111-8111-111111111111",
      "content_revision": 9,
      "pack_revision": 9,
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }],
    "items": [{
      "item_id": "33333333-3333-4333-8333-333333333333",
      "deck_id": "11111111-1111-4111-8111-111111111111",
      "ordinal": 0
    }],
    "cursor": "1",
    "has_more": false
  }
})json";

const char kWordCandidatePageV1[] = R"json({
  "ok": true,
  "request_id": "req_word_candidates_0001",
  "server_time_ms": 1784512800000,
  "data": {
    "session_id": "22222222-2222-4222-8222-222222222222",
    "ordering": "guided_random_v1",
    "candidate_policy_version": "guided_random_v1",
    "seed": "seed_contract_001",
    "snapshot": [{
      "deck_id": "11111111-1111-4111-8111-111111111111",
      "content_revision": 9,
      "pack_revision": 9,
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    }],
    "progress_revision": 17,
    "cursor": "32",
    "next_cursor": "33",
    "items": [{
      "item_id": "44444444-4444-4444-8444-444444444444",
      "deck_id": "11111111-1111-4111-8111-111111111111",
      "ordinal": 32
    }],
    "has_more": true
  }
})json";

const char kWordObservationV1[] = R"json({
  "ok": true,
  "request_id": "req_word_observe_0001",
  "server_time_ms": 1784512801000,
  "data": {
    "observation_id": "44444444-4444-4444-8444-444444444444",
    "session_id": "22222222-2222-4222-8222-222222222222",
    "sequence": 0,
    "item_id": "33333333-3333-4333-8333-333333333333",
    "action": "unknown",
    "progress": {
      "status": "learning",
      "due_at": "2026-07-20T03:20:00.000Z",
      "reviewed_count": 1,
      "known_count": 0,
      "unknown_count": 1
    },
    "replayed": false
  }
})json";

const char kWordManifestV1[] = R"json({
  "ok": true,
  "request_id": "req_word_manifest_001",
  "server_time_ms": 1784512802000,
  "data": {
    "cursor": "17",
    "has_more": false,
    "decks": [{
      "deck_id": "11111111-1111-4111-8111-111111111111",
      "title": "WQN 预设词库",
      "change_sequence": 17,
      "content_revision": 9,
      "deleted": false,
      "pack": {
        "pack_id": "55555555-5555-4555-8555-555555555555",
        "pack_revision": 9,
        "schema_version": 2,
        "format": "jsonl",
        "compression": "none",
        "entry_count": 1,
        "byte_size": 512,
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "download_url": "/api/esp32/v3/words/packs/55555555-5555-4555-8555-555555555555"
      }
    }]
  }
})json";

const char kWordManifestUnsafeCursorV1[] = R"json({
  "ok": true,
  "request_id": "req_word_manifest_002",
  "server_time_ms": 1784512802000,
  "data": {"cursor":"9007199254740992","has_more":false,"decks":[]}
})json";

const char kV3ClaimStart[] = R"json({
  "ok": true,
  "request_id": "req_claim_start_0001",
  "server_time_ms": 1784426400000,
  "data": {
    "claim_id": "11111111-1111-4111-8111-111111111111",
    "display_code": "31415926",
    "expires_at_ms": 1784427000000,
    "poll_interval_ms": 3000
  }
})json";

const char kV3ClaimPending[] = R"json({
  "ok": true,
  "request_id": "req_claim_poll_0000",
  "server_time_ms": 1784426401000,
  "data": {
    "status": "pending",
    "poll_interval_ms": 3000
  }
})json";

const char kV3ClaimApproved[] = R"json({
  "ok": true,
  "request_id": "req_claim_poll_0001",
  "server_time_ms": 1784426403000,
  "data": {
    "status": "approved",
    "sealed_credential": {
      "server_public_key": "BBDnMwKxCsA1KvHzI0dBZKXH8nHx3oWyBzF9owKtZcV2e3ixYp0yb6M8SQrtQmReGkE_bjHgJxQkj7nJlFJvVn0",
      "salt": "ERERERERQRGBEREREREREQ",
      "iv": "IiIiIiIiIiIiIiIi",
      "ciphertext": "MzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMz"
    }
  }
})json";

const char kPollPaired[] = R"json({
  "success": true,
  "data": {
    "status": "paired",
    "access_token": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kPollNoPending[] = R"json({
  "success": true,
  "data": { "status": "no_pending" },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kPollAlreadyPaired[] = R"json({
  "success": true,
  "data": {
    "status": "already_paired",
    "device_name": "ZecTrix_Note4",
    "message": "Device is already paired. Unpair from the web before requesting a new token."
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kSyncDue[] = R"json({
  "success": true,
  "data": {
    "due_problems": [
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222"
    ],
    "total": 2
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kProblemDetails[] = R"json({
  "success": true,
  "data": {
    "problems": [
      {
        "id": "11111111-1111-4111-8111-111111111111",
        "title": "Linear equation",
        "content": "<p>Solve <strong>x + 2 = 5</strong>.</p>",
        "content_format": "esp32_text_v1",
        "content_text": "Solve x + 2 = 5.",
        "problem_type": "short",
        "answer_config": { "mode": "text" },
        "solution_text": "x = 3",
        "assets": [
          {
            "role": "problem",
            "kind": "image",
            "mime_type": "image/png",
            "url": "https://wqn.helema.cn/api/esp32/assets?path=user%2Fdemo%2Fproblems%2Fp1%2Fproblem%2Fscan.png",
            "sha256": "",
            "width": 0,
            "height": 0,
            "bytes": 0
          }
        ]
      }
    ]
  },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kProblemWithMath[] = R"json({
  "success": true,
  "data": {
    "problems": [
      {
        "id": "33333333-3333-4333-8333-333333333333",
        "title": "Quadratic",
        "content_format": "esp32_text_v1",
        "content_text": "求 x^2 >= 4 的解。",
        "problem_type": "short",
        "solution_text": "x <= -2 或 x >= 2"
      }
    ]
  }
})json";

const char kReviewComplete[] = R"json({
  "success": true,
  "data": { "processed": 1 },
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

const char kTodoList[] = R"json({
  "success": true,
  "data": {
    "todos": [
      {
        "id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "title": "Earlier review",
        "status": "pending",
        "priority": "normal",
        "due_at": "2026-06-01T08:00:00.000Z",
        "reminder_at": null,
        "subject_name": "English",
        "updated_at": "2026-06-01T00:00:00.000Z"
      },
      {
        "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "pending",
        "priority": "normal",
        "due_at": "2026-06-02T12:00:00.000Z",
        "reminder_at": null,
        "subject_name": "Math",
        "updated_at": "2026-06-01T00:00:00.000Z",
        "ignored_future_field": true
      }
    ],
    "previous_cursor": "prev-window",
    "next_cursor": "next-window",
    "has_earlier": true,
    "has_later": true,
    "has_more": true,
    "total": 2,
    "selected_todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
    "selected_index": 1,
    "server_time": "2026-06-01T00:00:00.000Z"
  }
})json";

const char kTodoComplete[] = R"json({
  "success": true,
  "data": {
    "todo": {
      "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
      "title": "Review algebra",
      "status": "completed",
      "completed_at": "2026-06-01T00:00:00.000Z"
    }
  }
})json";

const char kTodoListWithoutSelection[] = R"json({
  "success": true,
  "data": {
    "todos": [
      {
        "id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "title": "Earlier review",
        "status": "pending",
        "due_at": "2026-06-01T08:00:00.000Z",
        "updated_at": "2026-06-01T00:00:00.000Z"
      },
      {
        "id": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "title": "Future nearest",
        "status": "pending",
        "due_at": "2026-06-01T10:30:00.000Z",
        "updated_at": "2026-06-01T00:00:00.000Z"
      },
      {
        "id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "title": "Future later",
        "status": "pending",
        "due_at": "2026-06-01T18:00:00.000Z",
        "updated_at": "2026-06-01T00:00:00.000Z"
      }
    ],
    "total": 3,
    "server_time": "2026-06-01T10:00:00.000Z"
  }
})json";

const char kAiTodoActions[] = R"json({
  "success": true,
  "data": {
    "transcript": "Add a todo",
    "reply_text": "Done",
    "conversation_id": "conversation-id",
    "latency_ms": 123,
    "actions": [
      {
        "type": "todo_created",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "pending",
        "due_at": "2026-06-02T12:00:00.000Z",
        "reminder_at": null,
        "unknown": "ignored"
      },
      {
        "type": "todo_status_updated",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "completed"
      },
      {
        "type": "todo_status_updated",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "pending"
      },
      {
        "type": "todo_status_updated",
        "todo_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "title": "Review algebra",
        "status": "cancelled"
      },
      {
        "type": "future_action",
        "title": "Ignored but parsed"
      }
    ]
  }
})json";

const char kWordSync[] = R"json({
  "success": true,
  "data": {
    "cursor": "cursor-2",
    "server_time": "2026-06-06T00:00:00.000Z",
    "decks": [
      {
        "id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "title": "Core Words",
        "source": "system",
        "language": "en",
        "target_language": "zh-CN",
        "is_system": true,
        "revision": 3,
        "deleted": false,
        "ignored_future_field": true
      },
      {
        "title": "Missing id"
      }
    ],
    "entries": [
      {
        "id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "deck_id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "word": "confirm",
        "phonetic": "/confirm/",
        "meaning": "verify",
        "example": "Please confirm your choice.",
        "example_translation": "Confirm it.",
        "revision": 2,
        "deleted": false
      },
      {
        "id": "ffffffff-ffff-4fff-8fff-ffffffffffff",
        "word": "broken"
      }
    ],
    "progress": [
      {
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "status": "learning",
        "due_at": "2026-06-06T00:00:00.000Z",
        "correct_streak": 0,
        "lapses": 1,
        "revision": 4
      },
      {
        "status": "new"
      }
    ]
  }
})json";

const char kWordReviewQueue[] = R"json({
  "success": true,
  "data": {
    "mode": "sequential",
    "daily_target": 20,
    "reviewed_today": 8,
    "due_count": 12,
    "words": [
      {
        "id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "consistent",
        "phonetic": "/consistent/",
        "meaning": "steady",
        "example": "Keep a consistent study habit.",
        "example_translation": "Keep it steady.",
        "status": "new"
      },
      {
        "word": "missing-id",
        "meaning": "bad item"
      }
    ]
  }
})json";

const char kWordReviewSubmit[] = R"json({
  "success": true,
  "data": {
    "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
    "status": "learning",
    "due_at": "2026-06-06T00:00:00.000Z",
    "actions": [
      {
        "type": "word_review_recorded",
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "consistent",
        "status": "learning",
        "outcome": "unknown",
        "due_at": "2026-06-06T00:00:00.000Z"
      },
      {
        "type": "word_added_to_mistakes",
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "consistent",
        "problem_set_id": "99999999-9999-4999-8999-999999999999",
        "problem_id": "88888888-8888-4888-8888-888888888888",
        "title": "consistent"
      }
    ]
  }
})json";

const char kWordSearch[] = R"json({
  "success": true,
  "data": {
    "prefix": "co",
    "words": [
      {
        "id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "concept",
        "phonetic": "/concept/",
        "meaning": "idea"
      },
      {
        "id": "ffffffff-ffff-4fff-8fff-ffffffffffff",
        "word": "broken"
      }
    ],
    "next_letters": ["m", "n", "r"]
  }
})json";

const char kAiWordActions[] = R"json({
  "success": true,
  "data": {
    "transcript": "Review confirm",
    "reply_text": "Recorded",
    "conversation_id": "conversation-id",
    "actions": [
      {
        "type": "word_review_recorded",
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "confirm",
        "status": "learning",
        "outcome": "unknown",
        "due_at": "2026-06-06T00:00:00.000Z"
      },
      {
        "type": "word_added_to_mistakes",
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "confirm",
        "problem_set_id": "99999999-9999-4999-8999-999999999999",
        "problem_id": "88888888-8888-4888-8888-888888888888",
        "title": "confirm"
      },
      {
        "type": "word_deck_created",
        "deck_id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "title": "My Words"
      },
      {
        "type": "word_added_to_deck",
        "deck_id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "word_id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "word": "confirm",
        "title": "confirm"
      },
      {
        "type": "future_word_action",
        "word": "ignored"
      }
    ]
  }
})json";

const char kUnauthorizedError[] = R"json({
  "error": "invalid token",
  "status": 401,
  "timestamp": "2026-05-10T12:00:00.000Z"
})json";

class JsonDocument {
public:
    explicit JsonDocument(const char* payload) : root_(cJSON_Parse(payload)) {}
    ~JsonDocument() { cJSON_Delete(root_); }

    cJSON* root() const { return root_; }
    bool ok() const { return root_ != nullptr; }

private:
    cJSON* root_ = nullptr;
};

bool Require(bool condition, const char* message)
{
    if (!condition) {
        ESP_LOGE(kTag, "fixture check failed: %s", message);
    }
    return condition;
}

bool CheckSuccess(cJSON* root)
{
    const cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    return cJSON_IsBool(success) && cJSON_IsTrue(success);
}

bool CheckPollPaired()
{
    JsonDocument document(kPollPaired);
    if (!Require(document.ok(), "poll paired parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
    cJSON* token = cJSON_GetObjectItemCaseSensitive(data, "access_token");

    return Require(CheckSuccess(document.root()), "poll paired success") &&
           Require(cJSON_IsString(status) && std::strcmp(status->valuestring, "paired") == 0, "poll paired status") &&
           Require(cJSON_IsString(token) && std::strlen(token->valuestring) == 64, "poll paired token");
}

bool CheckPollNoPending()
{
    JsonDocument document(kPollNoPending);
    if (!Require(document.ok(), "poll no_pending parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");

    return Require(CheckSuccess(document.root()), "poll no_pending success") &&
           Require(cJSON_IsString(status) && std::strcmp(status->valuestring, "no_pending") == 0, "poll no_pending status");
}

bool CheckPollAlreadyPaired()
{
    JsonDocument document(kPollAlreadyPaired);
    if (!Require(document.ok(), "poll already_paired parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
    cJSON* token = cJSON_GetObjectItemCaseSensitive(data, "access_token");

    return Require(CheckSuccess(document.root()), "poll already_paired success") &&
           Require(cJSON_IsString(status) && std::strcmp(status->valuestring, "already_paired") == 0, "poll already_paired status") &&
           Require(token == nullptr || cJSON_IsNull(token), "poll already_paired must not echo access_token");
}

bool CheckSyncDue()
{
    JsonDocument document(kSyncDue);
    if (!Require(document.ok(), "sync due parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* due = cJSON_GetObjectItemCaseSensitive(data, "due_problems");
    cJSON* total = cJSON_GetObjectItemCaseSensitive(data, "total");

    return Require(CheckSuccess(document.root()), "sync due success") &&
           Require(cJSON_IsArray(due) && cJSON_GetArraySize(due) == 2, "sync due ids") &&
           Require(cJSON_IsNumber(total) && total->valueint == 2, "sync due total");
}

bool CheckProblemDetails()
{
    JsonDocument document(kProblemDetails);
    if (!Require(document.ok(), "problem details parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* problems = cJSON_GetObjectItemCaseSensitive(data, "problems");
    cJSON* first = cJSON_GetArrayItem(problems, 0);
    cJSON* content = cJSON_GetObjectItemCaseSensitive(first, "content_text");
    cJSON* assets = cJSON_GetObjectItemCaseSensitive(first, "assets");

    const std::string text = cJSON_IsString(content) ? content->valuestring : "";
    return Require(CheckSuccess(document.root()), "problem details success") &&
           Require(cJSON_IsArray(problems) && cJSON_GetArraySize(problems) == 1, "problem details count") &&
           Require(text.find("x + 2 = 5") != std::string::npos, "problem device text") &&
           Require(cJSON_IsArray(assets) && cJSON_GetArraySize(assets) == 1, "problem image asset manifest");
}

bool CheckProblemMathFallback()
{
    JsonDocument document(kProblemWithMath);
    if (!Require(document.ok(), "problem math parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* problems = cJSON_GetObjectItemCaseSensitive(data, "problems");
    cJSON* first = cJSON_GetArrayItem(problems, 0);
    cJSON* content = cJSON_GetObjectItemCaseSensitive(first, "content_text");
    cJSON* solution = cJSON_GetObjectItemCaseSensitive(first, "solution_text");

    return Require(CheckSuccess(document.root()), "problem math success") &&
           Require(cJSON_IsString(content) && std::strstr(content->valuestring, "x^2 >= 4") != nullptr,
                   "problem math content fallback") &&
           Require(cJSON_IsString(solution) && std::strstr(solution->valuestring, "x <= -2") != nullptr,
                   "problem math solution fallback");
}

bool CheckReviewComplete()
{
    JsonDocument document(kReviewComplete);
    if (!Require(document.ok(), "review complete parses")) {
        return false;
    }

    cJSON* data = cJSON_GetObjectItemCaseSensitive(document.root(), "data");
    cJSON* processed = cJSON_GetObjectItemCaseSensitive(data, "processed");

    return Require(CheckSuccess(document.root()), "review complete success") &&
           Require(cJSON_IsNumber(processed) && processed->valueint == 1, "review complete processed");
}

bool CheckTodoList()
{
    wqn::WqnTodoListPage page;
    const esp_err_t result = wqn::ParseTodoListResponse(kTodoList, &page);
    wqn::WqnTodoListPage fallback_page;
    const esp_err_t fallback_result = wqn::ParseTodoListResponse(kTodoListWithoutSelection, &fallback_page);
    return Require(result == ESP_OK, "todo list parse result") &&
           Require(page.todos.size() == 2, "todo list count") &&
           Require(page.total == 2, "todo list total") &&
           Require(page.previous_cursor == "prev-window", "todo previous cursor") &&
           Require(page.next_cursor == "next-window", "todo next cursor") &&
           Require(page.has_earlier, "todo has earlier") &&
           Require(page.has_later, "todo has later") &&
           Require(page.has_more, "todo has more") &&
           Require(page.selected_todo_id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "todo selected id") &&
           Require(page.selected_index == 1, "todo selected index") &&
           Require(page.todos[1].id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "todo id") &&
           Require(page.todos[1].title == "Review algebra", "todo title") &&
           Require(page.todos[1].status == "pending", "todo pending status") &&
           Require(page.todos[1].due_at == "2026-06-02T12:00:00.000Z", "todo due_at") &&
           Require(page.todos[1].subject_name == "Math", "todo subject") &&
           Require(fallback_result == ESP_OK, "todo fallback parse result") &&
           Require(fallback_page.selected_index == 1, "todo fallback nearest index") &&
           Require(fallback_page.todos[1].id == "cccccccc-cccc-4ccc-8ccc-cccccccccccc", "todo fallback nearest id");
}

bool CheckTodoComplete()
{
    wqn::WqnTodoItem todo;
    const esp_err_t result = wqn::ParseTodoCompleteResponse(kTodoComplete, &todo);
    return Require(result == ESP_OK, "todo complete parse result") &&
           Require(todo.id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "todo complete id") &&
           Require(todo.status == "completed", "todo complete status") &&
           Require(todo.completed_at == "2026-06-01T00:00:00.000Z", "todo completed_at");
}

bool CheckAiTodoActions()
{
    wqn::WqnAiChatResponse response;
    const esp_err_t result = wqn::ParseAiChatResponseBody(kAiTodoActions, &response);
    return Require(result == ESP_OK, "AI todo action parse result") &&
           Require(response.transcript == "Add a todo", "AI transcript") &&
           Require(response.reply_text == "Done", "AI reply_text") &&
           Require(response.actions.size() == 5, "AI action count") &&
           Require(response.actions[0].type == "todo_created", "AI create action type") &&
           Require(response.actions[0].todo_id == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "AI todo_id") &&
           Require(response.actions[0].status == "pending", "AI todo pending status") &&
           Require(response.actions[0].due_at == "2026-06-02T12:00:00.000Z", "AI due_at") &&
           Require(response.actions[1].type == "todo_status_updated", "AI update action type") &&
           Require(response.actions[1].status == "completed", "AI completed status") &&
           Require(response.actions[2].status == "pending", "AI restored status") &&
           Require(response.actions[3].status == "cancelled", "AI cancelled status") &&
           Require(response.actions[4].type == "future_action", "AI unknown action preserved");
}

bool CheckWordSync()
{
    wqn::WqnWordSyncPage page;
    const esp_err_t result = wqn::ParseWordSyncResponse(kWordSync, &page);
    return Require(result == ESP_OK, "word sync parse result") &&
           Require(page.cursor == "cursor-2", "word sync cursor") &&
           Require(page.server_time == "2026-06-06T00:00:00.000Z", "word sync server time") &&
           Require(page.decks.size() == 1, "word sync skips invalid deck") &&
           Require(page.entries.size() == 1, "word sync skips invalid entry") &&
           Require(page.progress.size() == 1, "word sync skips invalid progress") &&
           Require(page.decks[0].id == "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "word deck id") &&
           Require(page.decks[0].is_system, "word deck system flag") &&
           Require(page.entries[0].word == "confirm", "word entry word") &&
           Require(page.entries[0].meaning == "verify", "word entry meaning") &&
           Require(page.progress[0].status == "learning", "word progress status") &&
           Require(page.progress[0].lapses == 1, "word progress lapses");
}

bool CheckWordReviewQueue()
{
    wqn::WqnWordReviewQueue queue;
    const esp_err_t result = wqn::ParseWordReviewQueueResponse(kWordReviewQueue, &queue);
    return Require(result == ESP_OK, "word review queue parse result") &&
           Require(queue.mode == "sequential", "word review mode") &&
           Require(queue.daily_target == 20, "word review daily target") &&
           Require(queue.reviewed_today == 8, "word review reviewed today") &&
           Require(queue.due_count == 12, "word review due count") &&
           Require(queue.words.size() == 1, "word review skips invalid word") &&
           Require(queue.words[0].word == "consistent", "word review word") &&
           Require(queue.words[0].status == "new", "word review status");
}

bool CheckWordReviewSubmit()
{
    wqn::WqnWordReviewSubmitResult review;
    const esp_err_t result = wqn::ParseWordReviewSubmitResponse(kWordReviewSubmit, &review);
    return Require(result == ESP_OK, "word review submit parse result") &&
           Require(review.word_id == "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "word review submit word_id") &&
           Require(review.status == "learning", "word review submit status") &&
           Require(review.due_at == "2026-06-06T00:00:00.000Z", "word review submit due_at") &&
           Require(review.actions.size() == 2, "word review submit action count") &&
           Require(review.actions[0].type == "word_review_recorded", "word review submit primary action type") &&
           Require(review.actions[0].outcome == "unknown", "word review submit outcome") &&
           Require(review.actions[1].type == "word_added_to_mistakes", "word review submit extra action type") &&
           Require(review.actions[1].problem_id == "88888888-8888-4888-8888-888888888888", "word review problem id");
}

bool CheckWordSearch()
{
    wqn::WqnWordSearchResult search;
    const esp_err_t result = wqn::ParseWordSearchResponse(kWordSearch, &search);
    return Require(result == ESP_OK, "word search parse result") &&
           Require(search.prefix == "co", "word search prefix") &&
           Require(search.words.size() == 1, "word search skips invalid word") &&
           Require(search.words[0].word == "concept", "word search word") &&
           Require(search.next_letters.size() == 3, "word search next letter count") &&
           Require(search.next_letters[1] == "n", "word search next letter value");
}

bool CheckAiWordActions()
{
    wqn::WqnAiChatResponse response;
    const esp_err_t result = wqn::ParseAiChatResponseBody(kAiWordActions, &response);
    return Require(result == ESP_OK, "AI word action parse result") &&
           Require(response.transcript == "Review confirm", "AI word transcript") &&
           Require(response.reply_text == "Recorded", "AI word reply_text") &&
           Require(response.actions.size() == 5, "AI word action count") &&
           Require(response.actions[0].type == "word_review_recorded", "AI word review action type") &&
           Require(response.actions[0].word_id == "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "AI word id") &&
           Require(response.actions[0].word == "confirm", "AI word value") &&
           Require(response.actions[0].outcome == "unknown", "AI word outcome") &&
           Require(response.actions[1].type == "word_added_to_mistakes", "AI word mistakes type") &&
           Require(response.actions[1].problem_set_id == "99999999-9999-4999-8999-999999999999", "AI word problem set") &&
           Require(response.actions[2].type == "word_deck_created", "AI word deck created") &&
           Require(response.actions[2].deck_id == "dddddddd-dddd-4ddd-8ddd-dddddddddddd", "AI word deck id") &&
           Require(response.actions[3].type == "word_added_to_deck", "AI word added to deck") &&
           Require(response.actions[4].type == "future_word_action", "AI unknown word action preserved");
}

bool CheckUnauthorizedError()
{
    JsonDocument document(kUnauthorizedError);
    if (!Require(document.ok(), "error parses")) {
        return false;
    }

    cJSON* error = cJSON_GetObjectItemCaseSensitive(document.root(), "error");
    cJSON* status = cJSON_GetObjectItemCaseSensitive(document.root(), "status");

    return Require(cJSON_IsString(error), "error message") &&
           Require(cJSON_IsNumber(status) && status->valueint == 401, "error status");
}

bool CheckV3ControlContract()
{
    wqn::protocol::v3::RequestMetadata metadata;
    metadata.request_id = "req_claim_start_0001";
    metadata.boot_id = "boot_contract_0001";
    metadata.firmware_version = "0.1.0-contract";
    metadata.config_revision = 99;
    metadata.sync_cursor = 88;
    std::string request_body;
    if (!Require(
            wqn::protocol::v3::BuildClaimStartRequest(
                metadata,
                "AABBCCDDEEFF",
                std::string(86, 'A'),
                &request_body) == ESP_OK,
            "v3 claim start request build")) {
        return false;
    }
    JsonDocument claim_request(request_body.c_str());
    if (!Require(claim_request.ok(), "v3 claim start request parses") ||
        !Require(
            cJSON_GetObjectItemCaseSensitive(
                claim_request.root(), "config_revision") == nullptr,
            "v3 claim request omits config revision") ||
        !Require(
            cJSON_GetObjectItemCaseSensitive(
                claim_request.root(), "sync_cursor") == nullptr,
            "v3 claim request omits sync cursor")) {
        return false;
    }

    wqn::protocol::v3::ClaimStartData claim_start;
    wqn::protocol::v3::Error error;
    if (!Require(
            wqn::protocol::v3::ParseClaimStartResponse(
                kV3ClaimStart,
                "req_claim_start_0001",
                &claim_start,
                &error) == ESP_OK,
            "v3 claim start parse") ||
        !Require(claim_start.display_code == "31415926", "v3 claim code") ||
        !Require(claim_start.poll_interval_ms == 3000, "v3 claim poll interval")) {
        return false;
    }

    wqn::protocol::v3::ClaimPollData claim_poll;
    if (!Require(
            wqn::protocol::v3::ParseClaimPollResponse(
                kV3ClaimPending,
                "req_claim_poll_0000",
                &claim_poll,
                &error) == ESP_OK,
            "v3 claim pending parse") ||
        !Require(
            claim_poll.status == wqn::protocol::v3::ClaimStatus::kPending,
            "v3 claim pending status") ||
        !Require(claim_poll.poll_interval_ms == 3000, "v3 pending interval")) {
        return false;
    }
    if (!Require(
            wqn::protocol::v3::ParseClaimPollResponse(
                kV3ClaimApproved,
                "req_claim_poll_0001",
                &claim_poll,
                &error) == ESP_OK,
            "v3 claim approved parse") ||
        !Require(
            claim_poll.status == wqn::protocol::v3::ClaimStatus::kApproved,
            "v3 claim approved status") ||
        !Require(
            !claim_poll.sealed_credential.ciphertext.empty(),
            "v3 claim sealed credential")) {
        return false;
    }

    wqn::protocol::v3::BootstrapData bootstrap;
    if (!Require(
            wqn::protocol::v3::ParseBootstrapResponse(
                kV3Bootstrap, "req_bootstrap_0001", &bootstrap, &error) == ESP_OK,
            "v3 bootstrap parse") ||
        !Require(bootstrap.config_revision == 7, "v3 bootstrap revision") ||
        !Require(bootstrap.sync_cursor == 42, "v3 bootstrap cursor")) {
        return false;
    }

    wqn::protocol::v3::BootstrapData unsafe_counter;
    if (!Require(
            wqn::protocol::v3::ParseBootstrapResponse(
                kV3UnsafeCounter,
                "req_bootstrap_0002",
                &unsafe_counter,
                &error) == ESP_ERR_INVALID_RESPONSE,
            "v3 rejects counters above JSON safe integer")) {
        return false;
    }
    metadata.config_revision = wqn::protocol::v3::kMaxSafeJsonInteger + 1;
    if (!Require(
            wqn::protocol::v3::BuildBootstrapRequest(metadata, &request_body) ==
                ESP_ERR_INVALID_ARG,
            "v3 refuses unsafe request counters")) {
        return false;
    }

    wqn::protocol::v3::SyncData sync;
    if (!Require(
            wqn::protocol::v3::ParseSyncResponse(
                kV3Sync, "req_sync_000000001", &sync, &error) == ESP_OK,
            "v3 sync parse") ||
        !Require(sync.due_problem_ids.size() == 1, "v3 sync due count") ||
        !Require(sync.todo_count == 2, "v3 sync todo count") ||
        !Require(sync.word_due_count == 5, "v3 sync word count")) {
        return false;
    }

    const esp_err_t error_result = wqn::protocol::v3::ParseSyncResponse(
        kV3Error, "req_sync_000000002", &sync, &error);
    return Require(error_result != ESP_OK, "v3 error returns failure") &&
           Require(error.code == "TEMPORARILY_UNAVAILABLE", "v3 error code") &&
           Require(error.retryable, "v3 error retryable") &&
           Require(error.retry_after_ms == 10000, "v3 retry delay");
}

bool CheckWordStudyV1Contract()
{
    namespace words = wqn::protocol::word_study_v1;
    wqn::protocol::v3::Error error;
    words::SessionData session;
    if (!Require(
            words::ParseSessionResponse(
                kWordSessionV1, "req_word_session_0001", &session, &error) == ESP_OK,
            "word-study session fixture") ||
        !Require(session.mode == words::Mode::kRandom, "word-study visible mode") ||
        !Require(
            session.ordering == words::Ordering::kGuidedRandomV1,
            "word-study random ordering") ||
        !Require(
            session.candidate_policy_version == "guided_random_v1",
            "word-study candidate policy") ||
        !Require(session.progress_revision == 17, "word-study progress revision") ||
        !Require(session.snapshot.size() == 1, "word-study snapshot count") ||
        !Require(session.items.size() == 1, "word-study item count")) {
        return false;
    }

    words::CandidatePageData candidate_page;
    if (!Require(
            words::ParseCandidatePageResponse(
                kWordCandidatePageV1,
                "req_word_candidates_0001",
                &candidate_page,
                &error) == ESP_OK,
            "word-study candidate page fixture") ||
        !Require(candidate_page.cursor == "32", "word-study candidate cursor") ||
        !Require(candidate_page.next_cursor == "33", "word-study candidate next cursor") ||
        !Require(candidate_page.items.size() == 1, "word-study candidate count") ||
        !Require(candidate_page.items[0].ordinal == 32, "word-study candidate ordinal") ||
        !Require(candidate_page.has_more, "word-study candidate has more")) {
        return false;
    }

    words::ObservationData observation;
    if (!Require(
            words::ParseObservationResponse(
                kWordObservationV1,
                "req_word_observe_0001",
                &observation,
                &error) == ESP_OK,
            "word-study observation fixture") ||
        !Require(
            observation.action == words::ObservationAction::kUnknown,
            "word-study observation action") ||
        !Require(observation.progress.present, "word-study progress projection") ||
        !Require(observation.progress.unknown_count == 1, "word-study unknown count")) {
        return false;
    }

    words::ManifestData manifest;
    if (!Require(
            words::ParseManifestResponse(
                kWordManifestV1,
                "req_word_manifest_001",
                &manifest,
                &error) == ESP_OK,
            "word-study manifest fixture") ||
        !Require(manifest.cursor == 17, "word-study exact manifest cursor") ||
        !Require(manifest.decks.size() == 1, "word-study manifest deck count") ||
        !Require(manifest.decks[0].has_pack, "word-study live pack") ||
        !Require(
            manifest.decks[0].pack.schema_version == 2,
            "word-study pack schema")) {
        return false;
    }
    if (!Require(
            words::ParseManifestResponse(
                kWordManifestUnsafeCursorV1,
                "req_word_manifest_002",
                &manifest,
                &error) == ESP_ERR_INVALID_RESPONSE,
            "word-study rejects unsafe decimal cursor")) {
        return false;
    }

    wqn::protocol::v3::RequestMetadata metadata;
    metadata.request_id = "req_word_manifest_001";
    metadata.boot_id = "boot_word_study_001";
    metadata.firmware_version = "0.1.0-contract";
    std::string body;
    if (!Require(
            words::BuildManifestRequest(metadata, 17, 20, &body) == ESP_OK,
            "word-study manifest request build") ||
        !Require(
            body.find("\"cursor\":\"17\"") != std::string::npos,
            "word-study cursor encoded exactly") ||
        !Require(
            body.find("word.study.v1") != std::string::npos,
            "word-study capability advertised")) {
        return false;
    }

    words::CandidatePageRequest candidate_request;
    candidate_request.metadata = metadata;
    candidate_request.metadata.request_id = "req_word_candidates_0001";
    candidate_request.cursor = "32";
    candidate_request.limit = 64;
    if (!Require(
            words::BuildCandidatePageRequest(candidate_request, &body) == ESP_OK,
            "word-study candidate request build") ||
        !Require(
            body.find("\"cursor\":\"32\"") != std::string::npos,
            "word-study candidate cursor encoded exactly") ||
        !Require(
            body.find("\"limit\":64") != std::string::npos,
            "word-study candidate limit")) {
        return false;
    }

    const char* ids[] = {
        "00000000-0000-4000-8000-000000000001",
        "00000000-0000-4000-8000-000000000002",
        "00000000-0000-4000-8000-000000000003",
        "00000000-0000-4000-8000-000000000004",
        "00000000-0000-4000-8000-000000000005",
        "00000000-0000-4000-8000-000000000006",
        "00000000-0000-4000-8000-000000000007",
    };
    const uint64_t hashes[] = {
        UINT64_C(0x11423e284650d546), UINT64_C(0x11423d284650d393),
        UINT64_C(0x11423c284650d1e0), UINT64_C(0x114243284650ddc5),
        UINT64_C(0x114242284650dc12), UINT64_C(0x114241284650da5f),
        UINT64_C(0x114240284650d8ac),
    };
    for (size_t index = 0; index < 7; ++index) {
        if (!Require(
                words::GuidedRandomHash("seed_contract_001", ids[index]) ==
                    hashes[index],
                "word-study FNV fixture")) {
            return false;
        }
    }

    const int32_t sort_indices[] = {1, 2, 3, 0, 0, 1, 2};
    const uint32_t deck_orders[] = {0, 0, 0, 0, 1, 1, 1};
    const words::CandidateStatus statuses[] = {
        words::CandidateStatus::kLearning,
        words::CandidateStatus::kReview,
        words::CandidateStatus::kNew,
        words::CandidateStatus::kReview,
        words::CandidateStatus::kMastered,
        words::CandidateStatus::kLearning,
        words::CandidateStatus::kNew,
    };
    const int64_t due[] = {0, 1000, -1, 2000, 3000, -1, -1};
    const char* normalized[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta"};
    std::vector<words::Candidate> candidates;
    for (size_t index = 0; index < 7; ++index) {
        words::Candidate candidate;
        candidate.item_id = ids[index];
        candidate.normalized_word = normalized[index];
        candidate.deck_order = deck_orders[index];
        candidate.sort_index = sort_indices[index];
        candidate.status = statuses[index];
        candidate.due_at_ms = due[index];
        candidates.push_back(candidate);
    }
    words::OrderCandidates(
        &candidates, words::Ordering::kGuidedRandomV1, "seed_contract_001", 1000);
    const size_t expected[] = {0, 5, 1, 2, 6, 3, 4};
    for (size_t index = 0; index < 7; ++index) {
        if (!Require(
                candidates[index].item_id == ids[expected[index]],
                "word-study guided random fixture")) {
            return false;
        }
    }
    return true;
}

bool CheckAiStreamHttpFailures()
{
    return Require(
               std::strcmp(wqn::internal::AiStreamHttpErrorCode(401), "unauthorized") == 0,
               "SSE 401 classification") &&
           Require(
               std::strcmp(wqn::internal::AiStreamHttpErrorCode(429), "rate_limited") == 0,
               "SSE 429 classification") &&
           Require(
               std::strcmp(wqn::internal::AiStreamHttpErrorCode(500), "model_failed") == 0,
               "SSE 500 classification") &&
           Require(
               wqn::internal::FinalizeAiStreamResult(true, ESP_OK) == ESP_FAIL,
               "fatal SSE HTTP status cannot complete successfully") &&
           Require(
               wqn::internal::FinalizeAiStreamResult(false, ESP_OK) == ESP_OK,
               "successful SSE stream remains successful");
}

}  // namespace

namespace wqn {

bool RunContractFixtureSelfTest()
{
    const bool ok =
        CheckPollPaired() &&
        CheckPollNoPending() &&
        CheckPollAlreadyPaired() &&
        CheckSyncDue() &&
        CheckProblemDetails() &&
        CheckProblemMathFallback() &&
        CheckReviewComplete() &&
        CheckTodoList() &&
        CheckTodoComplete() &&
        CheckAiTodoActions() &&
        CheckWordSync() &&
        CheckWordReviewQueue() &&
        CheckWordReviewSubmit() &&
        CheckWordSearch() &&
        CheckAiWordActions() &&
        CheckUnauthorizedError() &&
        CheckV3ControlContract() &&
        CheckWordStudyV1Contract() &&
        CheckAiStreamHttpFailures() &&
        wqn::ui::RunConfirmGestureArbiterSelfTest() &&
        device_ui_internal::RunStatusControlShellSelfTest();

    if (ok) {
        ESP_LOGI(kTag, "contract fixture self-test passed");
    }
    return ok;
}

}  // namespace wqn
