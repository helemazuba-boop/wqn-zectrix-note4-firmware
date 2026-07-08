// AI history implementation: PSRAM-backed ring buffer.

#include "ai_history.h"

#include <cstring>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace wqn {

namespace {
constexpr char kTag[] = "ai_history";
constexpr size_t kAlignBytes = 64;
}  // namespace

// ============================================================================
// PsramBufferResource
// ============================================================================

PsramBufferResource::PsramBufferResource(size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    // Round up to alignment to keep std::pmr happy.
    const size_t aligned = (bytes + kAlignBytes - 1) & ~(kAlignBytes - 1);
    chunk_ = heap_caps_malloc(aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk_ != nullptr) {
        chunk_size_ = aligned;
        using_psram_ = true;
        return;
    }
    ESP_LOGW(kTag, "PSRAM not available (%zu bytes), falling back to internal heap",
             aligned);
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
        // Out-of-band: caller asked for more than the configured chunk. Use
        // the matching cap so we are still inside our memory budget hint.
        const size_t cap_flag = using_psram_ ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                             : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        return heap_caps_aligned_alloc(alignment, bytes, cap_flag);
    }
    // monotonic_buffer_resource will only request up to its upstream pool.
    // Just forward to the underlying malloc for this scoped arena.
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
    // Allocations inside the monotonic buffer resource are never released
    // individually — the whole arena goes away with the pool destructor.
    // The fallback path (heap_caps_aligned_alloc) still owns the block so we
    // must free it.
    if (p != chunk_ && (reinterpret_cast<uint8_t*>(p) < reinterpret_cast<uint8_t*>(chunk_) ||
                         reinterpret_cast<uint8_t*>(p) >=
                             reinterpret_cast<uint8_t*>(chunk_) + chunk_size_)) {
        heap_caps_free(p);
    } else {
        // Owned by the arena; ignore.
        (void)bytes;
    }
}

bool PsramBufferResource::do_is_equal(const memory_resource& other) const noexcept
{
    return this == &other;
}

// ============================================================================
// AiHistory
// ============================================================================

esp_err_t AiHistory::Init(size_t cap_bytes)
{
    if (initialized_) {
        return ESP_OK;
    }
    cap_bytes_ = cap_bytes;
    auto* arena = new PsramBufferResource(cap_bytes);
    heap_ = arena;
    // Construct the monotonic pool over the arena in place; the type is
    // move-only so we destroy any prior value first and re-init via the
    // allocator-aware constructor.
    pool_.release();
    new (&pool_) std::pmr::monotonic_buffer_resource(cap_bytes, arena);
    byte_size_ = 0;
    messages_.clear();
    initialized_ = true;
    ESP_LOGI(kTag, "Init cap=%zu bytes psram=%d",
             cap_bytes_, arena->using_psram() ? 1 : 0);
    return ESP_OK;
}

void AiHistory::Clear()
{
    messages_.clear();
    byte_size_ = 0;
}

void AiHistory::Append(ChatMessage msg)
{
    if (!initialized_) {
        return;
    }
    // Move-construct into the deque so the strings route through the PMR pool.
    // The temporaries in `msg` were passed by value so this is cheap.
    const size_t cost = CostOf(msg);
    messages_.push_back(std::move(msg));
    byte_size_ += cost;

    // Trim from the front until we fit under the cap. We always keep at
    // least the most recent message so a streaming answer never disappears
    // entirely.
    while (byte_size_ > cap_bytes_ && messages_.size() > 1) {
        byte_size_ -= CostOf(messages_.front());
        messages_.pop_front();
    }
}

size_t AiHistory::CostOf(const ChatMessage& m) const
{
    size_t total = sizeof(ChatMessage);
    total += m.text.size();
    total += m.tool_name.size();
    total += m.tool_args_json.size();
    total += m.tool_result_json.size();
    return total;
}

void AiHistory::ReleaseMessageStorage()
{
    // No-op: monotonic buffer never frees. Kept for future rebalance code.
}

void AiHistory::AppendUser(std::pmr::string text, int64_t now_ms)
{
    ChatMessage m;
    m.kind = ChatMessageKind::kUser;
    m.timestamp_ms = now_ms;
    m.text = std::move(text);
    Append(std::move(m));
}

void AiHistory::AppendAssistant(std::pmr::string text, int64_t now_ms)
{
    ChatMessage m;
    m.kind = ChatMessageKind::kAssistant;
    m.timestamp_ms = now_ms;
    m.text = std::move(text);
    Append(std::move(m));
}

void AiHistory::AppendThinking(std::pmr::string text, int64_t now_ms)
{
    ChatMessage m;
    m.kind = ChatMessageKind::kThinking;
    m.timestamp_ms = now_ms;
    m.text = std::move(text);
    Append(std::move(m));
}

void AiHistory::AppendToolStart(std::pmr::string name, std::pmr::string args, int64_t now_ms)
{
    ChatMessage m;
    m.kind = ChatMessageKind::kToolStart;
    m.timestamp_ms = now_ms;
    m.tool_name = std::move(name);
    m.tool_args_json = std::move(args);
    Append(std::move(m));
}

void AiHistory::AppendToolResult(std::pmr::string name, std::pmr::string args,
                                 std::pmr::string result, bool ok, int32_t elapsed_ms,
                                 int64_t now_ms)
{
    ChatMessage m;
    m.kind = ChatMessageKind::kToolResult;
    m.timestamp_ms = now_ms;
    m.tool_name = std::move(name);
    m.tool_args_json = std::move(args);
    m.tool_result_json = std::move(result);
    m.tool_ok = ok;
    m.tool_elapsed_ms = elapsed_ms;
    Append(std::move(m));
}

bool AiHistory::PopLastIf(ChatMessageKind kind)
{
    if (messages_.empty() || messages_.back().kind != kind) {
        return false;
    }
    byte_size_ -= CostOf(messages_.back());
    messages_.pop_back();
    return true;
}

const ChatMessage* AiHistory::LatestTool() const
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->kind == ChatMessageKind::kToolStart || it->kind == ChatMessageKind::kToolResult) {
            return &(*it);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Process-global accessor (single conversation for the whole device).
// ---------------------------------------------------------------------------

namespace {
AiHistory g_history;
bool g_history_inited = false;
}  // namespace

AiHistory& GetAiHistory()
{
    if (!g_history_inited) {
        constexpr size_t kCapBytes = 512 * 1024;
        if (g_history.Init(kCapBytes) == ESP_OK) {
            g_history_inited = true;
        }
    }
    return g_history;
}

}  // namespace wqn
