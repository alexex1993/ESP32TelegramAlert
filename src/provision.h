#pragma once

// First-boot / re-provisioning captive portal. Brings up an open SoftAP named
// APP_PROVISION_AP_SSID, hijacks DNS so phones auto-pop the sign-in sheet, and
// serves a one-page web form covering every device setting (WiFi creds, bot
// token, timezone, language, SOCKS5 proxy). On submit the form is validated,
// written to NVS via settings_save(), and the device reboots into station mode.
//
// The function does not return: it paints the on-screen instructions, starts
// the AP + DNS + HTTP server, and blocks. The only exit is the reboot that
// follows a successful save (or, in principle, never -- an unprovisioned device
// stays an open AP until configured).
void provision_start(void);
