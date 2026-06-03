#include "word_app.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

namespace {

constexpr char kTag[] = "wqn_word";
constexpr char kNvsNamespace[] = "vocab";
constexpr char kProgressKey[] = "progress";
constexpr uint32_t kProgressMagic = 0x31565157;  // WQV1
constexpr uint16_t kProgressVersion = 1;
constexpr uint16_t kDefaultDailyTarget = 20;
constexpr int32_t kSecondsPerDay = 86400;

const wqn::WordCard kWordDeck[] = {
    {1, "abandon", "/əˈbændən/", "放弃；抛弃", "Do not abandon the plan too early.", "不要过早放弃这个计划。"},
    {2, "accurate", "/ˈækjərət/", "准确的；精确的", "The data must be accurate.", "数据必须准确。"},
    {3, "analysis", "/əˈnæləsɪs/", "分析", "The analysis explains the mistake.", "这个分析解释了错误。"},
    {4, "approach", "/əˈproʊtʃ/", "方法；接近", "Try a different approach.", "换一种方法试试。"},
    {5, "assume", "/əˈsuːm/", "假设；认为", "We assume the answer is correct.", "我们假设答案是正确的。"},
    {6, "benefit", "/ˈbenɪfɪt/", "益处；受益", "Review brings long-term benefit.", "复习会带来长期收益。"},
    {7, "concept", "/ˈkɑːnsept/", "概念", "The concept is simple but important.", "这个概念简单但重要。"},
    {8, "confirm", "/kənˈfɜːrm/", "确认", "Please confirm your choice.", "请确认你的选择。"},
    {9, "consistent", "/kənˈsɪstənt/", "一致的；稳定的", "Keep a consistent study habit.", "保持稳定的学习习惯。"},
    {10, "context", "/ˈkɑːntekst/", "上下文；背景", "Context helps you remember words.", "上下文能帮助你记单词。"},
    {11, "derive", "/dɪˈraɪv/", "推导；获得", "Derive the formula step by step.", "一步步推导公式。"},
    {12, "efficient", "/ɪˈfɪʃnt/", "高效的", "A small review queue is efficient.", "小复习队列更高效。"},
    {13, "estimate", "/ˈestɪmeɪt/", "估计；估算", "Estimate the answer first.", "先估算答案。"},
    {14, "evidence", "/ˈevɪdəns/", "证据", "Use evidence before guessing.", "先看证据，不要猜。"},
    {15, "factor", "/ˈfæktər/", "因素；因子", "Time is an important factor.", "时间是重要因素。"},
    {16, "function", "/ˈfʌŋkʃn/", "函数；功能", "This function updates progress.", "这个函数更新进度。"},
    {17, "improve", "/ɪmˈpruːv/", "改进；提高", "Daily practice improves memory.", "每日练习提高记忆。"},
    {18, "interval", "/ˈɪntərvl/", "间隔", "The review interval gets longer.", "复习间隔会变长。"},
    {19, "method", "/ˈmeθəd/", "方法", "Choose the simplest method.", "选择最简单的方法。"},
    {20, "specific", "/spəˈsɪfɪk/", "具体的；特定的", "Give a specific example.", "给出一个具体例子。"},
    {21, "strategy", "/ˈstrætədʒi/", "策略", "A review strategy reduces forgetting.", "复习策略能减少遗忘。"},
    {22, "structure", "/ˈstrʌktʃər/", "结构", "The sentence structure is clear.", "句子结构很清楚。"},
    {23, "sufficient", "/səˈfɪʃnt/", "足够的", "Give sufficient time to review.", "留出足够时间复习。"},
    {24, "verify", "/ˈverɪfaɪ/", "验证", "Verify the result after solving.", "解完后验证结果。"},
};

constexpr size_t kWordDeckSize = sizeof(kWordDeck) / sizeof(kWordDeck[0]);

struct ProgressHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    int32_t stats_day;
    uint16_t reviewed_today;
    uint16_t correct_today;
    uint16_t streak_days;
    uint16_t daily_target;
};

struct PackedWordProgressState {
    int32_t due_day;
    int32_t last_review_day;
    uint16_t interval_days;
    uint8_t status;
    uint8_t correct_streak;
    uint16_t lapses;
    uint16_t reserved;
};

int32_t CurrentDay()
{
    std::time_t now = 0;
    std::time(&now);
    if (now <= 0) {
        return 0;
    }
    return static_cast<int32_t>(now / kSecondsPerDay);
}

wqn::WordReviewStatus ClampStatus(uint8_t raw)
{
    switch (static_cast<wqn::WordReviewStatus>(raw)) {
        case wqn::WordReviewStatus::kNew:
        case wqn::WordReviewStatus::kLearning:
        case wqn::WordReviewStatus::kReview:
        case wqn::WordReviewStatus::kMastered:
            return static_cast<wqn::WordReviewStatus>(raw);
    }
    return wqn::WordReviewStatus::kNew;
}

uint16_t ClampDailyTarget(uint16_t target)
{
    return static_cast<uint16_t>(std::min<uint16_t>(200, std::max<uint16_t>(1, target)));
}

uint16_t ClampInterval(uint32_t days)
{
    return static_cast<uint16_t>(std::min<uint32_t>(days, 180));
}

bool IsDue(const wqn::WordProgressState& state, int32_t today)
{
    if (state.status == wqn::WordReviewStatus::kNew) {
        return true;
    }
    return state.due_day <= today;
}

const wqn::WordCard* CurrentCard(const wqn::WordAppState& state)
{
    if (state.queue_position >= state.queue.size()) {
        return nullptr;
    }
    const size_t deck_index = state.queue[state.queue_position];
    if (deck_index >= kWordDeckSize) {
        return nullptr;
    }
    return &kWordDeck[deck_index];
}

void ResetDailyStatsIfNeeded(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    const int32_t today = CurrentDay();
    if (state->today == today) {
        return;
    }
    if (state->today != 0 && state->reviewed_today > 0) {
        state->streak_days = static_cast<uint16_t>(std::min<int>(UINT16_MAX, state->streak_days + 1));
    }
    state->today = today;
    state->reviewed_today = 0;
    state->correct_today = 0;
}

void BuildQueue(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return;
    }
    state->queue.clear();
    state->queue_position = 0;
    if (state->reviewed_today >= state->daily_target) {
        return;
    }

    const size_t remaining = state->daily_target - state->reviewed_today;
    for (size_t i = 0; i < state->progress.size() && state->queue.size() < remaining; ++i) {
        const wqn::WordProgressState& progress = state->progress[i];
        if (progress.status != wqn::WordReviewStatus::kNew && progress.due_day <= state->today) {
            state->queue.push_back(i);
        }
    }
    for (size_t i = 0; i < state->progress.size() && state->queue.size() < remaining; ++i) {
        if (state->progress[i].status == wqn::WordReviewStatus::kNew) {
            state->queue.push_back(i);
        }
    }
}

esp_err_t LoadProgress(wqn::WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    state->progress.assign(kWordDeckSize, wqn::WordProgressState{});
    state->daily_target = kDefaultDailyTarget;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t blob_size = 0;
    ret = nvs_get_blob(handle, kProgressKey, nullptr, &blob_size);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret != ESP_OK || blob_size < sizeof(ProgressHeader)) {
        nvs_close(handle);
        return ret == ESP_OK ? ESP_OK : ret;
    }

    std::vector<uint8_t> blob(blob_size);
    ret = nvs_get_blob(handle, kProgressKey, blob.data(), &blob_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ProgressHeader header = {};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != kProgressMagic || header.version != kProgressVersion) {
        ESP_LOGW(kTag, "ignore incompatible word progress blob");
        return ESP_OK;
    }

    state->today = header.stats_day;
    state->reviewed_today = header.reviewed_today;
    state->correct_today = header.correct_today;
    state->streak_days = header.streak_days;
    state->daily_target = ClampDailyTarget(header.daily_target);

    const size_t available_count = (blob.size() - sizeof(ProgressHeader)) / sizeof(PackedWordProgressState);
    const size_t copy_count = std::min({kWordDeckSize, static_cast<size_t>(header.count), available_count});
    for (size_t i = 0; i < copy_count; ++i) {
        PackedWordProgressState packed = {};
        std::memcpy(&packed, blob.data() + sizeof(ProgressHeader) + i * sizeof(PackedWordProgressState), sizeof(packed));
        wqn::WordProgressState progress;
        progress.status = ClampStatus(packed.status);
        progress.due_day = packed.due_day;
        progress.last_review_day = packed.last_review_day;
        progress.interval_days = packed.interval_days;
        progress.correct_streak = packed.correct_streak;
        progress.lapses = packed.lapses;
        state->progress[i] = progress;
    }
    return ESP_OK;
}

esp_err_t SaveProgress(const wqn::WordAppState& state)
{
    ProgressHeader header = {};
    header.magic = kProgressMagic;
    header.version = kProgressVersion;
    header.count = static_cast<uint16_t>(std::min<size_t>(state.progress.size(), UINT16_MAX));
    header.stats_day = state.today;
    header.reviewed_today = state.reviewed_today;
    header.correct_today = state.correct_today;
    header.streak_days = state.streak_days;
    header.daily_target = state.daily_target;

    const size_t packed_count = header.count;
    std::vector<uint8_t> blob(sizeof(ProgressHeader) + packed_count * sizeof(PackedWordProgressState));
    std::memcpy(blob.data(), &header, sizeof(header));
    for (size_t i = 0; i < packed_count; ++i) {
        PackedWordProgressState packed = {};
        packed.due_day = state.progress[i].due_day;
        packed.last_review_day = state.progress[i].last_review_day;
        packed.interval_days = state.progress[i].interval_days;
        packed.status = static_cast<uint8_t>(state.progress[i].status);
        packed.correct_streak = state.progress[i].correct_streak;
        packed.lapses = state.progress[i].lapses;
        std::memcpy(blob.data() + sizeof(ProgressHeader) + i * sizeof(PackedWordProgressState), &packed, sizeof(packed));
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(handle, kProgressKey, blob.data(), blob.size());
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

void ApplyAnswer(wqn::WordProgressState* progress, bool known, int32_t today)
{
    if (progress == nullptr) {
        return;
    }
    progress->last_review_day = today;
    if (!known) {
        progress->status = wqn::WordReviewStatus::kLearning;
        progress->correct_streak = 0;
        progress->lapses = ClampInterval(static_cast<uint32_t>(progress->lapses) + 1);
        progress->interval_days = 0;
        progress->due_day = today;
        return;
    }

    progress->correct_streak =
        static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(progress->correct_streak) + 1, UINT8_MAX));
    switch (progress->status) {
        case wqn::WordReviewStatus::kNew:
            progress->status = wqn::WordReviewStatus::kLearning;
            progress->interval_days = 1;
            break;
        case wqn::WordReviewStatus::kLearning:
            progress->status = progress->correct_streak >= 2 ? wqn::WordReviewStatus::kReview : wqn::WordReviewStatus::kLearning;
            progress->interval_days = progress->correct_streak >= 2 ? 3 : 1;
            break;
        case wqn::WordReviewStatus::kReview:
            progress->interval_days = ClampInterval(std::max<uint32_t>(3, progress->interval_days * 2U));
            if (progress->correct_streak >= 5 && progress->interval_days >= 30) {
                progress->status = wqn::WordReviewStatus::kMastered;
            }
            break;
        case wqn::WordReviewStatus::kMastered:
            progress->interval_days = 90;
            break;
    }
    progress->due_day = today + progress->interval_days;
}

uint16_t CountDue(const wqn::WordAppState& state)
{
    uint32_t due = 0;
    for (const wqn::WordProgressState& progress : state.progress) {
        if (IsDue(progress, state.today)) {
            ++due;
        }
    }
    return static_cast<uint16_t>(std::min<uint32_t>(due, UINT16_MAX));
}

uint16_t CountMastered(const wqn::WordAppState& state)
{
    uint32_t mastered = 0;
    for (const wqn::WordProgressState& progress : state.progress) {
        if (progress.status == wqn::WordReviewStatus::kMastered) {
            ++mastered;
        }
    }
    return static_cast<uint16_t>(std::min<uint32_t>(mastered, UINT16_MAX));
}

std::string StatusText(wqn::WordReviewStatus status)
{
    switch (status) {
        case wqn::WordReviewStatus::kNew:
            return "新词";
        case wqn::WordReviewStatus::kLearning:
            return "学习中";
        case wqn::WordReviewStatus::kReview:
            return "复习";
        case wqn::WordReviewStatus::kMastered:
            return "已掌握";
    }
    return "新词";
}

}  // namespace

namespace wqn {

esp_err_t InitWordApp(WordAppState* state)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = LoadProgress(state);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "load progress failed: %s", esp_err_to_name(ret));
    }
    ResetDailyStatsIfNeeded(state);
    BuildQueue(state);
    state->initialized = true;
    state->showing_back = false;
    if (state->queue.empty()) {
        state->message = "今日单词已完成";
    }
    return ret == ESP_OK ? ESP_OK : ret;
}

esp_err_t HandleWordAppInput(WordAppState* state, WordInput input)
{
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!state->initialized) {
        ESP_RETURN_ON_ERROR(InitWordApp(state), kTag, "init word app");
    }
    ResetDailyStatsIfNeeded(state);
    if (state->queue.empty()) {
        BuildQueue(state);
        if (state->queue.empty()) {
            state->message = "今日单词已完成";
            return ESP_OK;
        }
    }

    const size_t deck_index = state->queue[state->queue_position];
    if (deck_index >= state->progress.size()) {
        BuildQueue(state);
        return ESP_OK;
    }

    switch (input) {
        case WordInput::kConfirm:
            if (!state->showing_back) {
                state->showing_back = true;
                state->message = "确认=认识，上=不认识，下=稍后";
                return ESP_OK;
            }
            ApplyAnswer(&state->progress[deck_index], true, state->today);
            ++state->reviewed_today;
            ++state->correct_today;
            state->message = "已记住";
            break;

        case WordInput::kUp:
            if (!state->showing_back) {
                if (state->queue_position > 0) {
                    --state->queue_position;
                }
                state->message = "上一词";
                return ESP_OK;
            }
            ApplyAnswer(&state->progress[deck_index], false, state->today);
            ++state->reviewed_today;
            state->message = "已标记复习";
            break;

        case WordInput::kDown:
        case WordInput::kLongConfirm:
            if (state->queue_position + 1 < state->queue.size()) {
                ++state->queue_position;
            } else {
                state->queue_position = 0;
            }
            state->showing_back = false;
            state->message = "稍后再看";
            return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(SaveProgress(*state), kTag, "save word progress");
    BuildQueue(state);
    state->showing_back = false;
    if (state->queue.empty()) {
        state->message = "今日单词已完成";
    }
    return ESP_OK;
}

WordAppSnapshot BuildWordAppSnapshot(const WordAppState& state)
{
    WordAppSnapshot snapshot;
    snapshot.showing_back = state.showing_back;
    snapshot.reviewed_today = state.reviewed_today;
    snapshot.correct_today = state.correct_today;
    snapshot.daily_target = state.daily_target;
    snapshot.due_count = CountDue(state);
    snapshot.mastered_count = CountMastered(state);
    snapshot.total_count = static_cast<uint16_t>(kWordDeckSize);
    snapshot.card_count = static_cast<uint16_t>(std::min<size_t>(state.queue.size(), UINT16_MAX));
    snapshot.card_position = state.queue.empty() ? 0 : static_cast<uint16_t>(state.queue_position + 1);
    snapshot.finished_today = state.queue.empty();
    snapshot.progress_line = WordAppProgressLabel(state);
    snapshot.status_line = WordAppStatusLine(state);
    snapshot.hint = state.message.empty() ? "确认翻面；长按确认稍后" : state.message;

    const WordCard* card = CurrentCard(state);
    if (card == nullptr) {
        snapshot.word = "今日完成";
        snapshot.meaning = "没有待复习单词";
        snapshot.hint = "长按上下切换页面";
        return snapshot;
    }
    snapshot.has_card = true;
    snapshot.word = card->word == nullptr ? "" : card->word;
    snapshot.phonetic = card->phonetic == nullptr ? "" : card->phonetic;
    snapshot.meaning = card->meaning == nullptr ? "" : card->meaning;
    snapshot.example = card->example == nullptr ? "" : card->example;
    snapshot.example_translation = card->example_translation == nullptr ? "" : card->example_translation;
    const size_t deck_index = state.queue[state.queue_position];
    if (deck_index < state.progress.size()) {
        snapshot.status_line += " · " + StatusText(state.progress[deck_index].status);
    }
    return snapshot;
}

std::string WordAppProgressLabel(const WordAppState& state)
{
    if (state.daily_target == 0) {
        return "--%";
    }
    const int percent = std::min(100, static_cast<int>(state.reviewed_today) * 100 / state.daily_target);
    return std::to_string(percent) + "%";
}

std::string WordAppStatusLine(const WordAppState& state)
{
    return "今日 " + std::to_string(state.reviewed_today) + "/" + std::to_string(state.daily_target) +
           " · 待复习 " + std::to_string(CountDue(state));
}

std::string WordAppSignature(const WordAppState& state)
{
    std::string signature;
    signature.reserve(80);
    signature.append(state.showing_back ? "back" : "front");
    signature.push_back('/');
    signature.append(std::to_string(state.queue_position));
    signature.push_back('/');
    signature.append(std::to_string(state.queue.size()));
    signature.push_back('/');
    signature.append(std::to_string(state.reviewed_today));
    signature.push_back('/');
    signature.append(std::to_string(state.correct_today));
    signature.push_back('/');
    signature.append(state.message);
    return signature;
}

}  // namespace wqn
