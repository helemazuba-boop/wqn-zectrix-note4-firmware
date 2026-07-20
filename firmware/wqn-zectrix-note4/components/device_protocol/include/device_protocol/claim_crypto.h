#pragma once

#include <array>
#include <string>

#include "device_protocol/v3.h"
#include "esp_err.h"

namespace wqn::protocol::v3 {

class ClaimKeyPair final {
public:
    ClaimKeyPair() = default;
    ~ClaimKeyPair();

    ClaimKeyPair(const ClaimKeyPair&) = delete;
    ClaimKeyPair& operator=(const ClaimKeyPair&) = delete;

    const std::string& public_key() const { return public_key_; }
    bool valid() const { return valid_; }
    void Clear();

private:
    friend esp_err_t GenerateClaimKeyPair(ClaimKeyPair* key_pair);
    friend esp_err_t OpenSealedCredential(
        const ClaimKeyPair& key_pair,
        const std::string& claim_id,
        const SealedCredential& sealed,
        std::string* device_id,
        std::string* access_token);

    std::array<unsigned char, 32> private_key_ = {};
    std::string public_key_;
    bool valid_ = false;
};

esp_err_t GenerateClaimKeyPair(ClaimKeyPair* key_pair);
esp_err_t OpenSealedCredential(
    const ClaimKeyPair& key_pair,
    const std::string& claim_id,
    const SealedCredential& sealed,
    std::string* device_id,
    std::string* access_token);

}  // namespace wqn::protocol::v3
