#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace wqn {

// One capture producer, one network consumer. Storage is supplied separately
// so firmware can require PSRAM, while host tests run this exact state machine.
template <size_t Capacity = 192, size_t Samples = 360>
class FlashUplinkQueue {
public:
    static_assert(Capacity > 0 && Capacity < UINT32_MAX);
    static_assert(Samples > 0 && Samples <= UINT16_MAX);
    struct Block {
        uint32_t generation;
        uint16_t sample_count;
        int16_t samples[Samples];
    };
    enum class Phase : uint32_t {
        kIdle, kRecording, kDraining, kComplete, kFinalSending, kFinalSent,
        kCancelled, kFailed,
    };
    enum class Failure : uint32_t {
        kNone, kOverflow, kTransport, kInvalidAudio,
    };
    static constexpr size_t kStorageSlots = Capacity + 1;
    static constexpr size_t kStorageBytes = sizeof(Block) * kStorageSlots;

    void Configure(Block* storage) { // before workers exist; starts raw PSRAM object lifetimes
        if (storage) {
            for (size_t i = 0; i < kStorageSlots; ++i) new (storage + i) Block{};
        }
        storage_ = storage;
    }

    // The orchestrator serializes Begin. Both workers must have acknowledged
    // the prior generation before any slot/counter can be reset.
    uint32_t Begin(uint32_t session) {
        if (!storage_ || session == 0 || !ProducerFinished() || !SenderFinished())
            return 0;
        const Phase old = phase_.load(std::memory_order_acquire);
        if (old == Phase::kRecording || old == Phase::kDraining ||
            old == Phase::kFinalSending ||
            (old == Phase::kComplete && accepted_.load() != 0))
            return 0;
        // An abandoned append must never be spliced into the next utterance.
        if ((old == Phase::kFailed || old == Phase::kCancelled) &&
            session == session_.load(std::memory_order_relaxed))
            return 0;
        uint32_t next = generation_.load(std::memory_order_relaxed) + 1;
        if (next == 0) next = 1;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        accepted_.store(0, std::memory_order_relaxed);
        sent_.store(0, std::memory_order_relaxed);
        peak_.store(0, std::memory_order_relaxed);
        failure_.store(Failure::kNone, std::memory_order_relaxed);
        producer_done_.store(false, std::memory_order_relaxed);
        session_.store(session, std::memory_order_relaxed);
        phase_.store(Phase::kRecording, std::memory_order_relaxed);
        generation_.store(next, std::memory_order_release);
        // The idle sender reads this acquire flag before loading Generation.
        // No new queue indices/payload may be used before publication completes.
        sender_done_.store(false, std::memory_order_release);
        return next;
    }

    bool Push(uint32_t generation, const int16_t* samples, size_t count) {
        if (!Current(generation) || phase_.load(std::memory_order_acquire) != Phase::kRecording ||
            producer_done_.load(std::memory_order_acquire)) return false;
        if (!samples || count == 0 || count > Samples) {
            Fail(generation, Failure::kInvalidAudio);
            return false;
        }
        const uint32_t write = write_.load(std::memory_order_relaxed);
        const uint32_t read = read_.load(std::memory_order_acquire);
        const uint32_t next = Advance(write);
        if (next == read) {
            Fail(generation, Failure::kOverflow);
            return false;
        }
        Block& slot = storage_[write];
        slot.generation = generation;
        slot.sample_count = static_cast<uint16_t>(count);
        std::memcpy(slot.samples, samples, count * sizeof(int16_t));
        accepted_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t depth = write >= read ? write - read + 1
            : static_cast<uint32_t>(kStorageSlots) - read + write + 1;
        if (depth > peak_.load(std::memory_order_relaxed))
            peak_.store(depth, std::memory_order_relaxed);
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool Pop(uint32_t generation, Block* output) {
        if (!Current(generation) || !output || Stopped()) return false;
        const uint32_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        *output = storage_[read];
        read_.store(Advance(read), std::memory_order_release);
        if (output->generation != generation || output->sample_count == 0 || output->sample_count > Samples) {
            Fail(generation, Failure::kInvalidAudio);
            return false;
        }
        return true;
    }

    void ProducerDone(uint32_t generation) {
        if (!Current(generation)) return;
        Phase expected = Phase::kRecording;
        phase_.compare_exchange_strong(expected, Phase::kDraining,
                                      std::memory_order_acq_rel);
        producer_done_.store(true, std::memory_order_release);
    }
    void RecordSent(uint32_t generation) {
        if (Current(generation)) sent_.fetch_add(1, std::memory_order_relaxed);
    }
    bool SenderCanFinish(uint32_t generation) const {
        return Current(generation) && (Stopped() || (ProducerFinished() && Empty()));
    }
    bool SenderDone(uint32_t generation) {
        if (!SenderCanFinish(generation)) return false;
        if (!Stopped()) {
            Phase expected = Phase::kDraining;
            phase_.compare_exchange_strong(expected, Phase::kComplete,
                                          std::memory_order_acq_rel);
        }
        sender_done_.store(true, std::memory_order_release);
        return true;
    }

    void Cancel(uint32_t generation) {
        if (Current(generation)) phase_.store(Phase::kCancelled, std::memory_order_release);
    }
    void Fail(uint32_t generation, Failure reason) {
        if (!Current(generation)) return;
        Failure expected = Failure::kNone;
        failure_.compare_exchange_strong(expected, reason, std::memory_order_relaxed);
        Phase phase = phase_.load(std::memory_order_acquire);
        while (phase != Phase::kCancelled && phase != Phase::kFailed &&
               !phase_.compare_exchange_weak(phase, Phase::kFailed, std::memory_order_acq_rel)) {}
    }
    bool ClaimFinal(uint32_t generation) {
        if (!Current(generation) || !ProducerFinished() || !SenderFinished() ||
            !Empty() || Accepted() == 0 || Accepted() != Sent()) return false;
        Phase expected = Phase::kComplete;
        return phase_.compare_exchange_strong(expected, Phase::kFinalSending,
                                             std::memory_order_acq_rel);
    }
    void FinalDone(uint32_t generation, bool success) {
        if (!Current(generation)) return;
        if (!success) { Fail(generation, Failure::kTransport); return; }
        Phase expected = Phase::kFinalSending;
        phase_.compare_exchange_strong(expected, Phase::kFinalSent,
                                      std::memory_order_acq_rel);
    }

    uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }
    uint32_t Session() const { return session_.load(std::memory_order_acquire); }
    bool Current(uint32_t generation) const { return generation != 0 && generation == Generation(); }
    bool ProducerFinished() const { return producer_done_.load(std::memory_order_acquire); }
    bool SenderFinished() const { return sender_done_.load(std::memory_order_acquire); }
    bool Empty() const { return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire); }
    Phase State() const { return phase_.load(std::memory_order_acquire); }
    bool Stopped() const { const auto p = State(); return p == Phase::kCancelled || p == Phase::kFailed; }
    Failure Error() const { return failure_.load(std::memory_order_acquire); }
    uint32_t Accepted() const { return accepted_.load(std::memory_order_acquire); }
    uint32_t Sent() const { return sent_.load(std::memory_order_acquire); }
    uint32_t Peak() const { return peak_.load(std::memory_order_acquire); }

private:
    static uint32_t Advance(uint32_t value) {
        return value + 1 == kStorageSlots ? 0 : value + 1;
    }
    Block* storage_ = nullptr;
    std::atomic<uint32_t> generation_{0}, session_{0}, read_{0}, write_{0};
    std::atomic<uint32_t> accepted_{0}, sent_{0}, peak_{0};
    std::atomic<bool> producer_done_{true}, sender_done_{true};
    std::atomic<Phase> phase_{Phase::kIdle};
    std::atomic<Failure> failure_{Failure::kNone};
};

} // namespace wqn
