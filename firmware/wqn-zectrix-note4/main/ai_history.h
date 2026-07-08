// AI conversation history: PSRAM-backed ring buffer for chat messages and tool blocks.
// Rebuilt each boot — no NVS persistence. Single conversation per device.
//
// The structure is intentionally simple: std::deque<ChatMessage> over a
// std::pmr::monotonic_buffer_resource so every string lives in one contiguous
// PSRAM region that can be reclaimed in O(1) when the cap is exceeded.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <string>

#include "esp_err.h"

namespace wqn {

enum class ChatMessageKind : uint8_t {
    kUser,
    kAssistant,
    kThinking,
    kToolStart,   // placeholder emitted when tool.start arrives
    kToolResult,  // finalised tool block: tool_name + args + result + elapsed
};

struct ChatMessage {
    ChatMessageKind kind = ChatMessageKind::kUser;
    int64_t timestamp_ms = 0;

    // For kUser / kAssistant / kThinking / kToolResult.
    std::pmr::string text;

    // Tool block payload (populated for kToolStart / kToolResult).
    std::pmr::string tool_name;
    std::pmr::string tool_args_json;
    std::pmr::string tool_result_json;
    int32_t tool_elapsed_ms = 0;
    bool tool_ok = false;

    // Local-only auxiliary state (UI sizing hint): approximate rendered row
    // count after wrap. Filled by the renderer on first draw, since messages
    // of varying length cost different row budgets.
    int rendered_rows_hint = 0;
};

class AiHistory {
public:
    // Allocates an internal monotonic buffer of `cap_bytes` from PSRAM when
    // available. Falls back to internal heap if PSRAM is missing. Idempotent.
    esp_err_t Init(size_t cap_bytes);

    // Clear history (does not release the PSRAM pool — reuse it).
    void Clear();

    // Append a copy of `msg`. Allocates from the PSRAM pool. Trims oldest
    // entries (from the front of the deque) until total cost ≤ cap.
    void Append(ChatMessage msg);

    // Convenience builders — push a fully-formed message in one call.
    void AppendUser(std::pmr::string text, int64_t now_ms);
    void AppendAssistant(std::pmr::string text, int64_t now_ms);
    void AppendThinking(std::pmr::string text, int64_t now_ms);
    void AppendToolStart(std::pmr::string name, std::pmr::string args, int64_t now_ms);
    void AppendToolResult(std::pmr::string name, std::pmr::string args,
                          std::pmr::string result, bool ok, int32_t elapsed_ms,
                          int64_t now_ms);

    // Number of stored messages.
    size_t size() const { return messages_.size(); }

    // True if there is no recorded message yet.
    bool empty() const { return messages_.empty(); }

    // Index from the front of the deque (0 = oldest, size-1 = newest).
    const ChatMessage& at(size_t index) const { return messages_[index]; }

    // Removes the latest message if it matches kind. Returns true if popped.
    bool PopLastIf(ChatMessageKind kind);

    // Approximate byte cost of all stored strings.
    size_t byte_size() const { return byte_size_; }

    // Configured cap (bytes).
    size_t cap() const { return cap_bytes_; }

    // Lookup the most recent (front) tool message by id; nullptr if none.
    const ChatMessage* LatestTool() const;

private:
    // Track the in-pool byte cost of one message so eviction stays cheap.
    size_t CostOf(const ChatMessage& m) const;

    void ReleaseMessageStorage();

    std::pmr::monotonic_buffer_resource pool_;
    std::pmr::memory_resource* heap_ = nullptr;   // non-owning, may be PSRAM pool or upstream
    std::deque<ChatMessage> messages_;
    size_t byte_size_ = 0;
    size_t cap_bytes_ = 0;
    bool initialized_ = false;
};

// PSRAM-aware allocator backend used by the monotonic pool. Pulls one big
// chunk via heap_caps_malloc when PSRAM is present, otherwise falls back to
// plain heap_alloc_caps.
class PsramBufferResource : public std::pmr::memory_resource {
public:
    explicit PsramBufferResource(size_t bytes);
    ~PsramBufferResource() override;

    bool using_psram() const { return using_psram_; }

private:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* p, size_t bytes, size_t alignment) override;
    bool do_is_equal(const memory_resource& other) const noexcept override;

    void* chunk_ = nullptr;
    size_t chunk_size_ = 0;
    bool using_psram_ = false;
};

// Process-global accessor used by ai_session.cpp and the AI page renderer.
// Idempotent — first call lazily allocates the underlying PSRAM pool.
AiHistory& GetAiHistory();

}  // namespace wqn
