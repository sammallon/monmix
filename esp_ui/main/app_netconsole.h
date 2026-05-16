#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Networked admin console.
//
// Once WiFi is associated, a TCP listener binds to port APP_NETCONSOLE_PORT
// (4242 by default; see app_netconsole.c). Exactly one client at a time --
// a new connect drops the previous session.
//
// Wire protocol is line-oriented, plain TCP. Lines are LF-terminated;
// CRLF accepted on input. Responses are LF-terminated. After connect:
//
//   greeting:  "monmix net-console v1\nauth: required\n"
//
// First command MUST be:
//
//   auth <token>          token comes from APP_NET_TOKEN in secrets.h
//
// On auth success: "OK authed\n". Token is plain TCP (LAN-only), so the
// LAN must be trusted. On failure: "ERR auth failed\n" + close.
//
// Post-auth commands (one per line):
//
//   ping                  -> "pong\n"
//   log-tail              -> begin streaming log lines (APP_LOGD_* and
//                            IDF ESP_LOGx) until log-stop. Reply: "OK log-tail\n".
//                            Subsequent log lines arrive interleaved with
//                            any other command output.
//   log-stop              -> "OK log-stop\n"
//   ota <url>             -> "OK ota\n" then a stream of progress lines:
//                            "OTA_BEGIN size=N\n", "OTA_PROGRESS done=N/N\n",
//                            "OTA_FINISH ok\n" or "OTA_ERROR <message>\n".
//                            On success, device reboots after ~1 s and the
//                            client connection drops.
//   reboot                -> "OK reboot\n", esp_restart() in 200 ms
//   quit                  -> "OK bye\n" and close.
//
// Unauthed clients only see "ERR auth required\n" for anything but
// `auth`. The greeting omits any version-specific build identifiers
// so the protocol stays stable for the host tooling.

// Initialise the network console. Spawns the listener task; bind/accept
// loop runs in there. Safe to call before WiFi is up -- the task waits
// for app_wifi_get_state() == CONNECTED.
//
// app_power must be initialised first because OTA mark-valid logic
// runs alongside; calling order: app_logd_init -> app_wifi_init_radio
// -> app_power_init -> app_netconsole_init.
void app_netconsole_init(void);

// Returns true if a client is currently connected AND authenticated.
// Used by app_ota's progress emit path to decide whether to send
// updates over the network.
bool app_netconsole_is_active(void);

// Send a line to the currently connected (and authed) client. No-op if
// no client. Non-blocking -- if the socket buffer is full, the line is
// dropped. Newline is appended if the caller didn't include one.
// Used by app_ota to push progress updates and OTA result lines.
void app_netconsole_send_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
