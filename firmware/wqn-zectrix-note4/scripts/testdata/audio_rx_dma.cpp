#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 257;
constexpr int ESP_ERR_INVALID_ARG = 258, ESP_ERR_INVALID_STATE = 259;
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_DMA = 2, MALLOC_CAP_8BIT = 4;
constexpr int I2S_NUM_0 = 0, I2S_ROLE_MASTER = 1;
constexpr char kTag[] = "audio";
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
const char* esp_err_to_name(int) { return "host-error"; }

int checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
    std::abort(); } } while (false)

namespace wqn {
void RecordError(const char*, const char*) {}
namespace services {
enum class AudioState { kCapturing };
const char* AudioStateName(AudioState) { return "capturing"; }
struct AudioSession {
    uint32_t id = 1;
    explicit operator bool() const { return id != 0; }
};
using AudioChannelHandle = void*;
}  // namespace services
}  // namespace wqn
using namespace wqn::services;

struct i2s_chan_config_t {
    int id = 0, role = 0;
    uint32_t dma_desc_num = 0, dma_frame_num = 0;
    bool auto_clear_after_cb = false, auto_clear_before_cb = false;
    int intr_priority = 0;
};
struct i2s_std_config_t {};
struct Allocation { size_t block, size; };
struct Channel {
    i2s_chan_config_t config;
    std::vector<Allocation> allocations;
    bool enabled = false;
};
using i2s_chan_handle_t = Channel*;

std::atomic<AudioState> g_state{AudioState::kCapturing};
std::atomic<bool> g_accept_operations{true};
constexpr size_t kMaxChannelHandles = 2;
int g_channel_slots[2] = {};
struct DriverOperation {
    bool valid;
    explicit DriverOperation(const AudioSession& session)
        : valid(session.id == 1 && g_accept_operations.load()) {}
    explicit operator bool() const { return valid; }
};

std::vector<size_t> blocks;
std::vector<i2s_chan_config_t> attempts;
std::vector<int> init_errors;
int deletes = 0, disables = 0, live_channels = 0, heap_dumps = 0;
int registration_error = ESP_OK;
bool delete_fails = false;
void* registered = nullptr;

size_t heap_caps_get_free_size(int)
{
    size_t total = 0;
    for (size_t size : blocks) total += size;
    return total;
}
size_t heap_caps_get_largest_free_block(int)
{
    return *std::max_element(blocks.begin(), blocks.end());
}
struct multi_heap_info_t {
    size_t total_free_bytes, total_allocated_bytes, largest_free_block;
    size_t minimum_free_bytes, allocated_blocks, free_blocks, total_blocks;
};
void heap_caps_get_info(multi_heap_info_t* info, int) {
    ++heap_dumps;
    *info = {};
    info->total_free_bytes = heap_caps_get_free_size(0);
    info->largest_free_block = heap_caps_get_largest_free_block(0);
    info->free_blocks = blocks.size();
}

bool Allocate(Channel* channel, size_t bytes)
{
    // Best-fit fragments with 8 bytes of allocator overhead per allocation.
    bytes += 8;
    size_t best = blocks.size();
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i] >= bytes && (best == blocks.size() || blocks[i] < blocks[best])) {
            best = i;
        }
    }
    if (best == blocks.size()) return false;
    blocks[best] -= bytes;
    channel->allocations.push_back({best, bytes});
    return true;
}
void FreeDma(Channel* channel)
{
    for (const auto& allocation : channel->allocations) {
        blocks[allocation.block] += allocation.size;
    }
    channel->allocations.clear();
}
int i2s_new_channel(const i2s_chan_config_t* config, void*, Channel** out)
{
    CHECK(live_channels == 0);  // A failed attempt must be deleted before retry.
    attempts.push_back(*config);
    *out = new Channel{*config, {}, false};
    ++live_channels;
    return ESP_OK;
}
i2s_std_config_t MakeStandardConfig(uint32_t, bool, bool) { return {}; }
int i2s_channel_init_std_mode(Channel* channel, const i2s_std_config_t*)
{
    if (attempts.size() <= init_errors.size()) return init_errors[attempts.size() - 1];
    // Model the IDF driver's separate descriptor/sample allocations and its
    // partial-allocation unwind. Driver objects/interrupts are outside this seam.
    for (uint32_t i = 0; i < channel->config.dma_desc_num; ++i) {
        if (!Allocate(channel, 12) || !Allocate(channel, channel->config.dma_frame_num * 4)) {
            FreeDma(channel);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}
int i2s_channel_enable(Channel* channel) { channel->enabled = true; return ESP_OK; }
int i2s_channel_disable(Channel* channel)
{
    ++disables;
    channel->enabled = false;
    return ESP_OK;
}
int i2s_del_channel(Channel* channel)
{
    ++deletes;
    if (delete_fails) return ESP_FAIL;
    CHECK(!channel->enabled);
    FreeDma(channel);
    delete channel;
    --live_channels;
    return ESP_OK;
}
int RegisterResource(int*, size_t, const AudioSession&, Channel* channel)
{
    if (registration_error != ESP_OK) return registration_error;
    registered = channel;
    return ESP_OK;
}

@@CONSTANTS@@
@@CONFIG@@
@@CREATE@@

void Reset(std::vector<size_t> free_blocks)
{
    CHECK(live_channels == 0);
    blocks = std::move(free_blocks);
    attempts.clear();
    init_errors.clear();
    deletes = disables = heap_dumps = 0;
    registration_error = ESP_OK;
    delete_fails = false;
    registered = nullptr;
}
int Create(AudioChannelHandle* rx, uint32_t frames = 128)
{
    return CreateAudioRxChannel(AudioSession{}, 16000, frames, true, rx);
}
void Release(AudioChannelHandle* rx)
{
    CHECK(*rx == registered);
    auto* channel = static_cast<Channel*>(*rx);
    if (channel->enabled) CHECK(i2s_channel_disable(channel) == ESP_OK);
    CHECK(i2s_del_channel(channel) == ESP_OK);
    registered = nullptr;
    *rx = nullptr;
}
void CheckCapacity(uint32_t frames = 128)
{
    for (const auto& config : attempts) {
        CHECK(config.dma_desc_num * config.dma_frame_num == 6 * frames);
    }
}

int main()
{
    AudioChannelHandle rx = nullptr;
    Reset({16000});
    CHECK(Create(&rx) == ESP_OK);
    CHECK(attempts.size() == 1);
    CHECK(attempts[0].dma_desc_num == 6 && attempts[0].dma_frame_num == 128);
    Release(&rx);
    CHECK(heap_caps_get_free_size(0) == 16000);

    // About 8 KiB free, largest block 1216 B: six 512 B sample buffers fail,
    // but twelve 256 B buffers fit, without shrinking the recording ring.
    std::vector<size_t> fragments{1216, 600, 600, 384, 384, 384, 384};
    fragments.insert(fragments.end(), 32, 128);
    Reset(fragments);
    const size_t total = heap_caps_get_free_size(0);
    CHECK(total == 8048 && heap_caps_get_largest_free_block(0) == 1216);
    CHECK(Create(&rx) == ESP_OK);
    CHECK(attempts.size() == 2 && deletes == 1);
    CHECK(attempts.back().dma_desc_num == 12 && attempts.back().dma_frame_num == 64);
    CheckCapacity();
    Release(&rx);
    CHECK(heap_caps_get_free_size(0) == total);

    Reset(std::vector<size_t>(40, 208));
    CHECK(Create(&rx) == ESP_OK);
    CHECK(attempts.size() == 3 && deletes == 2);
    CHECK(attempts.back().dma_desc_num == 24 && attempts.back().dma_frame_num == 32);
    CheckCapacity();
    Release(&rx);
    CHECK(heap_caps_get_free_size(0) == 8320);

    Reset({16000});
    init_errors = {ESP_ERR_NO_MEM, ESP_ERR_NO_MEM, ESP_ERR_NO_MEM};
    CHECK(Create(&rx) == ESP_ERR_NO_MEM);
    CHECK(rx == nullptr && registered == nullptr && live_channels == 0);
    CHECK(attempts.size() == 3 && deletes == 3 && heap_dumps == 1);

    Reset({16000});
    init_errors = {ESP_ERR_INVALID_ARG};
    CHECK(Create(&rx) == ESP_ERR_INVALID_ARG);
    CHECK(attempts.size() == 1 && deletes == 1);
    CHECK(rx == nullptr && live_channels == 0);

    Reset({16000});
    init_errors = {ESP_ERR_NO_MEM};
    delete_fails = true;
    CHECK(Create(&rx) == ESP_FAIL);
    CHECK(attempts.size() == 1 && deletes == 1 && live_channels == 1);
    CHECK(rx != nullptr && rx == registered);
    delete_fails = false;
    Release(&rx);

    Reset({16000});
    registration_error = ESP_ERR_INVALID_STATE;
    CHECK(Create(&rx) == ESP_ERR_INVALID_STATE);
    CHECK(rx == nullptr && live_channels == 0 && disables == 1 && deletes == 1);
    CHECK(heap_caps_get_free_size(0) == 16000);

    Reset({16000});
    init_errors = {ESP_ERR_NO_MEM};
    CHECK(Create(&rx, 240) == ESP_OK);
    CHECK(attempts.size() == 2 && attempts.back().dma_frame_num == 120);
    CheckCapacity(240);
    Release(&rx);

    for (uint32_t frames : {129U, 32U}) {
        Reset({16000});
        init_errors = {ESP_ERR_NO_MEM};
        CHECK(Create(&rx, frames) == ESP_ERR_NO_MEM);
        CHECK(attempts.size() == 1 && deletes == 1 && live_channels == 0);
    }
    Reset({16000});
    CHECK(CreateAudioRxChannel(AudioSession{0}, 16000, 128, true, &rx) == ESP_ERR_INVALID_STATE);
    CHECK(Create(nullptr) == ESP_ERR_INVALID_STATE);
    CHECK(attempts.empty());
    std::printf("audio RX DMA: %d checks passed (host allocation/cleanup only)\n", checks);
}
