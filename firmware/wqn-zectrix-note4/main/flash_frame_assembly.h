#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wqn {

// Receiver-owned. TCP callbacks within one WS frame and RFC6455 continuation
// frames are separate boundaries; validate both before allocating/appending.
class FlashFrameAssembly {
public:
    enum class Result { kPending, kComplete, kInvalid };

    Result Feed(uint8_t opcode, bool fin, uint64_t offset, uint64_t total,
                const char* data, size_t size)
    {
        if (offset == 0) {
            if (frame_active_) return Invalid();
            if (opcode == 1 || opcode == 2) {
                if (message_active_) return Invalid();
                message_active_ = true;
                opcode_ = opcode;
                bytes_.clear();
            } else if (opcode != 0 || !message_active_) {
                return Invalid();
            }
            const size_t cap = opcode_ == 2 ? 64 * 1024 : 128 * 1024;
            if (total > cap || bytes_.size() > cap - total) return Invalid();
            frame_active_ = true;
            frame_opcode_ = opcode;
            frame_fin_ = fin;
            frame_total_ = total;
            frame_offset_ = 0;
        }
        if (!frame_active_ || opcode != frame_opcode_ || fin != frame_fin_ ||
            total != frame_total_ || offset != frame_offset_ || offset > total ||
            size > total - offset || (size != 0 && data == nullptr)) return Invalid();
        if (size != 0) bytes_.append(data, size);
        frame_offset_ += size;
        if (frame_offset_ != frame_total_) return Result::kPending;
        frame_active_ = false;
        if (!fin) return Result::kPending;
        message_active_ = false;
        return Result::kComplete;
    }

    void Release() { Reset(); std::string().swap(bytes_); }

    uint8_t opcode() const { return opcode_; }
    const std::string& bytes() const { return bytes_; }
    void Reset()
    {
        frame_active_ = false;
        message_active_ = false;
        bytes_.clear();
    }

private:
    Result Invalid() { Reset(); return Result::kInvalid; }
    std::string bytes_;
    bool frame_active_ = false;
    bool message_active_ = false;
    bool frame_fin_ = false;
    uint8_t opcode_ = 0;
    uint8_t frame_opcode_ = 0;
    uint64_t frame_offset_ = 0;
    uint64_t frame_total_ = 0;
};

}  // namespace wqn
