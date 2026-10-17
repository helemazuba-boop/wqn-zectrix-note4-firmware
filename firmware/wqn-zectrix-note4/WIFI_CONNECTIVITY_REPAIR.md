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
