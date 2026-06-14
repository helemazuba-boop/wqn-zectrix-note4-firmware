// Word review page rendering: home, dictionary, lookup choice, card front/back.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>

#include "epd_display.h"
#include "esp_log.h"
#include "word_app.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::WordAppSnapshot& word = frame.word_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar("单词", frame.home);

    ESP_RETURN_ON_ERROR(DrawClippedText(10, 36, 250, word.status_line), kTag, "draw word status");
    ESP_RETURN_ON_ERROR(DrawClippedText(282, 36, 108, word.progress_line), kTag, "draw word progress");
    DrawHorizontalLine(10, 58, 380);

    auto draw_choice = [](int y, const std::string& title, const std::string& subtitle, bool selected) -> esp_err_t {
        const int x = 38;
        const int width = 324;
        const int height = 42;
        if (selected) {
            DrawRect(x - 3, y - 3, width + 6, height + 6);
            DrawRect(x - 1, y - 1, width + 2, height + 2);
        } else {
            DrawRect(x, y, width, height);
        }
        ESP_RETURN_ON_ERROR(DrawClippedText(x + 12, y + 7, 180, title), kTag, "draw word choice title");
        ESP_RETURN_ON_ERROR(DrawClippedText(x + 160, y + 7, 150, subtitle), kTag, "draw word choice subtitle");
        return ESP_OK;
    };

    auto draw_word_back = [&word]() -> esp_err_t {
        DrawRect(22, 128, 356, 116);
        const std::string title = word.part_of_speech.empty() ? word.meaning : word.part_of_speech + "  " + word.meaning;
        ESP_RETURN_ON_ERROR(DrawWrappedText(34, 140, 332, title, 2), kTag, "draw word meaning");
        if (!word.example.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(34, 184, 332, word.example, 2), kTag, "draw word example");
        }
        if (!word.example_translation.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(34, 224, 332, word.example_translation, 1), kTag, "draw word translation");
        }
        return ESP_OK;
    };

    if (word.mode == wqn::WordAppMode::kHome) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 72, 360, "单词复习"), kTag, "draw word home title");
        ESP_RETURN_ON_ERROR(
            draw_choice(110, "顺序复习", word.pack_ready ? "从词库开始" : "需同步词库", word.home_selection == wqn::WordHomeSelection::kSequential),
            kTag,
            "draw sequential choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(162, "随机复习", word.pack_ready ? "打乱今日词" : "需同步词库", word.home_selection == wqn::WordHomeSelection::kRandom),
            kTag,
            "draw random choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(214, "词典", word.pack_ready ? "按字母查词" : "在线同步后使用", word.home_selection == wqn::WordHomeSelection::kDictionary),
            kTag,
            "draw dictionary choice");
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        if (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig) {
            return RefreshStableRegion({0, 64, wqn::kEpdWidth, 220, "word-home"}, schedule);
        }
        return RefreshFrame(frame, schedule);
    }

    if (word.mode == wqn::WordAppMode::kDictionary) {
        const std::string prefix = word.dictionary_prefix.empty() ? "选择首字母" : word.dictionary_prefix;
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 70, 360, prefix), kTag, "draw dictionary prefix");
        const int start_x = 28;
        const int start_y = 108;
        const int cell_w = 42;
        const int cell_h = 30;
        for (size_t i = 0; i < word.dictionary_letters.size() && i < 24; ++i) {
            const int col = static_cast<int>(i % 8);
            const int row = static_cast<int>(i / 8);
            const int x = start_x + col * cell_w;
            const int y = start_y + row * cell_h;
            if (i == word.dictionary_letter_selected) {
                DrawRect(x - 2, y - 2, cell_w - 4, cell_h - 2);
            }
            char letter[2] = {word.dictionary_letters[i], 0};
            ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 7, cell_w - 8, letter), kTag, "draw dictionary letter");
        }
        int y = 212;
        for (size_t i = 0; i < word.dictionary_preview_words.size() && i < 3; ++i) {
            const std::string marker = i == word.dictionary_match_selected ? "> " : "  ";
            ESP_RETURN_ON_ERROR(DrawClippedText(42, y, 300, marker + word.dictionary_preview_words[i]), kTag, "draw dictionary preview");
            y += 22;
        }
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        return RefreshFrame(frame, schedule);
    }

    if (word.mode == wqn::WordAppMode::kLookupChoice) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 76, 360, word.dictionary_prefix), kTag, "draw lookup query");
        ESP_RETURN_ON_ERROR(
            draw_choice(128, "在线搜索", "查 WQN 服务器", word.lookup_selection == wqn::WordLookupSelection::kOnlineSearch),
            kTag,
            "draw online lookup choice");
        ESP_RETURN_ON_ERROR(
            draw_choice(182, "询问 AI", "临时释义", word.lookup_selection == wqn::WordLookupSelection::kAiLookup),
            kTag,
            "draw ai lookup choice");
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        return RefreshFrame(frame, schedule);
    }

    if (!word.has_card) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 118, 360, "词库未同步"), kTag, "draw word empty title");
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 148, 360, word.hint), kTag, "draw word empty body");
        return RefreshFrame(frame, schedule);
    }

    const std::string position =
        word.card_count == 0 ? "" : std::to_string(word.card_position) + "/" + std::to_string(std::max<uint16_t>(1, word.card_count));
    ESP_RETURN_ON_ERROR(DrawCenteredText(20, 68, 360, position), kTag, "draw word position");

    ESP_RETURN_ON_ERROR(DrawCenteredText(20, 94, 360, word.word), kTag, "draw word headword");
    if (!word.phonetic.empty()) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 118, 360, word.phonetic), kTag, "draw word phonetic");
    }

    if (word.mode == wqn::WordAppMode::kReviewFront) {
        DrawRect(74, 164, 252, 48);
        ESP_RETURN_ON_ERROR(DrawCenteredText(74, 181, 252, "先回忆释义"), kTag, "draw recall prompt");
    } else {
        ESP_RETURN_ON_ERROR(draw_word_back(), kTag, "draw word back");
    }

    ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
