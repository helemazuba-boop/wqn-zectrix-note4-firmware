// Word page rendering: home, dictionary picker and one shared card surface.
// Extracted from device_ui.cpp.

#include "ui_internal.h"

#include <string>

#include "display_service.h"
#include "esp_log.h"
#include "ui/assets/font_wqn_card_24_1.h"
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
        // [word-home-cards] Prototype 16: three rounded feature cards in the
        // content area (no centered "单词复习" big title — the status bar already
        // says 单词). Selected = rounded + 2px concentric double-line (user's
        // rounded design language). Left = 24px 1bpp asset, center = title +
        // dynamic subtitle, right = a rounded status chip.
        constexpr int kCardX = 10;
        constexpr int kCardW = 380;
        constexpr int kCardH = 60;
        constexpr int kCardGap = 12;
        constexpr int kCardY0 = 66;
        const std::string count_chip = std::to_string(word.total_count) + " 词";
        auto draw_card = [&word, &count_chip](int y0, const WqnBitmapAsset& icon, const std::string& title, const std::string& subtitle,
                                              const std::string& chip, bool selected) -> esp_err_t {
            DrawRoundedRect(kCardX, y0, kCardW, kCardH, 6);
            if (selected) {
                DrawRoundedRect(kCardX + 2, y0 + 2, kCardW - 4, kCardH - 4, 4);
            }
            DrawWqnBitmapAsset(kCardX + 16, y0 + 18, icon, true);
            ESP_RETURN_ON_ERROR(DrawClippedText(kCardX + 48, y0 + 10, 220, title), kTag, "draw word card title");
            ESP_RETURN_ON_ERROR(DrawClippedText(kCardX + 48, y0 + 34, 220, subtitle), kTag, "draw word card subtitle");
            // Right status chip: rounded 88x26 box + centered label.
            const std::string chip_text = wqn::TruncateUtf8TextToWidth(chip, 80);
            DrawRoundedRect(kCardX + 278, y0 + 17, 88, 26, 6);
            ESP_RETURN_ON_ERROR(DrawCenteredText(kCardX + 278, y0 + 22, 88, chip_text), kTag, "draw word card chip");
            return ESP_OK;
        };
        const bool ready = word.pack_ready;
        ESP_RETURN_ON_ERROR(
            draw_card(kCardY0,
                      w01_word_review_sequential_24_asset,
                      "顺序",
                      ready ? (word.sequential_session_resumable
                                   ? "可继续上次会话"
                                   : "按词库顺序浏览")
                            : "需同步词库",
                      ready ? count_chip : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kSequential),
            kTag,
            "draw sequential card");
        ESP_RETURN_ON_ERROR(
            draw_card(kCardY0 + (kCardH + kCardGap),
                      w02_word_review_random_24_asset,
                      "随机",
                      ready ? (word.random_session_resumable
                                   ? "可继续上次会话"
                                   : "随机浏览词库")
                            : "需同步词库",
                      ready ? count_chip : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kRandom),
            kTag,
            "draw random card");
        ESP_RETURN_ON_ERROR(
            draw_card(kCardY0 + 2 * (kCardH + kCardGap),
                      w03_word_dictionary_24_asset,
                      "词典",
                      ready ? "按字母查词" : "在线同步后使用",
                      ready ? "A-Z" : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kDictionary),
            kTag,
            "draw dictionary card");
        ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
        if (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig) {
            return RefreshStableRegion({0, 64, wqn::kEpdWidth, 220, "word-home"}, schedule);
        }
        return RefreshFrame(frame, schedule);
    }

    if (word.mode == wqn::WordAppMode::kDictionaryPicker) {
        if (word.dictionary_stage ==
            wqn::WordDictionaryStage::kLookupChoice) {
            ESP_RETURN_ON_ERROR(
                DrawCenteredText(20, 76, 360, word.dictionary_prefix),
                kTag,
                "draw lookup query");
            ESP_RETURN_ON_ERROR(
                draw_choice(128, "在线搜索", "查 WQN 服务器",
                            word.lookup_selection ==
                                wqn::WordLookupSelection::kOnlineSearch),
                kTag,
                "draw online lookup choice");
            ESP_RETURN_ON_ERROR(
                draw_choice(182, "询问 AI", "跳转到 AI",
                            word.lookup_selection ==
                                wqn::WordLookupSelection::kAiLookup),
                kTag,
                "draw ai lookup choice");
            ESP_RETURN_ON_ERROR(
                DrawClippedText(12, 278, 376, word.hint),
                kTag,
                "draw word hint");
            if (schedule == RefreshSchedule::kSelection ||
                schedule == RefreshSchedule::kConfig) {
                return RefreshRegion(
                    {0, 64, wqn::kEpdWidth, 236, "word-dictionary-picker"},
                    schedule);
            }
            return RefreshFrame(frame, schedule);
        }
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
        if (schedule == RefreshSchedule::kSelection ||
            schedule == RefreshSchedule::kConfig) {
            return RefreshRegion(
                {0, 64, wqn::kEpdWidth, 236, "word-dictionary-picker"},
                schedule);
        }
        return RefreshFrame(frame, schedule);
    }

    if (!word.has_card) {
        const std::string empty_title = word.mode == wqn::WordAppMode::kSessionStarting
            ? "正在准备"
            : "词库未同步";
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 118, 360, empty_title), kTag, "draw word empty title");
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

    if (word.card_phase == wqn::WordCardPhase::kFront) {
        DrawRect(74, 164, 252, 48);
        ESP_RETURN_ON_ERROR(DrawCenteredText(74, 181, 252, "先回忆释义"), kTag, "draw recall prompt");
    } else {
        ESP_RETURN_ON_ERROR(draw_word_back(), kTag, "draw word back");
    }

    if (word.card_phase == wqn::WordCardPhase::kPersisting) {
        DrawRoundedRect(122, 250, 156, 24, 5);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(122, 256, 156, "正在保存"),
            kTag,
            "draw word persisting");
    }

    ESP_RETURN_ON_ERROR(DrawClippedText(12, 278, 376, word.hint), kTag, "draw word hint");
    if (schedule == RefreshSchedule::kSelection ||
        schedule == RefreshSchedule::kConfig) {
        return RefreshRegion(
            {0, 64, wqn::kEpdWidth, 236, "word-card"},
            schedule);
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
