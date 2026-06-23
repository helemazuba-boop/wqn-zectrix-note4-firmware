// Word review cloud task: pack sync, review submit, online search, AI lookup.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "word_pack.h"
#include "wqn_api.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

QueueHandle_t g_word_request_queue = nullptr;
QueueHandle_t g_word_result_queue = nullptr;
TaskHandle_t g_word_task = nullptr;
volatile bool g_word_cloud_busy = false;

bool IsWordCloudBusy()
{
    return g_word_cloud_busy;
}

bool QueueWordCloudRequest(const WordCloudRequest& request)
{
    if (g_word_request_queue == nullptr || IsWordCloudBusy()) {
        return false;
    }
    if (xQueueSend(g_word_request_queue, &request, 0) != pdTRUE) {
        return false;
    }
    g_word_cloud_busy = true;
    return true;
}

bool QueueWordReviewRefresh()
{
    WordCloudRequest request;
    request.op = WordCloudOp::kPackSync;
    return QueueWordCloudRequest(request);
}

bool QueueWordReviewSubmit(const wqn::WqnWordReviewSubmission& submission, const std::string& word)
{
    if (submission.word_id.empty() || submission.outcome.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kSubmit;
    std::snprintf(request.word_id, sizeof(request.word_id), "%s", submission.word_id.c_str());
    std::snprintf(request.outcome, sizeof(request.outcome), "%s", submission.outcome.c_str());
    std::snprintf(request.word, sizeof(request.word), "%s", word.c_str());
    return QueueWordCloudRequest(request);
}

bool QueueWordSearch(const wqn::WqnWordSearchRequest& search)
{
    if (search.query.empty() && search.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kSearch;
    const std::string query = !search.query.empty() ? search.query : search.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

bool QueueWordAiLookup(const wqn::WqnWordAiLookupRequest& lookup)
{
    if (lookup.query.empty() && lookup.prefix.empty()) {
        return false;
    }
    WordCloudRequest request;
    request.op = WordCloudOp::kAiLookup;
    const std::string query = !lookup.query.empty() ? lookup.query : lookup.prefix;
    std::snprintf(request.query, sizeof(request.query), "%s", query.c_str());
    return QueueWordCloudRequest(request);
}

void SendWordCloudResult(WordCloudResult* result)
{
    if (result == nullptr) {
        return;
    }
    if (g_word_result_queue == nullptr || xQueueSend(g_word_result_queue, &result, pdMS_TO_TICKS(100)) != pdTRUE) {
        g_word_cloud_busy = false;
        delete result;
    }
}

bool ApplyWordCloudResult(wqn::UiState* state, const WordCloudResult& result)
{
    if (state == nullptr) {
        return false;
    }
    if (result.op == WordCloudOp::kPackSync) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordPackIndex(&state->word_app, result.pack_index, result.message);
        } else {
            state->word_app.cloud_sync_failed = true;
            state->word_app.cloud_loaded_once = true;
            state->word_app.cloud_sync_requested = false;
            state->word_app.message = result.auth_required ? "请重新配对" : "单词同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }

    if (result.op == WordCloudOp::kSubmit) {
        if (result.result == ESP_OK) {
            if (std::strcmp(result.outcome, "unknown") == 0) {
                state->word_app.message = "已加入遗忘的单词";
            } else if (std::strcmp(result.outcome, "known") == 0) {
                state->word_app.message = "已记录";
            } else {
                state->word_app.message = "已同步";
            }
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "单词同步失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kSearch) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordSearchResult(&state->word_app, result.search);
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "在线搜索失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    if (result.op == WordCloudOp::kAiLookup) {
        if (result.result == ESP_OK) {
            wqn::ApplyWordAiLookupResult(&state->word_app, result.lookup);
        } else {
            state->word_app.message = result.auth_required ? "请重新配对" : "AI 查词失败";
        }
        BuildHomeSummary(state);
        return true;
    }
    return false;
}

void WordCloudTask(void*)
{
    ESP_LOGI(kTag, "Word cloud task started");
    while (true) {
        WordCloudRequest request;
        if (xQueueReceive(g_word_request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        WordCloudResult* result = new (std::nothrow) WordCloudResult();
        if (result == nullptr) {
            g_word_cloud_busy = false;
            ESP_LOGW(kTag, "alloc Word cloud result failed");
            continue;
        }
        result->op = request.op;
        std::snprintf(result->word_id, sizeof(result->word_id), "%s", request.word_id);
        std::snprintf(result->outcome, sizeof(result->outcome), "%s", request.outcome);
        std::snprintf(result->word, sizeof(result->word), "%s", request.word);
        result->message.clear();

        std::string token;
        if (!LoadValidTokenForTodo(&token)) {
            result->auth_required = true;
            result->result = ESP_ERR_INVALID_STATE;
            SendWordCloudResult(result);
            continue;
        }

        if (request.op == WordCloudOp::kPackSync) {
            wqn::WqnWordPackManifest manifest;
            result->result = wqn::FetchWordPackManifest(token, &manifest);
            if (result->result == ESP_OK) {
                size_t total_needed = 0;
                for (const auto& item : manifest.packs) {
                    if (wqn::WordPackNeedsDownload(item) && item.byte_size > 0) {
                        total_needed += item.byte_size;
                    }
                }
                if (total_needed > 0) {
                    size_t total_bytes = 0, used_bytes = 0;
                    if (esp_spiffs_info("storage", &total_bytes, &used_bytes) == ESP_OK) {
                        const size_t available = total_bytes > used_bytes ? total_bytes - used_bytes : 0;
                        if (available < total_needed) {
                            ESP_LOGW(kTag, "SPIFFS space insufficient: need=%u avail=%u",
                                     static_cast<unsigned>(total_needed), static_cast<unsigned>(available));
                            result->result = ESP_ERR_NO_MEM;
                            result->message = "存储空间不足";
                        }
                    }
                }

                std::vector<std::string> downloaded_stems;
                for (const wqn::WqnWordPackManifestItem& item : manifest.packs) {
                    if (!wqn::WordPackNeedsDownload(item)) {
                        continue;
                    }
                    std::string pack_body;
                    result->result = wqn::DownloadWordPack(token, item, &pack_body);
                    if (result->result != ESP_OK) {
                        break;
                    }
                    result->result = wqn::SaveWordPackFromBytes(item, pack_body);
                    if (result->result != ESP_OK) {
                        break;
                    }
                    downloaded_stems.push_back(wqn::SafePackStemFromId(item.pack_id));
                }
                if (result->result != ESP_OK && !downloaded_stems.empty()) {
                    ESP_LOGW(kTag, "rolling back %u downloaded packs due to error",
                             static_cast<unsigned>(downloaded_stems.size()));
                    for (const std::string& stem : downloaded_stems) {
                        std::string path = std::string("/storage/wp_") + stem + ".wqwp";
                        std::remove(path.c_str());
                    }
                }
            }
            if (result->result == ESP_OK) {
                result->result = wqn::SaveWordPackManifest(manifest);
            }
            if (result->result == ESP_OK) {
                result->result = wqn::LoadWordPackIndex(&result->pack_index);
                result->message = result->pack_index.status_message;
            }
        } else if (request.op == WordCloudOp::kSubmit) {
            wqn::WqnWordReviewSubmission submission;
            submission.word_id = request.word_id;
            submission.outcome = request.outcome;
            submission.mode = "sequential";
            result->result = wqn::SubmitWordReview(token, submission, &result->submit);
        } else if (request.op == WordCloudOp::kSearch) {
            wqn::WqnWordSearchRequest search;
            search.query = request.query;
            search.limit = 8;
            result->result = wqn::SearchWords(token, search, &result->search);
        } else if (request.op == WordCloudOp::kAiLookup) {
            wqn::WqnWordAiLookupRequest lookup;
            lookup.query = request.query;
            result->result = wqn::LookupWordWithAi(token, lookup, &result->lookup);
        } else {
            result->result = ESP_ERR_INVALID_ARG;
        }

        if (result->result != ESP_OK) {
            std::string after_token;
            result->auth_required = !LoadValidTokenForTodo(&after_token);
        }
        SendWordCloudResult(result);
    }
}

}  // namespace device_ui_internal
