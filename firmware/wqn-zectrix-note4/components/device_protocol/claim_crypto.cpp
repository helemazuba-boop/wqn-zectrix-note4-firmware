#include "device_protocol/claim_crypto.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "cJSON.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

namespace {

constexpr char kClaimInfoPrefix[] = "wqn-device-claim-v3:";
constexpr size_t kP256PrivateKeyBytes = 32;
constexpr size_t kP256PublicKeyBytes = 65;
constexpr size_t kClaimSaltBytes = 16;
constexpr size_t kClaimIvBytes = 12;
constexpr size_t kGcmTagBytes = 16;
constexpr size_t kMaxCiphertextBytes = 384;
constexpr size_t kSha256Bytes = 32;
constexpr size_t kMaxClaimIdBytes = 64;

int EspRandom(void*, unsigned char* output, size_t size)
{
    esp_fill_random(output, size);
    return 0;
}

std::string Base64UrlEncode(const unsigned char* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return {};
    }
    const size_t capacity = 4 * ((size + 2) / 3) + 1;
    std::string encoded(capacity, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(encoded.data()),
            encoded.size(),
            &written,
            data,
            size) != 0) {
        return {};
    }
    encoded.resize(written);
    for (char& value : encoded) {
        if (value == '+') {
            value = '-';
        } else if (value == '/') {
            value = '_';
        }
    }
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

esp_err_t Base64UrlDecode(
    const std::string& encoded,
    unsigned char* output,
    size_t capacity,
    size_t* size)
{
    if (encoded.empty() || output == nullptr || size == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    std::string base64 = encoded;
    for (char& value : base64) {
        if (value == '-') {
            value = '+';
        } else if (value == '_') {
            value = '/';
        } else if (!((value >= 'A' && value <= 'Z') ||
                     (value >= 'a' && value <= 'z') ||
                     (value >= '0' && value <= '9') || value == '=')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    while ((base64.size() % 4) != 0) {
        base64.push_back('=');
    }
    size_t written = 0;
    const int result = mbedtls_base64_decode(
        output,
        capacity,
        &written,
        reinterpret_cast<const unsigned char*>(base64.data()),
        base64.size());
    if (result != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *size = written;
    return ESP_OK;
}

bool IsValidToken(const std::string& token)
{
    if (token.size() != 64) {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](char value) {
        return (value >= '0' && value <= '9') ||
            (value >= 'a' && value <= 'f') ||
            (value >= 'A' && value <= 'F');
    });
}

int HkdfSha256(
    const unsigned char* salt,
    size_t salt_size,
    const unsigned char* input_key,
    size_t input_key_size,
    const std::string& info,
    unsigned char* output,
    size_t output_size)
{
    if (salt == nullptr || input_key == nullptr || output == nullptr ||
        output_size != kSha256Bytes || info.size() > kMaxClaimIdBytes +
            sizeof(kClaimInfoPrefix) - 1) {
        return -1;
    }
    const mbedtls_md_info_t* sha256 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (sha256 == nullptr) {
        return -1;
    }

    std::array<unsigned char, kSha256Bytes> pseudo_random_key = {};
    std::array<unsigned char,
        sizeof(kClaimInfoPrefix) - 1 + kMaxClaimIdBytes + 1> expand_input = {};
    int result = mbedtls_md_hmac(
        sha256,
        salt,
        salt_size,
        input_key,
        input_key_size,
        pseudo_random_key.data());
    if (result == 0) {
        std::memcpy(expand_input.data(), info.data(), info.size());
        expand_input[info.size()] = 0x01;
        result = mbedtls_md_hmac(
            sha256,
            pseudo_random_key.data(),
            pseudo_random_key.size(),
            expand_input.data(),
            info.size() + 1,
            output);
    }
    mbedtls_platform_zeroize(
        pseudo_random_key.data(), pseudo_random_key.size());
    mbedtls_platform_zeroize(expand_input.data(), expand_input.size());
    return result;
}

}  // namespace

namespace wqn::protocol::v3 {

ClaimKeyPair::~ClaimKeyPair()
{
    Clear();
}

void ClaimKeyPair::Clear()
{
    mbedtls_platform_zeroize(private_key_.data(), private_key_.size());
    public_key_.clear();
    valid_ = false;
}

esp_err_t GenerateClaimKeyPair(ClaimKeyPair* key_pair)
{
    if (key_pair == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    key_pair->Clear();

    mbedtls_ecp_keypair generated;
    mbedtls_ecp_keypair_init(&generated);
    int result = mbedtls_ecp_gen_key(
        MBEDTLS_ECP_DP_SECP256R1,
        &generated,
        EspRandom,
        nullptr);
    std::array<unsigned char, kP256PublicKeyBytes> public_key = {};
    size_t public_key_size = 0;
    if (result == 0) {
        result = mbedtls_mpi_write_binary(
            &generated.MBEDTLS_PRIVATE(d),
            key_pair->private_key_.data(),
            key_pair->private_key_.size());
    }
    if (result == 0) {
        result = mbedtls_ecp_point_write_binary(
            &generated.MBEDTLS_PRIVATE(grp),
            &generated.MBEDTLS_PRIVATE(Q),
            MBEDTLS_ECP_PF_UNCOMPRESSED,
            &public_key_size,
            public_key.data(),
            public_key.size());
    }
    mbedtls_ecp_keypair_free(&generated);
    if (result != 0 || public_key_size != kP256PublicKeyBytes ||
        public_key[0] != 0x04) {
        key_pair->Clear();
        return ESP_FAIL;
    }

    key_pair->public_key_ = Base64UrlEncode(public_key.data(), public_key_size);
    mbedtls_platform_zeroize(public_key.data(), public_key.size());
    if (key_pair->public_key_.size() < 86 || key_pair->public_key_.size() > 88) {
        key_pair->Clear();
        return ESP_FAIL;
    }
    key_pair->valid_ = true;
    return ESP_OK;
}

esp_err_t OpenSealedCredential(
    const ClaimKeyPair& key_pair,
    const std::string& claim_id,
    const SealedCredential& sealed,
    std::string* device_id,
    std::string* access_token)
{
    if (!key_pair.valid_ || claim_id.empty() || device_id == nullptr ||
        access_token == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    device_id->clear();
    access_token->clear();

    std::array<unsigned char, kP256PublicKeyBytes> server_public_key = {};
    std::array<unsigned char, 32> shared_secret = {};
    std::array<unsigned char, kClaimSaltBytes> salt = {};
    std::array<unsigned char, kClaimIvBytes> iv = {};
    std::array<unsigned char, 32> aes_key = {};
    std::array<unsigned char, kMaxCiphertextBytes> ciphertext = {};
    std::array<unsigned char, kMaxCiphertextBytes> plaintext = {};
    size_t server_public_key_size = 0;
    size_t salt_size = 0;
    size_t iv_size = 0;
    size_t ciphertext_size = 0;

    esp_err_t error = Base64UrlDecode(
        sealed.server_public_key,
        server_public_key.data(),
        server_public_key.size(),
        &server_public_key_size);
    if (error == ESP_OK) {
        error = Base64UrlDecode(
            sealed.salt, salt.data(), salt.size(), &salt_size);
    }
    if (error == ESP_OK) {
        error = Base64UrlDecode(sealed.iv, iv.data(), iv.size(), &iv_size);
    }
    if (error == ESP_OK) {
        error = Base64UrlDecode(
            sealed.ciphertext,
            ciphertext.data(),
            ciphertext.size(),
            &ciphertext_size);
    }
    if (error != ESP_OK || server_public_key_size != kP256PublicKeyBytes ||
        server_public_key[0] != 0x04 || salt_size != kClaimSaltBytes ||
        iv_size != kClaimIvBytes || ciphertext_size <= kGcmTagBytes) {
        error = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    {
        mbedtls_ecp_group group;
        mbedtls_ecp_point peer;
        mbedtls_mpi private_key;
        mbedtls_mpi shared;
        mbedtls_ecp_group_init(&group);
        mbedtls_ecp_point_init(&peer);
        mbedtls_mpi_init(&private_key);
        mbedtls_mpi_init(&shared);

        int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
        if (result == 0) {
            result = mbedtls_ecp_point_read_binary(
                &group,
                &peer,
                server_public_key.data(),
                server_public_key_size);
        }
        if (result == 0) {
            result = mbedtls_ecp_check_pubkey(&group, &peer);
        }
        if (result == 0) {
            result = mbedtls_mpi_read_binary(
                &private_key,
                key_pair.private_key_.data(),
                key_pair.private_key_.size());
        }
        if (result == 0) {
            result = mbedtls_ecp_check_privkey(&group, &private_key);
        }
        if (result == 0) {
            result = mbedtls_ecdh_compute_shared(
                &group,
                &shared,
                &peer,
                &private_key,
                EspRandom,
                nullptr);
        }
        if (result == 0) {
            result = mbedtls_mpi_write_binary(
                &shared,
                shared_secret.data(),
                shared_secret.size());
        }

        mbedtls_mpi_free(&shared);
        mbedtls_mpi_free(&private_key);
        mbedtls_ecp_point_free(&peer);
        mbedtls_ecp_group_free(&group);
        if (result != 0) {
            error = ESP_FAIL;
            goto cleanup;
        }
    }

    {
        const std::string info = std::string(kClaimInfoPrefix) + claim_id;
        if (claim_id.size() > kMaxClaimIdBytes ||
            HkdfSha256(
                salt.data(),
                salt_size,
                shared_secret.data(),
                shared_secret.size(),
                info,
                aes_key.data(),
                aes_key.size()) != 0) {
            error = ESP_FAIL;
            goto cleanup;
        }
    }

    {
        const size_t encrypted_size = ciphertext_size - kGcmTagBytes;
        mbedtls_gcm_context gcm;
        mbedtls_gcm_init(&gcm);
        int result = mbedtls_gcm_setkey(
            &gcm,
            MBEDTLS_CIPHER_ID_AES,
            aes_key.data(),
            aes_key.size() * 8);
        if (result == 0) {
            result = mbedtls_gcm_auth_decrypt(
                &gcm,
                encrypted_size,
                iv.data(),
                iv_size,
                nullptr,
                0,
                ciphertext.data() + encrypted_size,
                kGcmTagBytes,
                ciphertext.data(),
                plaintext.data());
        }
        mbedtls_gcm_free(&gcm);
        if (result != 0 || encrypted_size >= plaintext.size()) {
            error = ESP_ERR_INVALID_CRC;
            goto cleanup;
        }
        plaintext[encrypted_size] = '\0';

        cJSON* root = cJSON_ParseWithLength(
            reinterpret_cast<const char*>(plaintext.data()), encrypted_size);
        cJSON* protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
        cJSON* parsed_device_id =
            cJSON_GetObjectItemCaseSensitive(root, "device_id");
        cJSON* parsed_token =
            cJSON_GetObjectItemCaseSensitive(root, "access_token");
        const bool valid = cJSON_IsObject(root) && cJSON_IsNumber(protocol) &&
            protocol->valueint == 3 && cJSON_IsString(parsed_device_id) &&
            parsed_device_id->valuestring != nullptr &&
            parsed_device_id->valuestring[0] != '\0' && cJSON_IsString(parsed_token) &&
            parsed_token->valuestring != nullptr;
        if (valid) {
            *device_id = parsed_device_id->valuestring;
            *access_token = parsed_token->valuestring;
        }
        cJSON_Delete(root);
        if (!valid || !IsValidToken(*access_token)) {
            device_id->clear();
            access_token->clear();
            error = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    error = ESP_OK;

cleanup:
    mbedtls_platform_zeroize(server_public_key.data(), server_public_key.size());
    mbedtls_platform_zeroize(shared_secret.data(), shared_secret.size());
    mbedtls_platform_zeroize(salt.data(), salt.size());
    mbedtls_platform_zeroize(iv.data(), iv.size());
    mbedtls_platform_zeroize(aes_key.data(), aes_key.size());
    mbedtls_platform_zeroize(ciphertext.data(), ciphertext.size());
    mbedtls_platform_zeroize(plaintext.data(), plaintext.size());
    return error;
}

}  // namespace wqn::protocol::v3
