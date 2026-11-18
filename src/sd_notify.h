/*
 * Minimal systemd notification support (READY, STATUS, WATCHDOG, STOPPING) without a
 * libsystemd dependency. Everything is a no-op when not started by systemd.
 */
#ifndef BLE_SSH_SD_NOTIFY_H
#define BLE_SSH_SD_NOTIFY_H

#include <glib.h>

// Send a raw notification such as "READY=1\nSTATUS=...". Returns TRUE if it was delivered.
gboolean sd_notify_send(const char *state);

gboolean sd_notify_printf(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

// If the unit sets WatchdogSec=, ping systemd from the GLib main loop at half that interval,
// so a stuck main loop gets the service restarted. Returns TRUE if a watchdog was armed.
gboolean sd_notify_start_watchdog(void);

#endif
