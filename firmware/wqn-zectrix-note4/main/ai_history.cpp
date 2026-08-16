// AI history implementation: PSRAM-backed ring buffer + immutable UI snapshots.

#include "ai_history.h"

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace wqn {

namespace {
constexpr char kTag[] = "ai_history";
constexpr size_t kCapBytes = 512 * 1024;
}  // namespace

bool AiHistory::PsramMemoryResource::using_psram() const
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

void* AiHistory::PsramMemoryResource::do_allocate(size_t bytes, size_t alignment)
{
    void* p = heap_caps_aligned_alloc(
        alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_aligned_alloc(
            alignment, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (p == nullptr) {
        ESP_LOGE(kTag, "history allocation failed: %zu bytes align=%zu", bytes, alignment);
        std::abort();
    }
    return p;
}

void AiHistory::PsramMemoryResource::do_deallocate(
    void* p, size_t bytes, size_t alignment)
{
    (void)bytes;
    (void)alignment;
    if (p != nullptr) {
        heap_caps_free(p);
    }
}

bool AiHistory::PsramMemoryResource::do_is_equal(
    const memory_resource& other) const noexcept
{
    return this == &other;
}

AiHistory::AiHistory()
    : mutex_(xSemaphoreCreateMutexStatic(&mutex_storage_))
{
}

esp_err_t AiHistory::Init(size_t cap_bytes)
{
    if (mutex_ == nullptr) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (initialized_) {
        xSemaphoreGive(mutex_);
        return ESP_OK;
    }

    cap_bytes_ = cap_bytes;
    byte_size_ = 0;
    messages_.clear();
    next_message_id_ = 1;
    revision_ = 0;
    cached_snapshot_.reset();
    cached_snapshot_revision_ = UINT64_MAX;
    initialized_ = true;
    ESP_LOGI(kTag, "Init cap=%zu bytes psram=%d", cap_bytes_, heap_.using_psram() ? 1 : 0);
    xSemaphoreGive(mutex_);
    return ESP_OK;
}

void AiHistory::Clear()
{
    if (mutex_ == nullptr) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!messages_.empty()) {
        messages_.clear();
        byte_size_ = 0;
        ++revision_;
    }
    xSemaphoreGive(mutex_);
}

size_t AiHistory::CostOf(const ChatMessage& m) const
{
    return sizeof(ChatMessage) + m.text.size() + m.tool_name.size() +
           m.tool_args_json.size() + m.tool_result_json.size();
}

void AiHistory::TrimLocked()
{
    while (byte_size_ > cap_bytes_ && messages_.size() > 1) {
        byte_size_ -= CostOf(messages_.front());
        messages_.pop_front();
    }
}

ChatMessageId AiHistory::AppendLocked(ChatMessage msg)
{
    if (!initialized_) {
        return kInvalidChatMessageId;
    }
    msg.id = next_message_id_++;
    const ChatMessageId id = msg.id;
    byte_size_ += CostOf(msg);
    messages_.push_back(std::move(msg));
    TrimLocked();
    ++revision_;
    return id;
}

ChatMessageId AiHistory::AppendUser(std::string_view text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessage msg(&heap_);
    msg.kind = ChatMessageKind::kUser;
    msg.timestamp_ms = now_ms;
    msg.text.assign(text.data(), text.size());
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendAssistant(std::string_view text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessage msg(&heap_);
    msg.kind = ChatMessageKind::kAssistant;
    msg.timestamp_ms = now_ms;
    msg.text.assign(text.data(), text.size());
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendThinking(std::string_view text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessage msg(&heap_);
    msg.kind = ChatMessageKind::kThinking;
    msg.timestamp_ms = now_ms;
    msg.text.assign(text.data(), text.size());
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendToolStart(std::string_view name, std::string_view args,
                                         int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessage msg(&heap_);
    msg.kind = ChatMessageKind::kToolStart;
    msg.timestamp_ms = now_ms;
    msg.tool_name.assign(name.data(), name.size());
    msg.tool_args_json.assign(args.data(), args.size());
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendToolResult(std::string_view name, std::string_view args,
                                          std::string_view result, bool ok,
                                          int32_t elapsed_ms, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessage msg(&heap_);
    msg.kind = ChatMessageKind::kToolResult;
    msg.timestamp_ms = now_ms;
    msg.tool_name.assign(name.data(), name.size());
    msg.tool_args_json.assign(args.data(), args.size());
    msg.tool_result_json.assign(result.data(), result.size());
    msg.tool_ok = ok;
    msg.tool_elapsed_ms = elapsed_ms;
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

bool AiHistory::ReplaceText(ChatMessageId id, ChatMessageKind expected_kind,
                            std::string_view text, int64_t now_ms)
{
    if (id == kInvalidChatMessageId || mutex_ == nullptr) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (ChatMessage& msg : messages_) {
        if (msg.id != id) continue;
        if (msg.kind != expected_kind) {
            xSemaphoreGive(mutex_);
            return false;
        }
        if (msg.text.size() == text.size() &&
            std::memcmp(msg.text.data(), text.data(), text.size()) == 0) {
            xSemaphoreGive(mutex_);
            return true;
        }
        byte_size_ -= CostOf(msg);
        msg.text.assign(text.data(), text.size());
        msg.timestamp_ms = now_ms;
        byte_size_ += CostOf(msg);
        TrimLocked();
        ++revision_;
        xSemaphoreGive(mutex_);
        return true;
    }
    xSemaphoreGive(mutex_);
    return false;
}

bool AiHistory::PopLastIf(ChatMessageKind kind)
{
    if (mutex_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (messages_.empty() || messages_.back().kind != kind) {
        xSemaphoreGive(mutex_);
        return false;
    }
    byte_size_ -= CostOf(messages_.back());
    messages_.pop_back();
    ++revision_;
    xSemaphoreGive(mutex_);
    return true;
}

std::shared_ptr<const AiHistorySnapshot> AiHistory::Snapshot() const
{
    if (mutex_ == nullptr) return std::make_shared<AiHistorySnapshot>();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // [ai-memory-fix] RenderUiFrame can be called more than once while
    // comparing signatures. Reuse the immutable DTO until history changes
    // instead of rebuilding every string/vector on each comparison.
    if (cached_snapshot_ != nullptr && cached_snapshot_revision_ == revision_) {
        const std::shared_ptr<const AiHistorySnapshot> cached = cached_snapshot_;
        xSemaphoreGive(mutex_);
        return cached;
    }
    auto snapshot = std::make_shared<AiHistorySnapshot>();
    snapshot->revision = revision_;
    snapshot->messages.reserve(messages_.size());
    for (const ChatMessage& msg : messages_) {
        ChatMessageSnapshot out;
        out.id = msg.id;
        out.kind = msg.kind;
        out.timestamp_ms = msg.timestamp_ms;
        out.text.assign(msg.text.data(), msg.text.size());
        out.tool_name.assign(msg.tool_name.data(), msg.tool_name.size());
        out.tool_args_json.assign(msg.tool_args_json.data(), msg.tool_args_json.size());
        out.tool_result_json.assign(msg.tool_result_json.data(), msg.tool_result_json.size());
        out.tool_elapsed_ms = msg.tool_elapsed_ms;
        out.tool_ok = msg.tool_ok;
        snapshot->messages.push_back(std::move(out));
    }
    cached_snapshot_ = snapshot;
    cached_snapshot_revision_ = revision_;
    xSemaphoreGive(mutex_);
    return snapshot;
}

size_t AiHistory::size() const
{
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t value = messages_.size();
    xSemaphoreGive(mutex_);
    return value;
}

bool AiHistory::empty() const { return size() == 0; }

size_t AiHistory::byte_size() const
{
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t value = byte_size_;
    xSemaphoreGive(mutex_);
    return value;
}

uint64_t AiHistory::revision() const
{
    if (mutex_ == nullptr) return 0;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    uint64_t value = revision_;
    xSemaphoreGive(mutex_);
    return value;
}

namespace {
AiHistory g_history_stdpro;
AiHistory g_history_flash;
}  // namespace

AiHistory& GetAiHistory(AiHistoryChannel channel)
{
    AiHistory& history = channel == AiHistoryChannel::kFlash
        ? g_history_flash
        : g_history_stdpro;
    (void)history.Init(kCapBytes);
    return history;
}

std::shared_ptr<const AiHistorySnapshot> GetAiHistorySnapshot(AiHistoryChannel channel)
{
    return GetAiHistory(channel).Snapshot();
}

}  // namespace wqn
