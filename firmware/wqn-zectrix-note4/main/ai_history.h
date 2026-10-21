// AI conversation history: PSRAM-backed ring buffer for chat messages and tool blocks.
// Rebuilt each boot — no NVS persistence. STD/Pro, Flash and Agent (OpenCode)
// use independent histories.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace wqn {

using ChatMessageId = uint64_t;
constexpr ChatMessageId kInvalidChatMessageId = 0;

enum class ChatMessageKind : uint8_t {
    kUser,
    kAssistant,
    kThinking,
    kToolStart,
    kToolResult,
};

// [agent] One channel per AI-page tier. kAgent carries the OpenCode gateway
// conversation, which shares the chat bubble / markdown / tool-block renderer
// with kStdPro but must never be merged into it: the two backends answer the
// same prompts with completely different content and lifetimes.
enum class AiHistoryChannel : uint8_t {
    kStdPro,
    kFlash,
    kAgent,
};

struct ChatMessage {
    explicit ChatMessage(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : text(resource),
          tool_name(resource),
          tool_args_json(resource),
          tool_result_json(resource)
    {
    }

    ChatMessageId id = kInvalidChatMessageId;
    ChatMessageKind kind = ChatMessageKind::kUser;
    int64_t timestamp_ms = 0;
    std::pmr::string text;
    std::pmr::string tool_name;
    std::pmr::string tool_args_json;
    std::pmr::string tool_result_json;
    int32_t tool_elapsed_ms = 0;
    bool tool_ok = false;
    int rendered_rows_hint = 0;
};

// Plain immutable DTO used by UiFrame. It never borrows PMR storage from AiHistory.
struct ChatMessageSnapshot {
    ChatMessageId id = kInvalidChatMessageId;
    ChatMessageKind kind = ChatMessageKind::kUser;
    int64_t timestamp_ms = 0;
    std::string text;
    std::string tool_name;
    std::string tool_args_json;
    std::string tool_result_json;
    int32_t tool_elapsed_ms = 0;
    bool tool_ok = false;
};

struct AiHistorySnapshot {
    uint64_t revision = 0;
    std::vector<ChatMessageSnapshot> messages;
};

class AiHistory {
public:
    AiHistory();

    esp_err_t Init(size_t cap_bytes);
    void Clear();

    ChatMessageId AppendUser(std::string_view text, int64_t now_ms);
    ChatMessageId AppendAssistant(std::string_view text, int64_t now_ms);
    ChatMessageId AppendThinking(std::string_view text, int64_t now_ms);
    ChatMessageId AppendToolStart(std::string_view name, std::string_view args,
                                  int64_t now_ms);
    ChatMessageId AppendToolResult(std::string_view name, std::string_view args,
                                   std::string_view result, bool ok, int32_t elapsed_ms,
                                   int64_t now_ms);

    // Replace a known message without changing order. The kind guard prevents a
    // late event from overwriting a different message after eviction/clear.
    //
    // [evict-recovery] The id can be gone for two very different reasons -- the ring
    // evicted it, or it never belonged to this kind -- and the return value
    // cannot tell them apart, which mattered once AiSession began mirroring
    // streamed text into history on every render tick (doc/1005 item D1): a
    // long answer grows one entry for its whole lifetime, so eviction became
    // reachable mid-answer, and a caller that mistook "evicted" for "kind
    // mismatch" would re-append and duplicate the bubble while a caller that
    // mistook it for success would lose the answer outright. Contains()
    // answers which of the two it was.
    bool ReplaceText(ChatMessageId id, ChatMessageKind expected_kind,
                     std::string_view text, int64_t now_ms);

    // True while `id` is still in the ring AT ALL, with any kind. The streaming
    // mirror needs to know which of two very different things ReplaceText's
    // false return meant -- "the ring evicted the entry I have been growing for
    // the whole answer" (safe to re-append) or "this id belongs to some other
    // message" (a logic error; re-appending duplicates the bubble) -- and the
    // bare bool cannot tell them apart.
    //
    // [evict-recovery] There is deliberately no kind parameter. The caller has
    // just been told the entry does not carry the kind it expected, so "is it
    // here with that kind?" is a question the false it already holds answers.
    // The discriminator has to be presence alone; an earlier version took the
    // kind and therefore returned false for BOTH cases, which made the recovery
    // branch unconditional -- exactly the duplicate-bubble behaviour it was
    // written to prevent.
    bool Contains(ChatMessageId id) const;

    bool PopLastIf(ChatMessageKind kind);
    std::shared_ptr<const AiHistorySnapshot> Snapshot() const;

    size_t size() const;
    bool empty() const;
    size_t byte_size() const;
    size_t cap() const { return cap_bytes_; }
    uint64_t revision() const;

private:
    ChatMessageId AppendLocked(ChatMessage msg);
    size_t CostOf(const ChatMessage& m) const;
    void TrimLocked();

    class PsramMemoryResource : public std::pmr::memory_resource {
    public:
        bool using_psram() const;

    private:
        void* do_allocate(size_t bytes, size_t alignment) override;
        void do_deallocate(void* p, size_t bytes, size_t alignment) override;
        bool do_is_equal(const memory_resource& other) const noexcept override;
    };

    PsramMemoryResource heap_;
    std::deque<ChatMessage> messages_;
    // Built lazily and reused until revision_ changes. Protected by mutex_.
    // UiFrame instances keep their own shared reference after the lock drops.
    mutable std::shared_ptr<const AiHistorySnapshot> cached_snapshot_;
    mutable uint64_t cached_snapshot_revision_ = UINT64_MAX;
    mutable StaticSemaphore_t mutex_storage_{};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    size_t byte_size_ = 0;
    size_t cap_bytes_ = 0;
    ChatMessageId next_message_id_ = 1;
    uint64_t revision_ = 0;
    bool initialized_ = false;
};

// Explicit channel selection prevents a late background stream from writing to
// the history of whichever tier happens to be visible now.
AiHistory& GetAiHistory(AiHistoryChannel channel);
std::shared_ptr<const AiHistorySnapshot> GetAiHistorySnapshot(AiHistoryChannel channel);

}  // namespace wqn
