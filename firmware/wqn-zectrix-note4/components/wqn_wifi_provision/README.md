# WQN WiFi Provisioning Component

This component is a focused adaptation of the SoftAP provisioning design from
[`78/esp-wifi-connect`](https://github.com/78/esp-wifi-connect), version 3.0.2
(MIT), as shipped in the ZecTrix/xiaozhi reference firmware.

Only the reusable provisioning layer was adapted:

- SoftAP + captive portal
- asynchronous WiFi scans cached outside the HTTP task stack
- chunked scan responses
- DNS redirection
- ordered HTTP/DNS/AP teardown

The upstream `WifiManager`, `WifiStation`, `SsidManager`, SmartConfig, saved-network
management, and advanced OTA/power settings are deliberately excluded. Credential
storage and STA connection remain owned by the WQN firmware through
`provision_manager.cpp` and `storage.cpp`.

WQN-specific safety changes include bounded DNS responses, cooperative DNS task
completion, JSON-safe SSID serialization, no STA connection attempt inside an HTTP
handler, and no WiFi driver deinitialization during AP-to-STA handoff.
