// Word page rendering: home, dictionary picker and one shared card surface.
// Extracted from device_ui.cpp.

#include "ui_internal.h"
#include "ui_widgets.h"

#include <string>

#include "display_service.h"
#include "esp_log.h"
#include "ui/assets/font_wqn_card_24_1.h"
#include "word_app.h"

namespace device_ui_internal {

constexpr char kTag[] = "wqn_ui";

// ---- Word page internal geometry (page-local, derives from ui_layout tokens) ----
// Page header summary band: status (left) + progress (right) under the system
// status bar (y=kStatusBarDividerY), with a divider a couple of px below the
// status line.
constexpr int kWordHeaderLineY = 36;
constexpr int kWordHeaderDividerY = 58;
constexpr int kWordProgressX = 282;          // right-aligned progress, width 108 to kEpdWidth-kMarginX
constexpr int kWordProgressWidth = (wqn::kEpdWidth - kMarginX) - kWordProgressX;

// Word home feature cards (the rounded-card language).
constexpr int kWordCardH = 60;
constexpr int kWordCardGap = 12;
constexpr int kWordCardY0 = 66;
constexpr int kStatusChipWidth = 88;
constexpr int kStatusChipHeight = 26;
constexpr int kStatusChipRadius = 6;
constexpr int kStatusChipOffsetX = 278;     // chip origin offset from the card x

// Dictionary lookup choice rows (kInvert focus -- compact operable items).
constexpr int kWordChoiceX = 38;
constexpr int kWordChoiceW = 324;
constexpr int kWordChoiceH = 42;

// Dictionary letter grid.
constexpr int kLetterStartX = 28;
constexpr int kLetterStartY = 108;
constexpr int kLetterCellW = 42;
constexpr int kLetterCellH = 30;

// Content (non-focus) display frames -- plain outlined containers drawn with
// DrawSelectionDecoration(kNone)? No: kNone draws nothing. Content containers
// use a plain DrawRect outline via the kInnerBorder path WITHOUT the focus
// meaning, but to keep the decoration layer focused on FOCUS only (design
// decision: containers keep DrawRect), we draw container outlines directly.
constexpr int kWordBackX = 22;
constexpr int kWordBackW = 356;
constexpr int kWordBackH = 116;
constexpr int kWordBackTextX = 34;
constexpr int kWordBackTextW = 332;
constexpr int kWordRecallX = 74;
constexpr int kWordRecallW = 252;
constexpr int kWordRecallH = 48;

// Persisting toast pill (rounded, content-only -- not a focus decoration).
constexpr int kPersistPillX = 122;
constexpr int kPersistPillW = 156;
constexpr int kPersistPillH = 24;
constexpr int kPersistPillR = 5;

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::WordAppSnapshot& word = frame.word_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar("单词", frame.home);

    ESP_RETURN_ON_ERROR(DrawClippedText(kMarginX, kWordHeaderLineY, 250, word.status_line), kTag, "draw word status");
    ESP_RETURN_ON_ERROR(DrawClippedText(kWordProgressX, kWordHeaderLineY, kWordProgressWidth, word.progress_line), kTag, "draw word progress");
    DrawHorizontalLine(kMarginX, kWordHeaderDividerY, kContentWidth);

    auto draw_choice = [](int y, const std::string& title, const std::string& subtitle, bool selected) -> esp_err_t {
        DrawSelectionDecoration(kWordChoiceX, y, kWordChoiceW, kWordChoiceH,
                                selected ? SelectionStyle::kInvert : SelectionStyle::kNone);
        const bool black_text = !selected;
        ESP_RETURN_ON_ERROR(DrawClippedText(kWordChoiceX + 12, y + 7, 180, title, black_text), kTag, "draw word choice title");
        ESP_RETURN_ON_ERROR(DrawClippedText(kWordChoiceX + 160, y + 7, 150, subtitle, black_text), kTag, "draw word choice subtitle");
        return ESP_OK;
    };

    auto draw_word_back = [&word]() -> esp_err_t {
        DrawRoundedRect(kWordBackX, 128, kWordBackW, kWordBackH, kRoundedOuterRadius);  // content container outline
        const std::string title = word.part_of_speech.empty() ? word.meaning : word.part_of_speech + "  " + word.meaning;
        ESP_RETURN_ON_ERROR(DrawWrappedText(kWordBackTextX, 140, kWordBackTextW, title, 2), kTag, "draw word meaning");
        if (!word.example.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(kWordBackTextX, 184, kWordBackTextW, word.example, 2), kTag, "draw word example");
        }
        if (!word.example_translation.empty()) {
            ESP_RETURN_ON_ERROR(DrawWrappedText(kWordBackTextX, 224, kWordBackTextW, word.example_translation, 1), kTag, "draw word translation");
        }
        return ESP_OK;
    };

    if (word.mode == wqn::WordAppMode::kHome) {
        // [word-home-cards] Three rounded feature cards. The card body outline
        // (rounded r6) is always drawn; when selected, a 2px-inset concentric
        // rounded double-line is added as the kRoundedInnerBorder FOCUS
        // decoration. Left = 24px 1bpp asset, center = title + dynamic
        // subtitle, right = a rounded status chip (DrawStatusChip, non-focus).
        constexpr int kCardX = kMarginX;
        constexpr int kCardW = kContentWidth;
        const std::string count_chip = std::to_string(word.total_count) + " 词";
        auto draw_card = [&word, &count_chip](int y0, const WqnBitmapAsset& icon, const std::string& title, const std::string& subtitle,
                                               const std::string& chip, bool selected) -> esp_err_t {
            // Card outline: drawn by the focus decoration when selected (it
            // draws outline + 2px-inset concentric inner), or as a plain
            // rounded outline when unselected. One path owns the outline.
            if (selected) {
                DrawSelectionDecoration(kCardX, y0, kCardW, kWordCardH, SelectionStyle::kRoundedInnerBorder);
            } else {
                DrawRoundedRect(kCardX, y0, kCardW, kWordCardH, kRoundedOuterRadius);
            }
            DrawWqnBitmapAsset(kCardX + 16, y0 + 18, icon, true);
            ESP_RETURN_ON_ERROR(DrawClippedText(kCardX + 48, y0 + 10, 220, title), kTag, "draw word card title");
            ESP_RETURN_ON_ERROR(DrawClippedText(kCardX + 48, y0 + 34, 220, subtitle), kTag, "draw word card subtitle");
            // Right status chip (non-selectable badge).
            const std::string chip_text = wqn::TruncateUtf8TextToWidth(chip, 80);
            DrawStatusChip(kCardX + kStatusChipOffsetX, y0 + 17, kStatusChipWidth, kStatusChipHeight, kStatusChipRadius, chip_text);
            return ESP_OK;
        };
        const bool ready = word.pack_ready;
        ESP_RETURN_ON_ERROR(
            draw_card(kWordCardY0,
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
            draw_card(kWordCardY0 + (kWordCardH + kWordCardGap),
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
            draw_card(kWordCardY0 + 2 * (kWordCardH + kWordCardGap),
                      w03_word_dictionary_24_asset,
                      "词典",
                      ready ? "按字母查词" : "在线同步后使用",
                      ready ? "A-Z" : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kDictionary),
            kTag,
            "draw dictionary card");
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
        for (size_t i = 0; i < word.dictionary_letters.size() && i < 24; ++i) {
            const int col = static_cast<int>(i % 8);
            const int row = static_cast<int>(i / 8);
            const int x = kLetterStartX + col * kLetterCellW;
            const int y = kLetterStartY + row * kLetterCellH;
            const bool letter_selected = i == word.dictionary_letter_selected;
            // Letter cell: small compact operable unit -> kInvert focus
            // (ink-filled cell, paper glyph). Unselected cells have no frame.
            char letter[2] = {word.dictionary_letters[i], 0};
            if (letter_selected) {
                DrawSelectionDecoration(x, y, kLetterCellW - 8, kLetterCellH - 2, SelectionStyle::kInvert);
                ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 7, kLetterCellW - 8, letter, false), kTag, "draw dictionary letter");
            } else {
                ESP_RETURN_ON_ERROR(DrawCenteredText(x, y + 7, kLetterCellW - 8, letter), kTag, "draw dictionary letter");
            }
        }
        int y = 212;
        for (size_t i = 0; i < word.dictionary_preview_words.size() && i < 3; ++i) {
            const std::string marker = i == word.dictionary_match_selected ? "> " : "  ";
            ESP_RETURN_ON_ERROR(DrawClippedText(42, y, 300, marker + word.dictionary_preview_words[i]), kTag, "draw dictionary preview");
            y += 22;
        }
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
        DrawRoundedRect(kWordRecallX, 164, kWordRecallW, kWordRecallH, kRoundedOuterRadius);  // content container outline
        ESP_RETURN_ON_ERROR(DrawCenteredText(kWordRecallX, 181, kWordRecallW, "先回忆释义"), kTag, "draw recall prompt");
    } else {
        ESP_RETURN_ON_ERROR(draw_word_back(), kTag, "draw word back");
    }

    if (word.card_phase == wqn::WordCardPhase::kPersisting) {
        DrawRoundedRect(kPersistPillX, 250, kPersistPillW, kPersistPillH, kPersistPillR);
        ESP_RETURN_ON_ERROR(
            DrawCenteredText(kPersistPillX, 256, kPersistPillW, "正在保存"),
            kTag,
            "draw word persisting");
    }

    if (schedule == RefreshSchedule::kSelection ||
        schedule == RefreshSchedule::kConfig) {
        return RefreshRegion(
            {0, 64, wqn::kEpdWidth, 236, "word-card"},
            schedule);
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
