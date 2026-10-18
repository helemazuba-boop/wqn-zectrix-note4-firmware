# WiFi provisioning and role repair

Primary and backup are configured identities. A successful failover changes the
active connection, never either network's role. The existing `wifi_creds` blob
layout/version is unchanged; its current `preferred` index becomes the fixed
primary identity on upgrade. Historical roles already exchanged by old firmware
cannot be reconstructed; the networks currently displayed are preserved.

Both portal roles can be saved in one visit. Saving does not close the portal;
Finish does. Each role retains its own draft, and pending saves lock editing and
Finish. Passwords stay on the device. The finish page checks the exit response,
offers retry when unacknowledged, and reports saved configuration rather than
claiming successful WiFi association.

The provisioning owner reports Connecting only after AP/DNS/HTTP teardown.
Connectivity reports Connected after an actual current STA IP event and Failed
after the initial connection/failover budget is exhausted. That bounded handoff
retains the existing connectivity lease even without a cloud demand. STA mode is
restored on reused drivers; full 32-byte SSIDs and 64-byte PSKs are copied intact.

## Offline validation

- `node scripts/test_wifi_provision.js`: 24 production-JavaScript assertions.
- `python3 scripts/test_wifi_connectivity.py`: 23 role/retry assertions and
  10 driver-handoff assertions, production code with controlled host ports.
- `python3 scripts/test_ui_settings.py`: 204 checks without the developer menu,
  219 with it, including actual settings input/render/result seams.
- `idf.py -B build-wsl build`: zero warnings, M8 ownership and compiled UI stack
  gates pass. `git -c core.whitespace=cr-at-eol diff --check` is clean for this change.

These checks do not prove radio behavior or RTOS ordering on hardware. Device
acceptance still needs: first-time primary+backup setup in one visit; invalid
primary with working backup; both APs absent/wrong password; loss of the phone's
exit response; repeated re-provisioning with no residual hotspot; full-length
credentials; and subsequent idle/deep-sleep/wake connectivity. No flash or HIL
was performed for this local change.

## Direct network switching

WiFi management now offers Connect primary, Connect backup, Re-provision and
Device pairing. Each network shows its saved SSID and whether it is connected.
The UI submits an operation ID and consumes only that operation's terminal
snapshot; leaving the page does not turn the operation into an unbounded lease.

`ConnectivityTask` loads the saved credentials and owns all driver calls.
Existing cloud/AI/sync demand, provisioning and sleep quiesce reject the switch
without interrupting the current link. Selecting the current online network is
a no-op. A different target gets one attempt; failure tries the other saved
network once, with the existing 15-second association and 15-second DHCP budgets.
Success, fallback and total failure are distinct results. Queuing failure,
rejection and every terminal result release the request's connectivity lease.
No new wake source or background polling is added.

The successful manual choice is retained for subsequent radio admissions in the
same boot. Restart and deep-sleep wake restore the configured primary priority.
Neither manual switching nor fallback writes credentials or exchanges roles.
Already-queued radio events from previous credential generations are ignored.

Final offline checks: 60 role/connectivity assertions, 10 driver-handoff
assertions, 24 portal-JavaScript assertions, and 217/232 settings assertions with
the developer menu off/on. The settings test also renders the native WiFi page
and checks current-link indicators and stale operation IDs. ESP-IDF build,
zero-warning, M8 ownership and compiled UI-stack gates pass.

Additional device acceptance: primary-to-backup and reverse, selecting the
current network, missing backup, active AI/sync/download, target failure with
successful fallback, both networks failing, rapid repeated requests, old radio
events during handoff, and sleep admission after each terminal outcome. These
remain HIL work; no device flash was performed.

## Native provisioning screen

The device screen now implements the approved About-page visual language:
brand mark, wordmark and Note4 identity, without a top status bar or persistent
bottom action hints. Starting, waiting, connecting, connected and failed states
share one native renderer. The existing About page and phone portal stay as-is.

Waiting shows two real QR symbols: an open-WiFi join payload for the actual
provisioning AP SSID, then `http://192.168.4.1/`. Captions retain manual entry.
The WiFi payload escapes delimiters, accepts up to 32 SSID bytes, and contains
no saved STA password. An MIT-licensed, unmodified Project Nayuki C encoder is
vendored in `components/qr_code`; versions 3–6, medium error correction,
integer pixel scaling and a four-module quiet zone fit the 400x300 display.

The existing settings tick copies primary, backup and active identities from
the connectivity service into UI state. Connecting shows saved roles;
Connected shows the actual active SSID, including a successful backup fallback.
Identity changes participate in frame signatures so late handoff data repaints.
Rendering performs no storage access or network operations. The existing EPD
owner, refresh policy, provisioning lifecycle and input behavior are retained;
there is no new task, lease or wake source.

Validation of this UI change:

- `python3 scripts/test_ui_settings.py --render-dir <output>`: 235 checks with
  developer menu off, 250 on; native pixels for all five states, no top bar or
  footer, real SSID projection, late identity updates and no storage submissions.
- `python3 scripts/test_provision_qr.py`: 25 production encoder/payload checks.
  With `--verify-frames <output>` (Pillow and zxing-cpp installed), 16 symbols
  decoded from the actual native framebuffers: hotspot and URL, normal SSID,
  escaped delimiters, Chinese characters and a maximum-length escaped SSID,
  in both developer-menu configurations.
- Navigation: 1308 assertions. Existing connectivity: 60 role/switch checks,
  10 driver-handoff checks. Portal JavaScript: 24 checks. M8 CTest fixture: 1/1.
- ESP-IDF `build-wsl`: zero warnings; M8 ownership and compiled UI-stack gates
  pass. CRLF-aware diff check is clean. Native framebuffer images were inspected.

These are host/build checks. Real-panel appearance, phone-camera scanning and
end-to-end hotspot/STA handoff still require device HIL; no flash was performed.
