# QR Code encoder

Unmodified Project Nayuki C encoder (MIT), copied from the existing local
reference dependency `espressif2022__esp_emote_gfx/src/lib/qrcode/qrcodegen.{c,h}`.
The complete copyright and permission notice remains in both source files.
Upstream: https://www.nayuki.io/page/qr-code-generator-library

No ESP/LVGL adapter or hardware ownership is imported. The encoder uses caller
buffers, performs no I/O and allocates no heap memory. The provisioning wrapper
limits versions to 3–6, with two 212-byte buffers and medium error correction.
See `main/ui/provision_qr.*` for WiFi payload escaping and bounded buffer use.
