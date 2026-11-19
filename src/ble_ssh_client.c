/*
 * ble_ssh_client - connect stdin/stdout to a ble_ssh_server over BLE.
 *
 * Meant to be used as an ssh ProxyCommand:
 *   ssh -o ProxyCommand="ble_ssh_client AA:BB:CC:DD:EE:FF" user@host
 *   ssh -o ProxyCommand="ble_ssh_client ssh-raspberrypi" user@host
 *
 * stdout carries the tunnel, so all diagnostics go to stderr and bluez_inc's own
 * logger (which writes to stdout) is kept disabled.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "adapter.h"
#include "agent.h"
#include "characteristic.h"
#include "device.h"
#include "logger.h"

#include "ble_ssh.h"

#define STDIN_HIGH_WATER (64 * 1024)
#define READ_CHUNK 4096

static gint opt_timeout = 20;
static gboolean opt_verbose = FALSE;
static gboolean opt_reliable = FALSE;
static gboolean opt_pair = FALSE;
static gchar **opt_target = NULL;

static const char *target = NULL;
static GMainLoop *loop = NULL;
static Adapter *adapter = NULL;
static Agent *agent = NULL;
static gboolean pairing_started = FALSE;
static gboolean reconnecting = FALSE; // after a fresh pairing, until the second link is up
static gboolean giving_up = FALSE;    // a definite failure: don't retry on the disconnect it causes
static guint reconnect_source = 0;
static guint timeout_source = 0;
static Device *device = NULL;
static GByteArray *outq = NULL;   // stdin bytes waiting to be written to RX
static gboolean write_in_flight = FALSE;
static gboolean stdin_eof = FALSE;
static gboolean tunnel_up = FALSE;
static guint stdin_watch = 0;
static int exit_code = 255;

static void msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("ble_ssh_client: ", stderr);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

#define debug(...) do { if (opt_verbose) msg(__VA_ARGS__); } while (0)

static void quit(int code) {
    exit_code = code;
    g_main_loop_quit(loop);
}

static gboolean on_timeout(gpointer data);

static void arm_timeout(void) {
    if (timeout_source != 0) g_source_remove(timeout_source);
    timeout_source = g_timeout_add_seconds((guint) MAX(opt_timeout, 1), on_timeout, NULL);
}

static gboolean on_reconnect(gpointer data) {
    reconnect_source = 0;
    debug("reconnecting to %s", binc_device_get_address(device));
    binc_device_connect(device);
    return G_SOURCE_REMOVE;
}

static void schedule_reconnect(void) {
    // bluetoothd needs a moment to tear down the old link; connecting right away races with it.
    if (reconnect_source == 0) reconnect_source = g_timeout_add(1000, on_reconnect, NULL);
}

static void start_stdin_watch(void);

static gsize chunk_size(void) {
    const guint mtu = binc_device_get_mtu(device);
    gsize size = mtu > BLE_SSH_ATT_OVERHEAD ? mtu - BLE_SSH_ATT_OVERHEAD : BLE_SSH_MIN_CHUNK;
    return CLAMP(size, BLE_SSH_MIN_CHUNK, BLE_SSH_MAX_CHUNK);
}

static void pump_writes(void) {
    if (write_in_flight || !tunnel_up) return;
    if (outq->len == 0) {
        if (stdin_eof) {
            debug("stdin closed, disconnecting");
            // Unsubscribing is what ends the session on the server, even if the link lingers.
            binc_device_stop_notify(device, BLE_SSH_SERVICE_UUID, BLE_SSH_TX_UUID);
            binc_device_disconnect(device);
        }
        return;
    }
    const gsize len = MIN(outq->len, chunk_size());
    GByteArray *chunk = g_byte_array_sized_new((guint) len);
    g_byte_array_append(chunk, outq->data, (guint) len);
    // Only one write is outstanding at a time; its completion callback sends the next.
    if (binc_device_write_char(device, BLE_SSH_SERVICE_UUID, BLE_SSH_RX_UUID, chunk,
                               opt_reliable ? WITH_RESPONSE : WITHOUT_RESPONSE)) {
        write_in_flight = TRUE;
        g_byte_array_remove_range(outq, 0, (guint) len);
    } else {
        msg("write to RX failed");
        binc_device_disconnect(device);
    }
    g_byte_array_free(chunk, TRUE);
}

static void on_write(Device *d, Characteristic *characteristic, const GByteArray *byteArray, const GError *error) {
    write_in_flight = FALSE;
    if (error != NULL) {
        msg("write failed: %s", error->message);
        binc_device_disconnect(device);
        return;
    }
    if (outq->len < STDIN_HIGH_WATER) start_stdin_watch();
    pump_writes();
}

static gboolean on_stdin(gint fd, GIOCondition condition, gpointer user_data) {
    guint8 buf[READ_CHUNK];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        stdin_eof = TRUE;
        stdin_watch = 0;
        pump_writes();
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(outq, buf, (guint) n);
    pump_writes();
    if (outq->len >= STDIN_HIGH_WATER) {
        stdin_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_stdin_watch(void) {
    if (stdin_watch == 0 && !stdin_eof) {
        stdin_watch = g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin, NULL);
    }
}

static void on_notify(Device *d, Characteristic *characteristic, const GByteArray *byteArray) {
    if (!g_str_equal(binc_characteristic_get_uuid(characteristic), BLE_SSH_TX_UUID)) return;
    const guint8 *data = byteArray->data;
    gsize len = byteArray->len;
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            msg("stdout: %s", g_strerror(errno));
            binc_device_disconnect(device);
            return;
        }
        data += n;
        len -= (gsize) n;
    }
}

static void on_notify_state(Device *d, Characteristic *characteristic, const GError *error) {
    if (error != NULL) {
        msg("enabling notifications failed: %s", error->message);
        binc_device_disconnect(device);
        return;
    }
    if (binc_characteristic_is_notifying(characteristic) && !tunnel_up) {
        tunnel_up = TRUE;
        debug("tunnel up (mtu %u)", binc_device_get_mtu(device));
        start_stdin_watch();
        pump_writes();
    }
}

static void on_services_resolved(Device *d) {
    if (!binc_device_has_service(device, BLE_SSH_SERVICE_UUID)) {
        msg("%s does not offer the ble-ssh service (is ble_ssh_server running there?)",
            binc_device_get_address(device));
        giving_up = TRUE;
        binc_device_disconnect(device);
        return;
    }
    if (opt_pair && binc_device_get_bonding_state(device) != BINC_BONDED) {
        // A server started with --require-bonding only accepts bonded centrals; bond first and
        // subscribe from on_bonding_state_changed.
        debug("pairing with %s", binc_device_get_address(device));
        pairing_started = TRUE;
        binc_device_pair(device);
        return;
    }
    binc_device_start_notify(device, BLE_SSH_SERVICE_UUID, BLE_SSH_TX_UUID);
}

static void on_bonding_state_changed(Device *d, BondingState new_state, BondingState old_state, const GError *error) {
    if (error != NULL) {
        msg("pairing failed: %s (is the server pairable?)", error->message);
        giving_up = TRUE;
        binc_device_disconnect(device);
        return;
    }
    debug("bonding state %d -> %d", old_state, new_state);
    if (new_state == BINC_BONDED && pairing_started && !tunnel_up) {
        pairing_started = FALSE;
        // bluetoothd on the peripheral side only marks us bonded once this first link drops,
        // so a server with --require-bonding would reject us now. Reconnect; the next link is
        // encrypted with the stored keys and the server sees a bonded central.
        debug("bonded, reconnecting so the server sees the bond");
        reconnecting = TRUE;
        arm_timeout(); // pairing took part of the budget; give the second link a full one
        binc_device_disconnect(device);
    }
}

static gboolean on_request_authorization(Device *d) {
    // Only reached for the server we chose to pair with (--pair); Just Works needs no input.
    return TRUE;
}

static guint32 on_request_passkey(Device *d) {
    msg("the server asked for a passkey; only Just Works pairing is supported");
    return 0;
}

static void on_connection_state(Device *d, ConnectionState state, const GError *error) {
    if (error != NULL) {
        if (!tunnel_up) {
            // Errors like le-connection-abort-by-local are transient (the previous link is
            // still being torn down, or the radio was busy); keep trying until --timeout.
            debug("connect attempt failed (%s), retrying", error->message);
            schedule_reconnect();
            return;
        }
        msg("connect failed: %s", error->message);
        quit(255);
        return;
    }
    debug("%s %s", binc_device_get_address(d), binc_device_get_connection_state_name(d));
    if (reconnecting) {
        if (state == BINC_CONNECTED) reconnecting = FALSE;
        else if (state == BINC_DISCONNECTED) schedule_reconnect();
        return;
    }
    if (state == BINC_DISCONNECTED && !tunnel_up && !giving_up && !stdin_eof) {
        // Dropped before the tunnel came up: usually the previous client's link still being
        // torn down. Try again until --timeout.
        debug("link dropped before the tunnel was up, retrying");
        schedule_reconnect();
        return;
    }
    if (state == BINC_DISCONNECTED) quit(tunnel_up ? 0 : 255);
}

static void connect_to(Device *d) {
    device = d;
    binc_device_set_connection_state_change_cb(device, &on_connection_state);
    binc_device_set_services_resolved_cb(device, &on_services_resolved);
    binc_device_set_notify_char_cb(device, &on_notify);
    binc_device_set_notify_state_cb(device, &on_notify_state);
    binc_device_set_write_char_cb(device, &on_write);
    binc_device_set_bonding_state_changed_cb(device, &on_bonding_state_changed);
    debug("connecting to %s (%s)", binc_device_get_address(device),
          binc_device_get_name(device) ? binc_device_get_name(device) : "?");
    binc_device_connect(device);
}

static gboolean matches_target(Device *d) {
    const char *name = binc_device_get_name(d);
    return g_ascii_strcasecmp(binc_device_get_address(d), target) == 0 ||
           (name != NULL && g_str_equal(name, target));
}

static void on_scan_result(Adapter *a, Device *d) {
    if (device != NULL || !matches_target(d)) return;
    binc_adapter_stop_discovery(a);
    connect_to(d);
}

static gboolean is_bt_address(const char *s) {
    if (strlen(s) != 17) return FALSE;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? s[i] != ':' : !g_ascii_isxdigit(s[i])) return FALSE;
    }
    return TRUE;
}

static void start(void) {
    Device *known = binc_adapter_get_device_by_address(adapter, target);
    if (known != NULL) {
        connect_to(known);
        return;
    }
    // A server started with --no-advertise is reached through another program's advertisement,
    // which doesn't list our service UUID. So when we know the address, scan unfiltered; a name
    // can only match our own advertisement, so filter on the UUID then.
    const gboolean by_address = is_bt_address(target);
    debug("scanning for %s%s", target, by_address ? "" : " (ble-ssh advertisers only)");
    GPtrArray *uuids = g_ptr_array_new();
    if (!by_address) g_ptr_array_add(uuids, BLE_SSH_SERVICE_UUID);
    binc_adapter_set_discovery_cb(adapter, &on_scan_result);
    binc_adapter_set_discovery_filter(adapter, -100, uuids, NULL);
    g_ptr_array_free(uuids, TRUE);
    binc_adapter_start_discovery(adapter);
}

static void on_powered(Adapter *a, gboolean state) {
    if (state && device == NULL) start();
}

static gboolean on_timeout(gpointer data) {
    timeout_source = 0;
    if (!tunnel_up) {
        msg("timed out after %d s waiting for %s", opt_timeout, target);
        if (device != NULL) binc_device_disconnect(device);
        quit(255);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_terminate(gpointer data) {
    if (device != NULL && tunnel_up) binc_device_stop_notify(device, BLE_SSH_SERVICE_UUID, BLE_SSH_TX_UUID);
    if (device != NULL) binc_device_disconnect(device);
    quit(tunnel_up ? 0 : 255);
    return G_SOURCE_REMOVE;
}

static void on_binc_log(LogLevel level, const char *tag, const char *message) {
    fprintf(stderr, "binc [%s] %s\n", tag, message);
}

int main(int argc, char **argv) {
    GOptionEntry entries[] = {
        {"timeout", 't', 0, G_OPTION_ARG_INT, &opt_timeout, "Seconds to wait for the tunnel (default 20)", "S"},
        {"reliable", 'r', 0, G_OPTION_ARG_NONE, &opt_reliable, "Use write-with-response (slower)", NULL},
        {"pair", 'p', 0, G_OPTION_ARG_NONE, &opt_pair,
         "Bond with the server first (needed for servers started with --require-bonding)", NULL},
        {"verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print progress to stderr", NULL},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_target, NULL, "ADDRESS|NAME"},
        {NULL}};
    GError *error = NULL;
    GOptionContext *context = g_option_context_new("ADDRESS|NAME - SSH over BLE, client side (ProxyCommand)");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error) || opt_target == NULL || opt_target[0] == NULL) {
        msg("%s", error != NULL ? error->message : "missing ADDRESS or NAME");
        return 2;
    }
    g_option_context_free(context);
    target = opt_target[0];

    // bluez_inc logs to stdout, which is our data channel: route it to stderr instead.
    log_set_handler(&on_binc_log);
    log_set_level(LOG_DEBUG);
    log_enabled(opt_verbose);
    signal(SIGPIPE, SIG_IGN);

    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (connection == NULL) {
        msg("system bus: %s", error->message);
        return 255;
    }
    loop = g_main_loop_new(NULL, FALSE);
    outq = g_byte_array_new();
    g_unix_signal_add(SIGINT, on_terminate, NULL);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);
    g_unix_signal_add(SIGHUP, on_terminate, NULL);

    adapter = binc_adapter_get_default(connection);
    if (adapter == NULL) {
        msg("no Bluetooth adapter found");
        return 255;
    }
    if (opt_pair) {
        // Registered only for --pair, so the client never takes over pairing dialogs otherwise.
        agent = binc_agent_create(adapter, "/org/bluez/BleSshClientAgent", NO_INPUT_NO_OUTPUT);
        binc_agent_set_request_authorization_cb(agent, &on_request_authorization);
        binc_agent_set_request_passkey_cb(agent, &on_request_passkey);
    }
    arm_timeout();
    binc_adapter_set_powered_state_cb(adapter, &on_powered);
    if (binc_adapter_get_powered_state(adapter)) {
        start();
    } else {
        binc_adapter_power_on(adapter);
    }

    g_main_loop_run(loop);

    if (agent != NULL) binc_agent_free(agent);
    if (adapter != NULL) {
        if (binc_adapter_get_discovery_state(adapter) != BINC_DISCOVERY_STOPPED) binc_adapter_stop_discovery(adapter);
        binc_adapter_free(adapter);
    }
    g_byte_array_free(outq, TRUE);
    g_main_loop_unref(loop);
    g_dbus_connection_close_sync(connection, NULL, NULL);
    g_object_unref(connection);
    g_strfreev(opt_target);
    return exit_code;
}
