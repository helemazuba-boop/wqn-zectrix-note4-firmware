// Word page rendering: home (智能复习 / 随机 / 遗忘的单词), the review completion
// page and one shared card surface. Extracted from device_ui.cpp.

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
constexpr int kWordActionDividerY = 274;
constexpr int kWordActionTextY = 280;

// Word home feature cards (the rounded-card language).
constexpr int kWordCardH = 60;
constexpr int kWordCardGap = 12;
constexpr int kWordCardY0 = 66;
constexpr int kStatusChipWidth = 88;
constexpr int kStatusChipHeight = 26;
constexpr int kStatusChipRadius = 6;
constexpr int kStatusChipOffsetX = 278;     // chip origin offset from the card x

// Completion page rows (kInvert focus -- compact operable items).
constexpr int kWordChoiceX = 38;
constexpr int kWordChoiceW = 324;
constexpr int kWordChoiceH = 42;

// Content (non-focus) display frames -- plain outlined containers drawn with
// DrawRect outlines directly (the decoration layer owns FOCUS only).
constexpr int kWordBackX = 22;
constexpr int kWordBackW = 356;
constexpr int kWordBackH = 116;
constexpr int kWordBackTextX = 34;
constexpr int kWordBackTextW = 332;
constexpr int kWordRecallX = 74;
constexpr int kWordRecallW = 252;
constexpr int kWordRecallH = 48;

std::string WordActionHint(const wqn::WordAppSnapshot& word)
{
    if (word.commit_state == wqn::WordObservationCommitState::kFailed) {
        return "保存失败 · 确认重试";
    }
    if (word.card_phase == wqn::WordCardPhase::kPersisting) {
        return "正在保存，请稍候";
    }
    switch (word.mode) {
        case wqn::WordAppMode::kHome:
            return "上下选择 · 确认进入 · 长按确认返回";
        case wqn::WordAppMode::kSessionStarting:
            return "长按确认取消";
        case wqn::WordAppMode::kReviewComplete:
            return "上下选择 · 确认执行 · 长按确认返回";
        case wqn::WordAppMode::kWordCard:
            if (word.card_phase == wqn::WordCardPhase::kFront) {
                return "确认看释义 · 下键跳过 · 长按确认暂停";
            }
            return "上键不认识 · 确认认识 · 下键跳过";
    }
    return "";
}

esp_err_t DrawWordActionFooter(const wqn::WordAppSnapshot& word)
{
    DrawHorizontalLine(kMarginX, kWordActionDividerY, kContentWidth);
    return DrawCenteredText(
        kMarginX, kWordActionTextY, kContentWidth, WordActionHint(word));
}

void DrawWordHeadline(const std::string& text)
{
    const lv_font_t* font = wqn::PrimaryUiFont();
    const int base_width = wqn::MeasureTextWithFont(font, text.c_str());
    if (base_width <= 0 || base_width > 360) {
        // Very long or unsupported entries retain the compact fallback so the
        // headword never runs outside the card surface.
        (void)DrawCenteredText(20, 80, 360, text);
        return;
    }
    const uint8_t scale = base_width * 2 <= 360 ? 2 : 1;
    const int y = scale == 2 ? 70 : 80;
    wqn::DrawTextWithFontScaledCentered(
        20, y, 360, font, text.c_str(), scale, true);
}

esp_err_t RenderWordToEpd(const wqn::UiFrame& frame, RefreshSchedule schedule)
{
    const wqn::WordAppSnapshot& word = frame.word_app;
    wqn::ClearEpdFramebuffer(true);
    DrawStatusBar("单词", frame.home);

    ESP_RETURN_ON_ERROR(DrawClippedText(kMarginX, kWordHeaderLineY, 250, word.status_line), kTag, "draw word status");
    const std::string header_progress = word.mode == wqn::WordAppMode::kHome
        ? "今日 " + std::to_string(word.reviewed_today) + "/" +
            std::to_string(word.correct_today)
        : word.progress_line;
    ESP_RETURN_ON_ERROR(DrawClippedText(kWordProgressX, kWordHeaderLineY, kWordProgressWidth, header_progress), kTag, "draw word progress");
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
        // [word-due-hint] The review card advertises today's due queue when the
        // last sync reported one; the shuffle card keeps the pack size.
        const std::string review_chip = word.review_due_count > 0
            ? std::to_string(word.review_due_count) + " 到期"
            : count_chip;
        // [word-mistake-hint] The mistakes card advertises the pool the last
        // sync reported (0 is a real "empty pool"); -1 means no hint yet and
        // falls back to the pack size like the review card.
        const std::string mistake_chip = word.mistake_count >= 0
            ? std::to_string(word.mistake_count) + " 词"
            : count_chip;
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
                      "智能复习",
                      ready ? (word.review_session_resumable
                                   ? "可继续上次会话"
                                   : "按到期顺序复习")
                            : "需同步词库",
                      ready ? review_chip : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kReview),
            kTag,
            "draw review card");
        ESP_RETURN_ON_ERROR(
            draw_card(kWordCardY0 + (kWordCardH + kWordCardGap),
                      w02_word_review_random_24_asset,
                      "随机",
                      ready ? (word.shuffle_session_resumable
                                   ? "可继续上次会话"
                                   : "完全随机浏览")
                            : "需同步词库",
                      ready ? count_chip : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kShuffle),
            kTag,
            "draw shuffle card");
        // The mistakes card reuses the retired dictionary glyph (a word-book):
        // no dedicated 24px asset exists yet.
        ESP_RETURN_ON_ERROR(
            draw_card(kWordCardY0 + 2 * (kWordCardH + kWordCardGap),
                      w03_word_dictionary_24_asset,
                      "遗忘的单词",
                      ready ? (word.mistakes_session_resumable
                                   ? "可继续上次会话"
                                   : "复习答错的单词")
                            : "需同步词库",
                      ready ? mistake_chip : "未同步",
                      word.home_selection == wqn::WordHomeSelection::kMistakes),
            kTag,
            "draw mistakes card");
        ESP_RETURN_ON_ERROR(DrawWordActionFooter(word), kTag, "draw word home actions");
        if (schedule == RefreshSchedule::kSelection || schedule == RefreshSchedule::kConfig) {
            return RefreshStableRegion({0, 64, wqn::kEpdWidth, 220, "word-home"}, schedule);
        }
        return RefreshFrame(frame, schedule);
    }

    if (word.mode == wqn::WordAppMode::kReviewComplete) {
        // [word-modes-v2] Local completion page of the review entry. The
        // session is over (queue and replay pool are empty), so this screen is
        // decided entirely on device and works offline.
        const std::string title = word.review_complete_empty
            ? "今天没有到期的单词"
            : "今天的复习完成了";
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 74, 360, title), kTag, "draw review complete title");
        const std::string totals =
            "复习 " + std::to_string(word.review_complete_reviewed) + " 张 · 重学 " +
            std::to_string(word.review_complete_replayed) + " 张";
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 102, 360, totals), kTag, "draw review complete totals");
        if (word.review_complete_unknown > 0) {
            const std::string missed = std::to_string(word.review_complete_unknown) +
                " 张没答对，下次优先复习";
            ESP_RETURN_ON_ERROR(DrawCenteredText(20, 128, 360, missed), kTag, "draw review complete missed");
        }
        const std::string cursor_chip =
            word.sequential_total == 0
            ? std::string()
            : (word.sequential_cursor >= word.sequential_total
                   ? "从头开始"
                   : "#" + std::to_string(word.sequential_cursor) + " / " +
                         std::to_string(word.sequential_total));
        ESP_RETURN_ON_ERROR(
            draw_choice(172, "顺序过词库", cursor_chip,
                        word.complete_selection ==
                            wqn::WordCompleteSelection::kSequential),
            kTag,
            "draw sequential action");
        ESP_RETURN_ON_ERROR(
            draw_choice(226, "返回", "",
                        word.complete_selection ==
                            wqn::WordCompleteSelection::kReturn),
            kTag,
            "draw return action");
        ESP_RETURN_ON_ERROR(DrawWordActionFooter(word), kTag, "draw review complete actions");
        if (schedule == RefreshSchedule::kSelection ||
            schedule == RefreshSchedule::kConfig) {
            return RefreshRegion(
                {0, 64, wqn::kEpdWidth, 236, "word-review-complete"}, schedule);
        }
        return RefreshFrame(frame, schedule);
    }

    if (!word.has_card) {
        const std::string empty_title = word.mode == wqn::WordAppMode::kSessionStarting
            ? "正在准备"
            : "词库未同步";
        // [v2] Unified empty-state surface (was bare centered text).
        ESP_RETURN_ON_ERROR(DrawEmptyState(empty_title, word.hint), kTag, "draw word empty state");
        ESP_RETURN_ON_ERROR(DrawWordActionFooter(word), kTag, "draw word empty actions");
        return RefreshFrame(frame, schedule);
    }

    DrawWordHeadline(word.word);
    if (!word.phonetic.empty()) {
        ESP_RETURN_ON_ERROR(DrawCenteredText(20, 110, 360, word.phonetic), kTag, "draw word phonetic");
    }

    if (word.card_phase == wqn::WordCardPhase::kFront) {
        DrawRoundedRect(kWordRecallX, 164, kWordRecallW, kWordRecallH, kRoundedOuterRadius);  // content container outline
        ESP_RETURN_ON_ERROR(DrawCenteredText(kWordRecallX, 181, kWordRecallW, "先回忆释义"), kTag, "draw recall prompt");
    } else {
        ESP_RETURN_ON_ERROR(draw_word_back(), kTag, "draw word back");
    }

    ESP_RETURN_ON_ERROR(DrawWordActionFooter(word), kTag, "draw word card actions");

    if (schedule == RefreshSchedule::kSelection ||
        schedule == RefreshSchedule::kConfig) {
        return RefreshRegion(
            {0, 64, wqn::kEpdWidth, 236, "word-card"},
            schedule);
    }
    return RefreshFrame(frame, schedule);
}

}  // namespace device_ui_internal
