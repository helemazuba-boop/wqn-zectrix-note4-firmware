// AI history implementation: PSRAM-backed ring buffer + immutable UI snapshots.

#include "ai_history.h"

#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace wqn {

namespace {
constexpr char kTag[] = "ai_history";
constexpr size_t kAlignBytes = 64;
constexpr size_t kCapBytes = 512 * 1024;
}  // namespace

PsramBufferResource::PsramBufferResource(size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    const size_t aligned = (bytes + kAlignBytes - 1) & ~(kAlignBytes - 1);
    chunk_ = heap_caps_malloc(aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk_ != nullptr) {
        chunk_size_ = aligned;
        using_psram_ = true;
        return;
    }
    ESP_LOGW(kTag, "PSRAM not available (%zu bytes), falling back to internal heap", aligned);
    chunk_ = heap_caps_malloc(aligned, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (chunk_ != nullptr) {
        chunk_size_ = aligned;
    } else {
        ESP_LOGE(kTag, "alloc failed: %zu bytes", aligned);
    }
}

PsramBufferResource::~PsramBufferResource()
{
    if (chunk_ != nullptr) {
        heap_caps_free(chunk_);
        chunk_ = nullptr;
        chunk_size_ = 0;
    }
}

void* PsramBufferResource::do_allocate(size_t bytes, size_t alignment)
{
    if (chunk_ == nullptr) {
        const size_t cap_flag = using_psram_ ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                             : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return heap_caps_aligned_alloc(alignment, bytes, cap_flag);
    }
    (void)alignment;
    const size_t cap_flag = using_psram_ ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                         : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return heap_caps_malloc(bytes, cap_flag);
}

void PsramBufferResource::do_deallocate(void* p, size_t bytes, size_t alignment)
{
    (void)alignment;
    if (p == nullptr) {
        return;
    }
    if (p != chunk_ && (reinterpret_cast<uint8_t*>(p) < reinterpret_cast<uint8_t*>(chunk_) ||
                         reinterpret_cast<uint8_t*>(p) >=
                             reinterpret_cast<uint8_t*>(chunk_) + chunk_size_)) {
        heap_caps_free(p);
    } else {
        (void)bytes;
    }
}

bool PsramBufferResource::do_is_equal(const memory_resource& other) const noexcept
{
    return this == &other;
}

esp_err_t AiHistory::Init(size_t cap_bytes)
{
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
        if (mutex_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (initialized_) {
        xSemaphoreGive(mutex_);
        return ESP_OK;
    }

    cap_bytes_ = cap_bytes;
    auto* arena = new (std::nothrow) PsramBufferResource(cap_bytes);
    if (arena == nullptr) {
        xSemaphoreGive(mutex_);
        return ESP_ERR_NO_MEM;
    }
    heap_ = arena;
    pool_.release();
    new (&pool_) std::pmr::monotonic_buffer_resource(cap_bytes, arena);
    byte_size_ = 0;
    messages_.clear();
    next_message_id_ = 1;
    revision_ = 0;
    initialized_ = true;
    ESP_LOGI(kTag, "Init cap=%zu bytes psram=%d", cap_bytes_, arena->using_psram() ? 1 : 0);
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

ChatMessageId AiHistory::AppendUser(std::pmr::string text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    ChatMessage msg;
    msg.kind = ChatMessageKind::kUser;
    msg.timestamp_ms = now_ms;
    msg.text = std::move(text);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendAssistant(std::pmr::string text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    ChatMessage msg;
    msg.kind = ChatMessageKind::kAssistant;
    msg.timestamp_ms = now_ms;
    msg.text = std::move(text);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendThinking(std::pmr::string text, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    ChatMessage msg;
    msg.kind = ChatMessageKind::kThinking;
    msg.timestamp_ms = now_ms;
    msg.text = std::move(text);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendToolStart(std::pmr::string name, std::pmr::string args,
                                         int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    ChatMessage msg;
    msg.kind = ChatMessageKind::kToolStart;
    msg.timestamp_ms = now_ms;
    msg.tool_name = std::move(name);
    msg.tool_args_json = std::move(args);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

ChatMessageId AiHistory::AppendToolResult(std::pmr::string name, std::pmr::string args,
                                          std::pmr::string result, bool ok,
                                          int32_t elapsed_ms, int64_t now_ms)
{
    if (mutex_ == nullptr) return kInvalidChatMessageId;
    ChatMessage msg;
    msg.kind = ChatMessageKind::kToolResult;
    msg.timestamp_ms = now_ms;
    msg.tool_name = std::move(name);
    msg.tool_args_json = std::move(args);
    msg.tool_result_json = std::move(result);
    msg.tool_ok = ok;
    msg.tool_elapsed_ms = elapsed_ms;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ChatMessageId id = AppendLocked(std::move(msg));
    xSemaphoreGive(mutex_);
    return id;
}

bool AiHistory::ReplaceText(ChatMessageId id, ChatMessageKind expected_kind,
                            std::pmr::string text, int64_t now_ms)
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
        const std::string incoming(text.data(), text.size());
        if (msg.text.size() == incoming.size() &&
            std::memcmp(msg.text.data(), incoming.data(), incoming.size()) == 0) {
            xSemaphoreGive(mutex_);
            return true;
        }
        byte_size_ -= CostOf(msg);
        msg.text.assign(incoming.data(), incoming.size());
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
    auto snapshot = std::make_shared<AiHistorySnapshot>();
    if (mutex_ == nullptr) return snapshot;
    xSemaphoreTake(mutex_, portMAX_DELAY);
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
bool g_stdpro_inited = false;
bool g_flash_inited = false;
}  // namespace

AiHistory& GetAiHistory(AiHistoryChannel channel)
{
    if (channel == AiHistoryChannel::kFlash) {
        if (!g_flash_inited && g_history_flash.Init(kCapBytes) == ESP_OK) {
            g_flash_inited = true;
        }
        return g_history_flash;
    }
    if (!g_stdpro_inited && g_history_stdpro.Init(kCapBytes) == ESP_OK) {
        g_stdpro_inited = true;
    }
    return g_history_stdpro;
}

std::shared_ptr<const AiHistorySnapshot> GetAiHistorySnapshot(AiHistoryChannel channel)
{
    return GetAiHistory(channel).Snapshot();
}

}  // namespace wqn
