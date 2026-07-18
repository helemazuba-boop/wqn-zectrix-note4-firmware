#include "dns_server.h"

#include <cerrno>
#include <cstring>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace wqn::provision {
namespace {

constexpr char kTag[] = "wqn_prov_dns";
constexpr size_t kDnsPacketSize = 512;
constexpr size_t kDnsAnswerSize = 16;
constexpr int kReceiveTimeoutMs = 250;

}  // namespace

DnsServer::DnsServer()
{
    events_ = xEventGroupCreate();
    if (events_ != nullptr) {
        xEventGroupSetBits(events_, kStoppedBit);
    }
}

DnsServer::~DnsServer()
{
    const esp_err_t result = Stop(portMAX_DELAY);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "DNS task did not stop cleanly: %s", esp_err_to_name(result));
        return;
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
}

esp_err_t DnsServer::Start(esp_ip4_addr_t gateway)
{
    if (events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (running_.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }

    gateway_ = gateway;
    xEventGroupClearBits(events_, kReadyBit | kFailedBit | kStoppedBit);
    const BaseType_t created = xTaskCreate(TaskEntry, "wqn_prov_dns", 4096, this, 4, nullptr);
    if (created != pdPASS) {
        running_ = false;
        xEventGroupSetBits(events_, kStoppedBit);
        return ESP_ERR_NO_MEM;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        events_, kReadyBit | kFailedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
    if ((bits & kReadyBit) != 0) {
        return ESP_OK;
    }

    running_ = false;
    xEventGroupWaitBits(events_, kStoppedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    return (bits & kFailedBit) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t DnsServer::Stop(TickType_t timeout)
{
    if (events_ == nullptr) {
        running_ = false;
        return ESP_OK;
    }
    if ((xEventGroupGetBits(events_) & kStoppedBit) != 0) {
        running_ = false;
        return ESP_OK;
    }

    running_ = false;
    const EventBits_t bits = xEventGroupWaitBits(events_, kStoppedBit, pdFALSE, pdFALSE, timeout);
    return (bits & kStoppedBit) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

void DnsServer::TaskEntry(void* arg)
{
    auto* self = static_cast<DnsServer*>(arg);
    self->Run();
    xEventGroupSetBits(self->events_, kStoppedBit);
    vTaskDelete(nullptr);
}

void DnsServer::Run()
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "socket create failed: errno=%d", errno);
        xEventGroupSetBits(events_, kFailedBit);
        running_ = false;
        return;
    }

    timeval receive_timeout = {
        .tv_sec = 0,
        .tv_usec = kReceiveTimeoutMs * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);
    if (bind(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        ESP_LOGE(kTag, "bind UDP:53 failed: errno=%d", errno);
        close(sock);
        xEventGroupSetBits(events_, kFailedBit);
        running_ = false;
        return;
    }

    ESP_LOGI(kTag, "DNS captive redirect started");
    xEventGroupSetBits(events_, kReadyBit);

    uint8_t packet[kDnsPacketSize] = {};
    while (running_) {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        const ssize_t received = recvfrom(
            sock,
            packet,
            sizeof(packet) - kDnsAnswerSize,
            0,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len);
        if (received < 0) {
            if (!running_) {
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ulTaskNotifyTake(pdTRUE, 0);
                continue;
            }
            ESP_LOGW(kTag, "recvfrom failed: errno=%d", errno);
            continue;
        }
        const uint16_t question_count =
            static_cast<uint16_t>((static_cast<uint16_t>(packet[4]) << 8) | packet[5]);
        if (received < 12 || question_count == 0) {
            continue;
        }

        // Retain only the first DNS question. Requests commonly carry an EDNS
        // OPT record after the question; copying the whole request and claiming
        // ARCOUNT=0 would make that OPT record look like our answer. Walk the
        // QNAME safely, then discard all original answer/authority/additional
        // records before appending one captive-portal A record.
        size_t question_end = 12;
        bool valid_question = false;
        while (question_end < static_cast<size_t>(received)) {
            const uint8_t label_len = packet[question_end++];
            if (label_len == 0) {
                valid_question = question_end + 4 <= static_cast<size_t>(received);
                if (valid_question) {
                    question_end += 4;  // QTYPE + QCLASS
                }
                break;
            }
            if ((label_len & 0xC0) != 0 || label_len > 63 ||
                question_end + label_len > static_cast<size_t>(received)) {
                break;
            }
            question_end += label_len;
        }
        if (!valid_question || question_end + kDnsAnswerSize > sizeof(packet)) {
            continue;
        }

        packet[2] = static_cast<uint8_t>((packet[2] | 0x80) & ~0x02);  // QR=1, TC=0
        packet[3] = static_cast<uint8_t>(packet[3] | 0x80);             // RA=1
        packet[4] = 0;
        packet[5] = 1;  // QDCOUNT
        packet[6] = 0;
        packet[7] = 1;  // ANCOUNT
        packet[8] = packet[9] = packet[10] = packet[11] = 0;

        uint8_t* out = packet + question_end;
        const uint8_t header[] = {
            0xC0, 0x0C,              // compressed name pointer
            0x00, 0x01,              // A
            0x00, 0x01,              // IN
            0x00, 0x00, 0x00, 0x1E,  // TTL 30 s
            0x00, 0x04,              // IPv4 length
        };
        std::memcpy(out, header, sizeof(header));
        out += sizeof(header);
        std::memcpy(out, &gateway_.addr, sizeof(gateway_.addr));
        out += sizeof(gateway_.addr);

        sendto(
            sock,
            packet,
            static_cast<size_t>(out - packet),
            0,
            reinterpret_cast<sockaddr*>(&client_addr),
            client_len);
    }

    close(sock);  // The DNS task is the sole owner and closer of the socket.
    running_ = false;
    ESP_LOGI(kTag, "DNS captive redirect stopped");
}

}  // namespace wqn::provision
