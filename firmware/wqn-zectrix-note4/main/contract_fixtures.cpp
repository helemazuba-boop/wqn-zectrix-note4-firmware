#include "contract_fixtures.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "device_protocol/v3.h"
#include "device_protocol/problem_study.h"
#include "device_protocol/word_study.h"
#include "esp_log.h"
#include "note_app.h"
#include "problem_app.h"
#include "problem_pack.h"
#include "text_render.h"
#include "ui/markdown_layout.h"
#include "word_app.h"
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
    "projection_applied": true,
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
        "compression": "zlib",
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

const char kProblemManifestV1[] = R"json({
  "ok": true,
  "request_id": "req_problem_manifest_01",
  "server_time_ms": 1784512800000,
  "data": {
    "cursor": "2",
    "has_more": false,
    "problem_sets": [
      {
        "problem_set_id": "11111111-1111-4111-8111-111111111111",
        "name": "圆锥曲线专项",
        "is_smart": false,
        "deleted": false,
        "pack": {
          "pack_id": "11111111-1111-4111-8111-111111111111",
          "pack_revision": 1784512700,
          "schema_version": 1,
          "format": "jsonl",
          "compression": "zlib",
          "entry_count": 12,
          "byte_size": 20480,
          "sha256": "9e00e194c412bff778bfd1235b3b2b25a4f7f8b1d3ef1c72fca11d21b36d1e05",
          "download_url": "https://example.com/api/esp32/v3/problems/packs/11111111-1111-4111-8111-111111111111"
        }
      },
      {
        "problem_set_id": "22222222-2222-4222-8222-222222222222",
        "name": "全部错题",
        "is_smart": true,
        "deleted": false,
        "pack": {
          "pack_id": "22222222-2222-4222-8222-222222222222",
          "pack_revision": 1784512750,
          "schema_version": 1,
          "format": "jsonl",
          "compression": "zlib",
          "entry_count": 48,
          "byte_size": 96031,
          "sha256": "1b1f4d9c22cf8d0b6cf6a52ad4a3f2e8809d15b9a7f96ff2f4bf1cf3a2b4c6d8",
          "download_url": "https://example.com/api/esp32/v3/problems/packs/22222222-2222-4222-8222-222222222222"
        }
      }
    ]
  }
})json";

const char kProblemObservationV1[] = R"json({
  "ok": true,
  "request_id": "req_problem_observe_01",
  "server_time_ms": 1784512801000,
  "data": {
    "observation_id": "44444444-4444-4444-8444-444444444444",
    "problem_id": "33333333-3333-4333-8333-333333333333",
    "action": "correct",
    "status": "mastered",
    "schedule": {
      "next_review_at": "2026-07-31T16:00:00+00:00",
      "interval_days": 3,
      "ease_factor": 2.6,
      "repetition_number": 2
    },
    "projection_applied": true,
    "replayed": false
  }
})json";

const char kProblemObservationSkipV1[] = R"json({
  "ok": true,
  "request_id": "req_problem_observe_02",
  "server_time_ms": 1784512802000,
  "data": {
    "observation_id": "55555555-5555-4555-8555-555555555555",
    "problem_id": "33333333-3333-4333-8333-333333333333",
    "action": "skip",
    "status": "needs_review",
    "schedule": null,
    "projection_applied": false,
    "replayed": false
  }
})json";

const char kProblemObservationBadActionV1[] = R"json({
  "ok": true,
  "request_id": "req_problem_observe_03",
  "server_time_ms": 1784512803000,
  "data": {
    "observation_id": "55555555-5555-4555-8555-555555555555",
    "problem_id": "33333333-3333-4333-8333-333333333333",
    "action": "partially_correct",
    "status": "needs_review",
    "schedule": null,
    "projection_applied": false,
    "replayed": false
  }
})json";

// One pack JSONL record (contracts/problem-study-v1 fixtures/valid/pack-row).
const char kProblemPackRowV1[] = R"json({
  "problem_id": "33333333-3333-4333-8333-333333333333",
  "title": "生物遗传综合题",
  "content_text": "某二倍体植物的花色由两对等位基因控制，请回答下列问题。",
  "parts": [
    {
      "index": 1,
      "label": "选择",
      "type": "single_choice",
      "full_marks": 6,
      "content_text": "该植物花色遗传遵循的规律是？",
      "answer_text": "B"
    },
    {
      "index": 2,
      "label": "填空",
      "type": "fill_blank",
      "full_marks": 4,
      "content_text": "F2 中白花植株的基因型共有____种。",
      "answer_text": "5"
    },
    {
      "index": 3,
      "label": "简答",
      "type": "essay",
      "full_marks": 10,
      "content_text": "请用遗传图解说明 F1 自交得到 F2 的过程。",
      "answer_text": ""
    }
  ],
  "source": { "type": "textbook", "label": "人教版必修二" },
  "status": "wrong",
  "is_optional": false,
  "image_ids": [
    "9e00e194c412bff778bfd1235b3b2b25a4f7f8b1d3ef1c72fca11d21b36d1e05"
  ],
  "solution_image_ids": [
    "1b1f4d9c22cf8d0b6cf6a52ad4a3f2e8809d15b9a7f96ff2f4bf1cf3a2b4c6d8"
  ]
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
        !Require(observation.projection_applied, "word-study projection applied") ||
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

    // Cloud and firmware must compare the UTF-8 bytes, not JavaScript UTF-16
    // code units. U+E000 (EE 80 80) therefore sorts before U+10000 (F0 90 80
    // 80), even though their UTF-16 ordering would be reversed.
    std::vector<words::Candidate> unicode_candidates(2);
    unicode_candidates[0].item_id = ids[1];
    unicode_candidates[0].normalized_word = "\xEE\x80\x80";
    unicode_candidates[1].item_id = ids[2];
    unicode_candidates[1].normalized_word = "\xF0\x90\x80\x80";
    words::OrderCandidates(
        &unicode_candidates,
        words::Ordering::kLexicographic,
        "seed_contract_001",
        1000);
    if (!Require(
            unicode_candidates[0].item_id == ids[1] &&
                unicode_candidates[1].item_id == ids[2],
            "word-study UTF-8 lexicographic fixture")) {
        return false;
    }
    return true;
}

bool CheckProblemStudyV1Contract()
{
    namespace problems = wqn::protocol::problem_study_v1;
    wqn::protocol::v3::Error error;
    problems::ManifestData manifest;
    if (!Require(
            problems::ParseManifestResponse(
                kProblemManifestV1,
                "req_problem_manifest_01",
                &manifest,
                &error) == ESP_OK,
            "problem-study manifest fixture") ||
        !Require(manifest.cursor == 2, "problem-study manifest cursor") ||
        !Require(!manifest.has_more, "problem-study manifest has_more") ||
        !Require(manifest.problem_sets.size() == 2, "problem-study manifest set count") ||
        !Require(!manifest.problem_sets[0].is_smart, "problem-study manual set") ||
        !Require(manifest.problem_sets[1].is_smart, "problem-study smart set") ||
        !Require(manifest.problem_sets[0].has_pack, "problem-study live pack") ||
        !Require(
            manifest.problem_sets[1].pack.entry_count == 48,
            "problem-study pack entry count") ||
        !Require(
            manifest.problem_sets[0].pack.schema_version == 1,
            "problem-study pack schema")) {
        return false;
    }

    problems::ObservationData observation;
    if (!Require(
            problems::ParseObservationResponse(
                kProblemObservationV1,
                "req_problem_observe_01",
                &observation,
                &error) == ESP_OK,
            "problem-study observation fixture") ||
        !Require(
            observation.action == problems::ReviewAction::kCorrect,
            "problem-study observation action") ||
        !Require(
            observation.status == problems::ProblemStatus::kMastered,
            "problem-study observation status") ||
        !Require(observation.schedule.present, "problem-study schedule projection") ||
        !Require(observation.schedule.interval_days == 3, "problem-study interval") ||
        !Require(observation.schedule.repetition_number == 2, "problem-study repetition") ||
        !Require(observation.projection_applied, "problem-study projection applied") ||
        !Require(!observation.replayed, "problem-study not replayed")) {
        return false;
    }
    if (!Require(
            problems::ParseObservationResponse(
                kProblemObservationSkipV1,
                "req_problem_observe_02",
                &observation,
                &error) == ESP_OK,
            "problem-study skip fixture") ||
        !Require(
            observation.action == problems::ReviewAction::kSkip,
            "problem-study skip action") ||
        !Require(!observation.schedule.present, "problem-study skip null schedule") ||
        !Require(!observation.projection_applied, "problem-study skip no projection")) {
        return false;
    }
    if (!Require(
            problems::ParseObservationResponse(
                kProblemObservationBadActionV1,
                "req_problem_observe_03",
                &observation,
                &error) == ESP_ERR_INVALID_RESPONSE,
            "problem-study rejects unknown action")) {
        return false;
    }

    wqn::protocol::v3::RequestMetadata metadata;
    metadata.request_id = "req_problem_manifest_01";
    metadata.boot_id = "boot_problem_study_01";
    metadata.firmware_version = "0.1.0-contract";
    std::string body;
    if (!Require(
            problems::BuildManifestRequest(metadata, 0, 50, &body) == ESP_OK,
            "problem-study manifest request build") ||
        !Require(
            body.find("\"cursor\":\"0\"") != std::string::npos,
            "problem-study cursor encoded exactly") ||
        !Require(
            body.find("problem.study.v1") != std::string::npos,
            "problem-study capability advertised")) {
        return false;
    }

    problems::ObservationRequest observe_request;
    observe_request.metadata = metadata;
    observe_request.metadata.request_id = "req_problem_observe_01";
    observe_request.problem_id = "33333333-3333-4333-8333-333333333333";
    observe_request.action = problems::ReviewAction::kHesitant;
    observe_request.occurred_at = "2026-07-28T03:20:00.000Z";
    if (!Require(
            problems::BuildObservationRequest(observe_request, &body) == ESP_OK,
            "problem-study observation request build") ||
        !Require(
            body.find("\"action\":\"hesitant\"") != std::string::npos,
            "problem-study action encoded") ||
        !Require(
            body.find("\"problem_id\":\"33333333-3333-4333-8333-333333333333\"") !=
                std::string::npos,
            "problem-study problem id encoded")) {
        return false;
    }
    observe_request.problem_id = "not-a-uuid";
    if (!Require(
            problems::BuildObservationRequest(observe_request, &body) ==
                ESP_ERR_INVALID_ARG,
            "problem-study refuses invalid problem id")) {
        return false;
    }

    // Pack row parsing: the same JSONL record the cloud freezes in
    // contracts/problem-study-v1 fixtures, both scan and body modes.
    wqn::WqnProblemEntry entry;
    if (!Require(
            wqn::ParseProblemRecordLine(kProblemPackRowV1, &entry, true) == ESP_OK,
            "problem pack row parses") ||
        !Require(entry.problem_id == "33333333-3333-4333-8333-333333333333", "pack row id") ||
        !Require(entry.title == "生物遗传综合题", "pack row title") ||
        !Require(
            entry.status == problems::ProblemStatus::kWrong,
            "pack row status") ||
        !Require(!entry.is_optional, "pack row optional flag") ||
        !Require(entry.parts.size() == 3, "pack row part count") ||
        !Require(entry.parts[0].index == 1, "pack row part index") ||
        !Require(entry.parts[0].type == "single_choice", "pack row part type") ||
        !Require(entry.parts[0].full_marks == 6, "pack row part marks") ||
        !Require(entry.parts[0].answer_text == "B", "pack row flattened answer") ||
        !Require(entry.parts[2].answer_text.empty(), "pack row essay empty answer") ||
        !Require(entry.image_ids.size() == 1, "pack row image ids") ||
        !Require(entry.solution_image_ids.size() == 1, "pack row solution image ids") ||
        !Require(
            entry.image_ids[0] ==
                "9e00e194c412bff778bfd1235b3b2b25a4f7f8b1d3ef1c72fca11d21b36d1e05",
            "pack row image id value")) {
        return false;
    }
    if (!Require(
            wqn::ParseProblemRecordLine(kProblemPackRowV1, &entry, false) == ESP_OK,
            "problem pack row scan parses") ||
        !Require(entry.parts.empty(), "pack row scan skips part bodies") ||
        !Require(entry.content_text.empty(), "pack row scan skips content") ||
        !Require(entry.image_ids.size() == 1, "pack row scan keeps image counts")) {
        return false;
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

// Exercises the note-body Markdown layout engine (ui/markdown_layout) over a
// showcase covering every stage-1 element, asserting each adornment class is
// produced and that the render/scroll-count paths agree. Runs on the UI-free
// boot self-test so a layout regression fails fast instead of on-panel.
bool CheckMarkdownLayout()
{
    using namespace device_ui_internal;
    static const char kShowcase[] =
        "# Heading 1\n"
        "## Heading 2\n"
        "### Heading 3\n"
        "Normal **bold** and *italic* and ~~strike~~ text.\n"
        "Inline `code` and a [link](https://example.com).\n"
        "An image ![diagram](http://img/x.png) inline.\n"
        "- item one\n"
        "- item two\n"
        "  - nested item\n"
        "1. first\n"
        "2. second\n"
        "> quoted line\n"
        ">> nested quote\n"
        "---\n"
        "```\ncode block\nsecond code line\n```\n"
        "| A | B | C |\n|---|---|---|\n| 1 | 2 | 3 |\n";

    const std::vector<MdLine> lines = LayoutMarkdown(kShowcase, 370);
    if (!Require(!lines.empty(), "markdown layout produced rows") ||
        !Require(
            CountMarkdownLines(kShowcase, 370) == lines.size(),
            "markdown count matches layout size")) {
        return false;
    }

    bool has_rule = false, has_heading_underline = false, has_bullet = false;
    bool has_code = false, has_quote = false, has_table = false, has_table_border = false;
    bool has_underline = false, has_codebox = false, has_strike = false;
    for (const MdLine& line : lines) {
        has_rule |= line.kind == MdLineKind::kRule;
        has_table |= line.kind == MdLineKind::kTableRow;
        has_table_border |= line.kind == MdLineKind::kTableRow && line.border_top;
        has_heading_underline |= line.rule_below;
        has_bullet |= line.bullet != MdBullet::kNone;
        has_code |= line.code;
        has_quote |= line.quote_depth > 0;
        for (const MdDecoration& deco : line.decorations) {
            has_underline |= deco.kind == MdDecoKind::kUnderline;
            has_codebox |= deco.kind == MdDecoKind::kCodeBox;
            has_strike |= deco.kind == MdDecoKind::kStrike;
        }
    }

    // A table wider than the native cap must degrade to plain text rows, never
    // kTableRow (the renderer only draws <=4 column grids).
    static const char kWideTable[] =
        "| a | b | c | d | e |\n|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 |\n";
    bool wide_downgraded = true;
    for (const MdLine& line : LayoutMarkdown(kWideTable, 370)) {
        if (line.kind == MdLineKind::kTableRow) wide_downgraded = false;
    }

    // A 4-column table of long cells still renders as a real grid (cells wrap,
    // columns scale down) -- it must NOT be demoted to source-like text rows.
    static const char kDenseTable[] =
        "| 语法类别 | Markdown 源码示例 | 实际渲染效果说明 | 边界极端情况测试 |\n"
        "| --- | --- | --- | --- |\n"
        "| 基础文本样式演示 | 粗体斜体删除线组合 | 显示的渲染结果 | 各种极端组合测试 |\n";
    bool dense_is_grid = false;
    for (const MdLine& line : LayoutMarkdown(kDenseTable, 370)) {
        if (line.kind == MdLineKind::kTableRow) dense_is_grid = true;
    }

    // Triple emphasis must strip completely; the old pairwise probe leaked a
    // literal '*' on each side ("*粗斜体*").
    const std::vector<MdLine> triple = LayoutMarkdown("***粗斜体***", 370);
    const bool triple_clean = triple.size() == 1 && triple[0].text == "粗斜体";

    // kMdNoSingleEmphasis (problem-face mode): math plain text keeps single
    // * / _ literal while double-marker bold still strips.
    const std::vector<MdLine> math =
        LayoutMarkdown("x*y*z 与 x_1 加 **粗体**", 370, kMdNoSingleEmphasis);
    const bool math_clean =
        math.size() == 1 && math[0].text == "x*y*z 与 x_1 加 粗体";

    // AI assistant width (378 px): the showcase lays out and the row-count
    // path agrees, mirroring the measure/draw split in page_ai.cpp.
    const std::vector<MdLine> ai_rows = LayoutMarkdown(kShowcase, 378);
    const bool ai_width_ok =
        !ai_rows.empty() && CountMarkdownLines(kShowcase, 378) == ai_rows.size();

    return Require(has_rule, "markdown horizontal rule") &&
           Require(has_heading_underline, "markdown heading underline") &&
           Require(has_bullet, "markdown list bullet") &&
           Require(has_code, "markdown code block") &&
           Require(has_quote, "markdown blockquote bar") &&
           Require(has_table && has_table_border, "markdown table with border") &&
           Require(has_underline, "markdown link underline") &&
           Require(has_codebox, "markdown inline code box") &&
           Require(has_strike, "markdown strikethrough") &&
           Require(wide_downgraded, "markdown wide table downgraded to text") &&
           Require(dense_is_grid, "markdown dense 4-col table renders as grid") &&
           Require(triple_clean, "markdown triple emphasis fully stripped") &&
           Require(math_clean, "markdown math mode keeps single emphasis literal") &&
           Require(ai_width_ok, "markdown AI width layout and count agree");
}

}  // namespace

namespace wqn {

bool RunContractFixtureSelfTest()
{
    const bool ok =
        CheckPollPaired() &&
        CheckPollNoPending() &&
        CheckPollAlreadyPaired() &&
        CheckTodoList() &&
        CheckTodoComplete() &&
        CheckAiTodoActions() &&
        CheckWordSearch() &&
        CheckAiWordActions() &&
        CheckUnauthorizedError() &&
        CheckV3ControlContract() &&
        CheckWordStudyV1Contract() &&
        CheckProblemStudyV1Contract() &&
        CheckAiStreamHttpFailures() &&
        CheckMarkdownLayout() &&
        RunWordPageStateSelfTest() &&
        RunNotePageStateSelfTest() &&
        RunProblemPageStateSelfTest();

    if (ok) {
        ESP_LOGI(kTag, "contract fixture self-test passed");
    }
    return ok;
}

}  // namespace wqn
