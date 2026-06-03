#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace wqn {

enum class WordReviewStatus : uint8_t {
    kNew = 0,
    kLearning,
    kReview,
    kMastered,
};

enum class WordInput {
    kUp,
    kDown,
    kConfirm,
    kLongConfirm,
};

struct WordCard {
    uint16_t id = 0;
    const char* word = nullptr;
    const char* phonetic = nullptr;
    const char* meaning = nullptr;
    const char* example = nullptr;
    const char* example_translation = nullptr;
};

struct WordProgressState {
    WordReviewStatus status = WordReviewStatus::kNew;
    int32_t due_day = 0;
    int32_t last_review_day = 0;
    uint16_t interval_days = 0;
    uint8_t correct_streak = 0;
    uint16_t lapses = 0;
};

struct WordAppState {
    bool initialized = false;
    bool showing_back = false;
    int32_t today = 0;
    uint16_t daily_target = 20;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t streak_days = 0;
    size_t queue_position = 0;
    std::vector<WordProgressState> progress;
    std::vector<size_t> queue;
    std::string message;
};

struct WordAppSnapshot {
    bool has_card = false;
    bool showing_back = false;
    bool finished_today = false;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t daily_target = 20;
    uint16_t due_count = 0;
    uint16_t mastered_count = 0;
    uint16_t total_count = 0;
    uint16_t card_position = 0;
    uint16_t card_count = 0;
    std::string word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string progress_line;
    std::string status_line;
    std::string hint;
};

esp_err_t InitWordApp(WordAppState* state);
esp_err_t HandleWordAppInput(WordAppState* state, WordInput input);
WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state);
std::string WordAppProgressLabel(const WordAppState& state);
std::string WordAppStatusLine(const WordAppState& state);
std::string WordAppSignature(const WordAppState& state);

}  // namespace wqn
