#include "ui/provision_qr.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int checks = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::abort(); } ++checks; } while (0)

int main()
{
    using namespace device_ui_internal;
    std::string payload = "stale";
    CHECK(!BuildProvisionWifiPayload("", &payload) && payload.empty());
    CHECK(!BuildProvisionWifiPayload(std::string(33, 'a'), &payload) && payload.empty());
    CHECK(!BuildProvisionWifiPayload(std::string("a\0b", 3), &payload) && payload.empty());
    CHECK(!BuildProvisionWifiPayload("ssid", nullptr));
    CHECK(BuildProvisionWifiPayload("WQN_N4_REAL", &payload));
    CHECK(payload == "WIFI:T:nopass;S:WQN_N4_REAL;;");
    CHECK(BuildProvisionWifiPayload("Note;4,Lab:\\\"'", &payload));
    CHECK(payload == "WIFI:T:nopass;S:Note\\;4\\,Lab\\:\\\\\\\"\\';;");
    CHECK(BuildProvisionWifiPayload("设备热点-Note4", &payload));
    CHECK(payload == "WIFI:T:nopass;S:设备热点-Note4;;");
    CHECK(BuildProvisionWifiPayload(std::string(32, ';'), &payload));
    CHECK(payload.size() == 82);

    ProvisionQrCode qr;
    CHECK(qr.size() == 0 && !qr.module(0, 0));
    CHECK(qr.Encode(payload));
    CHECK(qr.size() >= 29 && qr.size() <= 41);
    CHECK(123 / (qr.size() + 2 * kProvisionQrQuietModules) >= 2);
    CHECK(!qr.module(-1, 0) && !qr.module(qr.size(), 0));
    CHECK(qr.Encode(kProvisionPortalUrl));
    CHECK(qr.size() == 29);
    CHECK(!qr.Encode(std::string(300, 'x')));
    CHECK(qr.size() == 0 && !qr.module(0, 0));
    CHECK(qr.Encode("valid"));
    CHECK(!qr.Encode(std::string("bad\0data", 8)));
    CHECK(qr.size() == 0);
    CHECK(!qr.Encode(""));
    std::printf("Provision QR: %d checks PASS; production encoder and payload boundaries\n", checks);
}
