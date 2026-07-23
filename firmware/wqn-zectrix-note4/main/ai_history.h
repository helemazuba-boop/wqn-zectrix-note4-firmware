// AI conversation history: PSRAM-backed ring buffer for chat messages and tool blocks.
// Rebuilt each boot — no NVS persistence. STD/Pro and Flash use independent histories.

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

enum class AiHistoryChannel : uint8_t {
    kStdPro,
    kFlash,
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
    bool ReplaceText(ChatMessageId id, ChatMessageKind expected_kind,
                     std::string_view text, int64_t now_ms);

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
