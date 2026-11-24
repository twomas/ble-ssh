/*
 * ble_ssh_client - connect stdin/stdout to a ble_ssh_server over BLE.
 *
 * Meant to be used as an ssh ProxyCommand:
 *   ssh -o ProxyCommand="ble_ssh_client AA:BB:CC:DD:EE:FF" user@host
 *   ssh -o ProxyCommand="ble_ssh_client ssh-raspberrypi" user@host
 *
 * stdout carries the tunnel, so all diagnostics go to stderr and bluez_inc's own
 * logger (which writes to stdout) is kept disabled.
 *
 * With --l2cap, a server that offers an LE L2CAP channel (ble_ssh_server --l2cap) is reached
 * over that instead of GATT writes and notifications; otherwise GATT is used as before.
 * Channels share the link, so a second client on this machine joins a link that is up.
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
#include "ble_ssh_l2cap.h"

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
static gboolean quitting = FALSE;    // disconnecting on the way out; exit once the link is down
static guint quit_source = 0;
static gboolean checking_linked = FALSE; // waiting for the GATT tree of a link that was already up
static guint linked_wait_source = 0;
static guint restart_source = 0;
static gboolean linked = FALSE; // we had a link to device, so BlueZ may have read its GAP name
static gboolean opt_l2cap = FALSE;
static gboolean opt_nus = FALSE;

// The last bytes the server sent, to tell a --pty server's end of session (its end marker,
// then a dropped link) from a link that drops.
#define END_MARKER_LEN (sizeof(BLE_SSH_PTY_END_MARKER) - 1)
static guint8 tail[END_MARKER_LEN];
static gsize tail_len = 0;

static void remember_tail(const guint8 *data, gsize len) {
    if (len >= END_MARKER_LEN) {
        memcpy(tail, data + len - END_MARKER_LEN, END_MARKER_LEN);
        tail_len = END_MARKER_LEN;
        return;
    }
    const gsize keep = MIN(tail_len, END_MARKER_LEN - len);
    memmove(tail, tail + tail_len - keep, keep);
    memcpy(tail + keep, data, len);
    tail_len = keep + len;
}

static gboolean server_ended_session(void) {
    return tail_len == END_MARKER_LEN && memcmp(tail, BLE_SSH_PTY_END_MARKER, END_MARKER_LEN) == 0;
}
// The tunnel's service and characteristics: ble-ssh's own, or with --nus the Nordic UART Service.
static const char *svc_uuid = BLE_SSH_SERVICE_UUID;
static const char *rx_uuid = BLE_SSH_RX_UUID;
static const char *tx_uuid = BLE_SSH_TX_UUID;
static int l2_fd = -1;           // the L2CAP channel, while connecting or up
static guint l2_watch = 0;       // connect completion, then channel readable
static guint l2_out_watch = 0;   // channel writable again, while outq waits for credits
static guint16 l2_sndmtu = 0;
static gboolean via_l2cap = FALSE;   // the tunnel runs over the channel, not GATT
static gboolean joined_link = FALSE; // the link was up already (another client's): never drop it
static gboolean l2_ended = FALSE;

#define QUIT_GRACE_MS 5000
#define LINKED_WAIT_MS 3000

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

static gboolean on_quit_grace(gpointer data) {
    quit_source = 0;
    debug("link still up after %d ms, exiting anyway", QUIT_GRACE_MS);
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

// Exit only after BlueZ has dropped the link. Disconnect() is an asynchronous D-Bus call:
// quitting right after it can close our bus connection before the call goes out, and BlueZ
// then keeps the link up with nobody using it.
static void disconnect_and_quit(int code) {
    exit_code = code;
    if (device == NULL || binc_device_get_connection_state(device) == BINC_DISCONNECTED) {
        g_main_loop_quit(loop);
        return;
    }
    quitting = TRUE;
    // Unsubscribing is what ends the session on the server, even if the link lingers.
    if (tunnel_up && !via_l2cap) binc_device_stop_notify(device, svc_uuid, tx_uuid);
    binc_device_disconnect(device); // a connect still in progress is dropped once it completes
    if (quit_source == 0) quit_source = g_timeout_add(QUIT_GRACE_MS, on_quit_grace, NULL);
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

// After a link that was up, bluetoothd needs a moment to tear it down; connecting right away
// races with it. A connect that failed (le-connection-abort-by-local: the controller sent the
// connect request but never heard the server) has nothing to tear down, so try again sooner:
// with a poor radio link most attempts fail like this, and each one is a new chance.
#define RECONNECT_MS 1000
#define RETRY_FAILED_MS 250

static void schedule_reconnect(guint delay_ms) {
    if (reconnect_source == 0) reconnect_source = g_timeout_add(delay_ms, on_reconnect, NULL);
}

static void start_stdin_watch(void);

static gsize chunk_size(void) {
    const guint mtu = binc_device_get_mtu(device);
    gsize size = mtu > BLE_SSH_ATT_OVERHEAD ? mtu - BLE_SSH_ATT_OVERHEAD : BLE_SSH_MIN_CHUNK;
    return CLAMP(size, BLE_SSH_MIN_CHUNK, BLE_SSH_MAX_CHUNK);
}

// ---------------------------------------------------------------------------------------
// L2CAP channel (--l2cap)

static int l2_end_code = 0;
static guint l2_drain_source = 0;
static guint l2_drain_tries = 0;

static void l2_finish(void) {
    const int code = l2_end_code;
    if (l2_watch != 0) g_source_remove(l2_watch);
    if (l2_out_watch != 0) g_source_remove(l2_out_watch);
    l2_watch = l2_out_watch = 0;
    if (l2_fd >= 0) close(l2_fd);
    l2_fd = -1;
    if (joined_link) {
        device = NULL; // not ours to disconnect or forget
        quit(code);
        return;
    }
    disconnect_and_quit(code);
}

static gboolean on_l2_drain(gpointer data) {
    if (ble_ssh_l2cap_unsent(l2_fd) > 0 && ++l2_drain_tries < BLE_SSH_L2CAP_DRAIN_TRIES) return G_SOURCE_CONTINUE;
    if (l2_drain_tries >= BLE_SSH_L2CAP_DRAIN_TRIES) debug("L2CAP channel not drained, closing anyway");
    l2_drain_source = 0;
    l2_finish();
    return G_SOURCE_REMOVE;
}

// Close the channel, which ends the session on the server, and leave. A link we joined
// belongs to another client: leave it up. close() drops what the kernel has not sent yet,
// so on a normal end wait for that first.
static void l2_end(int code) {
    if (l2_ended) return;
    l2_ended = TRUE;
    l2_end_code = code;
    if (code == 0 && l2_fd >= 0 && via_l2cap && ble_ssh_l2cap_unsent(l2_fd) > 0) {
        l2_drain_source = g_timeout_add(BLE_SSH_L2CAP_DRAIN_MS, on_l2_drain, NULL);
        return;
    }
    l2_finish();
}

static gboolean on_l2_writable(gint fd, GIOCondition condition, gpointer user_data);

static void l2_pump(void) {
    if (!tunnel_up || l2_out_watch != 0 || l2_fd < 0) return;
    while (outq->len > 0) {
        const gsize len = MIN(outq->len, l2_sndmtu);
        const ssize_t n = send(l2_fd, outq->data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Out of credits: the server hasn't taken the earlier SDUs yet.
                l2_out_watch = g_unix_fd_add(l2_fd, G_IO_OUT, on_l2_writable, NULL);
                return;
            }
            msg("L2CAP send: %s", g_strerror(errno));
            l2_end(255);
            return;
        }
        g_byte_array_remove_range(outq, 0, (guint) n);
    }
    if (stdin_eof) {
        debug("stdin closed, closing the L2CAP channel");
        l2_end(0);
        return;
    }
    start_stdin_watch();
}

static gboolean on_l2_writable(gint fd, GIOCondition condition, gpointer user_data) {
    l2_out_watch = 0;
    l2_pump();
    return G_SOURCE_REMOVE;
}

static gboolean on_l2_readable(gint fd, GIOCondition condition, gpointer user_data) {
    guint8 buf[BLE_SSH_L2CAP_MTU];
    const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        l2_watch = 0;
        debug("L2CAP channel closed (%s)", n == 0 ? "by the server" : g_strerror(errno));
        l2_end(0);
        return G_SOURCE_REMOVE;
    }
    const guint8 *data = buf;
    gsize len = (gsize) n;
    while (len > 0) {
        const ssize_t w = write(STDOUT_FILENO, data, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            msg("stdout: %s", g_strerror(errno));
            l2_watch = 0;
            l2_end(255);
            return G_SOURCE_REMOVE;
        }
        data += w;
        len -= (gsize) w;
    }
    return G_SOURCE_CONTINUE;
}

static void start_gatt(void) {
    binc_device_start_notify(device, svc_uuid, tx_uuid);
}

static gboolean on_l2_connected(gint fd, GIOCondition condition, gpointer user_data) {
    l2_watch = 0;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
    if (err == 0 && (condition & (G_IO_ERR | G_IO_HUP))) err = ECONNRESET;
    if (err != 0) {
        debug("L2CAP channel failed (%s), using GATT", g_strerror(err));
        close(fd);
        l2_fd = -1;
        start_gatt();
        return G_SOURCE_REMOVE;
    }
    l2_sndmtu = ble_ssh_l2cap_sndmtu(fd);
    via_l2cap = TRUE;
    tunnel_up = TRUE;
    debug("tunnel up over L2CAP (send mtu %u)", l2_sndmtu);
    l2_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_l2_readable, NULL);
    start_stdin_watch();
    l2_pump();
    return G_SOURCE_REMOVE;
}

static gboolean l2_connect(guint16 psm) {
    // A bonded link is encrypted with the stored keys; a server with --require-bonding wants it.
    const gboolean bonded = binc_device_get_bonding_state(device) == BINC_BONDED;
    const int fd = ble_ssh_l2cap_socket(SOCK_NONBLOCK, bonded ? BLE_SSH_BT_SECURITY_MEDIUM : BLE_SSH_BT_SECURITY_LOW, 0);
    if (fd < 0) {
        debug("L2CAP socket: %s", g_strerror(errno));
        return FALSE;
    }
    struct ble_ssh_sockaddr_l2 addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = ble_ssh_htobs(psm);
    const char *type = binc_device_get_address_type(device);
    addr.l2_bdaddr_type = g_strcmp0(type, "random") == 0 ? BLE_SSH_BDADDR_LE_RANDOM : BLE_SSH_BDADDR_LE_PUBLIC;
    if (ble_ssh_str2ba(binc_device_get_address(device), &addr.l2_bdaddr) < 0 ||
        (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 && errno != EINPROGRESS)) {
        debug("L2CAP connect to PSM 0x%02x: %s", psm, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    debug("opening L2CAP channel to PSM 0x%02x (%s address)", psm, type ? type : "public");
    l2_fd = fd;
    l2_watch = g_unix_fd_add(fd, G_IO_OUT | G_IO_ERR | G_IO_HUP, on_l2_connected, NULL);
    return TRUE;
}

static void on_read_char(Device *d, Characteristic *characteristic, const GByteArray *byteArray, const GError *error) {
    if (!g_str_equal(binc_characteristic_get_uuid(characteristic), BLE_SSH_PSM_UUID) || tunnel_up || l2_fd >= 0) return;
    if (error != NULL && binc_device_get_connection_state(device) != BINC_CONNECTED) {
        // The link dropped while reading (desktop servers do that, see the README); the
        // reconnect reads again once the services are resolved.
        debug("reading the L2CAP PSM ended with the link (%s)", error->message);
        return;
    }
    if (error != NULL || byteArray == NULL || byteArray->len < 2) {
        debug("could not read the server's L2CAP PSM (%s), using GATT", error ? error->message : "short value");
        start_gatt();
        return;
    }
    const guint16 psm = (guint16) (byteArray->data[0] | (byteArray->data[1] << 8));
    if (!l2_connect(psm)) start_gatt();
}

// Open the tunnel: over an L2CAP channel when asked for and offered, else over GATT.
static void start_tunnel(void) {
    if (opt_l2cap && binc_device_get_characteristic(device, svc_uuid, BLE_SSH_PSM_UUID) != NULL) {
        if (binc_device_read_char(device, svc_uuid, BLE_SSH_PSM_UUID)) return;
        debug("reading the server's L2CAP PSM failed, using GATT");
    } else if (opt_l2cap) {
        debug("the server offers no L2CAP channel, using GATT");
    }
    start_gatt();
}

// ---------------------------------------------------------------------------------------

static void pump_writes(void) {
    if (via_l2cap || l2_fd >= 0) {
        l2_pump();
        return;
    }
    if (write_in_flight || !tunnel_up) return;
    if (outq->len == 0) {
        if (stdin_eof) {
            debug("stdin closed, disconnecting");
            // Unsubscribing is what ends the session on the server, even if the link lingers.
            binc_device_stop_notify(device, svc_uuid, tx_uuid);
            binc_device_disconnect(device);
        }
        return;
    }
    const gsize len = MIN(outq->len, chunk_size());
    GByteArray *chunk = g_byte_array_sized_new((guint) len);
    g_byte_array_append(chunk, outq->data, (guint) len);
    // Only one write is outstanding at a time; its completion callback sends the next.
    if (binc_device_write_char(device, svc_uuid, rx_uuid, chunk,
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
    if (!g_str_equal(binc_characteristic_get_uuid(characteristic), tx_uuid)) return;
    // BlueZ signals TX's value to every process watching the device: on a shared link these
    // are another client's GATT session (only ours when we subscribed ourselves).
    if (via_l2cap || l2_fd >= 0 || joined_link || !tunnel_up) return;
    remember_tail(byteArray->data, byteArray->len);
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
    if (error != NULL && (tunnel_up || quitting)) {
        // Unsubscribing on the way out; BlueZ may already have ended the notify session.
        debug("stopping notifications: %s", error->message);
        return;
    }
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

static void handle_linked(void);

static void on_services_resolved(Device *d) {
    if (checking_linked) {
        handle_linked();
        return;
    }
    if (!binc_device_has_service(device, svc_uuid)) {
        msg("%s does not offer the %s service (is ble_ssh_server%s running there?)",
            binc_device_get_address(device), opt_nus ? "Nordic UART" : "ble-ssh", opt_nus ? " --nus" : "");
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
    start_tunnel();
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
    if (quitting) {
        debug("%s %s", binc_device_get_address(d), binc_device_get_connection_state_name(d));
        if (state == BINC_CONNECTED && error == NULL) binc_device_disconnect(device);
        else if (state == BINC_DISCONNECTED || error != NULL) g_main_loop_quit(loop);
        return;
    }
    if (error != NULL) {
        if (!tunnel_up) {
            // Errors like le-connection-abort-by-local are transient (the previous link is
            // still being torn down, or the radio was busy); keep trying until --timeout.
            debug("connect attempt failed (%s), retrying", error->message);
            schedule_reconnect(RETRY_FAILED_MS);
            return;
        }
        msg("connect failed: %s", error->message);
        quit(255);
        return;
    }
    debug("%s %s", binc_device_get_address(d), binc_device_get_connection_state_name(d));
    if (state == BINC_CONNECTED) linked = TRUE;
    if (reconnecting) {
        if (state == BINC_CONNECTED) reconnecting = FALSE;
        else if (state == BINC_DISCONNECTED) schedule_reconnect(RECONNECT_MS);
        return;
    }
    if (state == BINC_DISCONNECTED && !tunnel_up && !giving_up && !stdin_eof) {
        // Dropped before the tunnel came up: usually the previous client's link still being
        // torn down. Try again until --timeout.
        debug("link dropped before the tunnel was up, retrying");
        schedule_reconnect(RECONNECT_MS);
        return;
    }
    if (state == BINC_DISCONNECTED && tunnel_up && !stdin_eof && server_ended_session()) {
        debug("the server ended the session");
    } else if (state == BINC_DISCONNECTED && tunnel_up && !stdin_eof) {
        // ssh still had the session open, and the server keeps the link when a session ends,
        // so this is a drop: what was in flight is lost. The usual cause is a server without
        // ReverseServiceDiscovery = false, whose bluetoothd reads our own GATT services (some
        // need pairing), fails to pair and drops the link a few seconds in. Same text as the
        // Python client's.
        msg("the link dropped while the session was open; if this happens a few seconds after "
            "connecting, set ReverseServiceDiscovery = false on the server (README: BlueZ configuration)");
    }
    if (state == BINC_DISCONNECTED) quit(tunnel_up ? 0 : 255);
}

// Decide what to do with a link that was already up when we started.
static void handle_linked(void) {
    checking_linked = FALSE;
    if (linked_wait_source != 0) {
        g_source_remove(linked_wait_source);
        linked_wait_source = 0;
    }
    if (opt_l2cap && binc_device_get_characteristic(device, svc_uuid, BLE_SSH_PSM_UUID) != NULL) {
        // L2CAP channels share the link, each its own session: join it instead of dropping it.
        debug("joining the link to %s that is already up", binc_device_get_address(device));
        joined_link = TRUE;
        start_tunnel();
        return;
    }
    Characteristic *tx = binc_device_get_characteristic(device, svc_uuid, tx_uuid);
    if (tx != NULL && binc_characteristic_is_notifying(tx)) {
        // BlueZ has one link per device, and the server one session per link.
        msg("the link to %s is in use by another client on this machine", binc_device_get_address(device));
        device = NULL; // not ours to disconnect
        quit(255);
        return;
    }
    // Left behind by an earlier client. Drop it; on_connection_state then connects afresh.
    debug("dropping a leftover link to %s", binc_device_get_address(device));
    binc_device_disconnect(device);
}

static gboolean on_linked_wait(gpointer data) {
    linked_wait_source = 0;
    debug("services of the existing link not known after %d ms", LINKED_WAIT_MS);
    handle_linked();
    return G_SOURCE_REMOVE;
}

static void connect_to(Device *d) {
    device = d;
    binc_device_set_connection_state_change_cb(device, &on_connection_state);
    binc_device_set_services_resolved_cb(device, &on_services_resolved);
    binc_device_set_notify_char_cb(device, &on_notify);
    binc_device_set_notify_state_cb(device, &on_notify_state);
    binc_device_set_write_char_cb(device, &on_write);
    binc_device_set_read_char_cb(device, &on_read_char);
    binc_device_set_bonding_state_changed_cb(device, &on_bonding_state_changed);
    if (binc_device_get_connection_state(device) != BINC_DISCONNECTED) {
        // A link that is already up: BlueZ won't Connect() it again. Whether another client is
        // using it shows in TX's Notifying, which bluez_inc only knows once it has read the
        // GATT tree; that happens in the background, so wait for it if needed.
        if (binc_device_get_services(device) == NULL) {
            checking_linked = TRUE;
            linked_wait_source = g_timeout_add(LINKED_WAIT_MS, on_linked_wait, NULL);
            return;
        }
        handle_linked();
        return;
    }
    debug("connecting to %s (%s)", binc_device_get_address(device),
          binc_device_get_name(device) ? binc_device_get_name(device) : "?");
    binc_device_connect(device);
}

static gboolean matches_name(Device *d) {
    const char *name = binc_device_get_name(d);
    const char *alias = binc_device_get_alias(d);
    return (name != NULL && g_str_equal(name, target)) || (alias != NULL && g_str_equal(alias, target));
}

static gboolean matches_target(Device *d) {
    return g_ascii_strcasecmp(binc_device_get_address(d), target) == 0 || matches_name(d);
}

// A server we are still linked to doesn't show up in a scan, so look among the links first.
static Device *find_linked_by_name(void) {
    Device *found = NULL;
    GList *devices = binc_adapter_get_devices(adapter);
    for (GList *it = devices; it != NULL && found == NULL; it = it->next) {
        Device *d = it->data;
        if (binc_device_get_connection_state(d) != BINC_DISCONNECTED && matches_name(d)) found = d;
    }
    g_list_free(devices);
    return found;
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
    const gboolean by_address = is_bt_address(target);
    if (by_address) {
        // binc looks devices up by address only (and asserts on anything else); BlueZ's
        // addresses are upper case.
        gchar *address = g_ascii_strup(target, -1);
        Device *known = binc_adapter_get_device_by_address(adapter, address);
        g_free(address);
        if (known != NULL) {
            connect_to(known);
            return;
        }
    } else {
        Device *linked = find_linked_by_name();
        if (linked != NULL) {
            connect_to(linked);
            return;
        }
    }
    // Unfiltered, by address or by name: a server started with --no-advertise is reached through
    // another program's advertisement, which carries that program's name and not our service
    // UUID. Whether the device runs ble-ssh is checked once its services are resolved.
    debug("scanning for %s", target);
    GPtrArray *uuids = g_ptr_array_new();
    binc_adapter_set_discovery_cb(adapter, &on_scan_result);
    binc_adapter_set_discovery_filter(adapter, -100, uuids, NULL);
    g_ptr_array_free(uuids, TRUE);
    binc_adapter_start_discovery(adapter);
}

static gboolean on_restart(gpointer data) {
    restart_source = 0;
    if (device == NULL && !quitting) start();
    return G_SOURCE_REMOVE;
}

// BlueZ drops a device it found in a scan once it has been disconnected for a while (30 s by
// default), also while we keep retrying a connect that fails. binc frees it right after this
// callback, so forget it, and look for the server again (BlueZ has no other way back to it).
static void on_device_removed(Adapter *a, Device *d) {
    if (d != device) return;
    debug("%s was removed by BlueZ", binc_device_get_address(d));
    device = NULL;
    checking_linked = FALSE;
    if (reconnect_source != 0) {
        g_source_remove(reconnect_source);
        reconnect_source = 0;
    }
    if (linked_wait_source != 0) {
        g_source_remove(linked_wait_source);
        linked_wait_source = 0;
    }
    if (quitting || giving_up || tunnel_up || stdin_eof) {
        g_main_loop_quit(loop);
        return;
    }
    reconnecting = FALSE;
    pairing_started = FALSE;
    // Not from inside the callback: d is still in binc's device list until it returns.
    if (restart_source == 0) restart_source = g_idle_add(on_restart, NULL);
}

static void on_powered(Adapter *a, gboolean state) {
    if (state && device == NULL) start();
}

static gboolean on_timeout(gpointer data) {
    timeout_source = 0;
    if (!tunnel_up) {
        msg("timed out after %d s waiting for %s", opt_timeout, target);
        if (joined_link || l2_fd >= 0) l2_end(255);
        else disconnect_and_quit(255);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_terminate(gpointer data) {
    // ssh sends SIGHUP when the session ends, often right after closing our stdin.
    if (quitting) return G_SOURCE_CONTINUE;
    if (via_l2cap || joined_link || l2_fd >= 0) l2_end(tunnel_up ? 0 : 255);
    else disconnect_and_quit(tunnel_up ? 0 : 255);
    return G_SOURCE_CONTINUE; // a second signal while we wait is ignored; the grace timer ends it
}

// Once linked, BlueZ replaces the name the server advertises (in its scan response) with the
// one its GAP service gives, which is the server's adapter Alias and often different, and
// stores it with the device. A later lookup by name then misses the server until a scan
// response gets through, which on a poor radio link can take long. So remove a device that
// nothing keeps (not bonded, not trusted): BlueZ keeps the advertised name in its cache and
// uses it again when the device next shows up in a scan.
static void forget_device(void) {
    if (!linked || binc_device_get_connection_state(device) != BINC_DISCONNECTED) return;
    if (binc_device_get_bonding_state(device) != BINC_BOND_NONE || binc_device_get_trusted(device)) return;
    debug("removing %s from BlueZ", binc_device_get_address(device));
    binc_adapter_remove_device(adapter, device);
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
        {"l2cap", 'L', 0, G_OPTION_ARG_NONE, &opt_l2cap,
         "Use the server's L2CAP channel if it offers one (ble_ssh_server --l2cap), else GATT", NULL},
        {"nus", 0, 0, G_OPTION_ARG_NONE, &opt_nus,
         "Talk to a server started with --nus (Nordic UART Service instead of ble-ssh's)", NULL},
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
    if (opt_nus) {
        svc_uuid = BLE_SSH_NUS_SERVICE_UUID;
        rx_uuid = BLE_SSH_NUS_RX_UUID;
        tx_uuid = BLE_SSH_NUS_TX_UUID;
    }

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
    binc_adapter_set_device_removal_cb(adapter, &on_device_removed);
    binc_adapter_set_powered_state_cb(adapter, &on_powered);
    if (binc_adapter_get_powered_state(adapter)) {
        start();
    } else {
        binc_adapter_power_on(adapter);
    }

    g_main_loop_run(loop);

    if (agent != NULL) binc_agent_free(agent);
    if (adapter != NULL && device != NULL) forget_device();
    if (adapter != NULL) {
        if (binc_adapter_get_discovery_state(adapter) != BINC_DISCOVERY_STOPPED) binc_adapter_stop_discovery(adapter);
        binc_adapter_free(adapter);
    }
    g_byte_array_free(outq, TRUE);
    g_main_loop_unref(loop);
    g_dbus_connection_flush_sync(connection, NULL, NULL);
    g_dbus_connection_close_sync(connection, NULL, NULL);
    g_object_unref(connection);
    g_strfreev(opt_target);
    return exit_code;
}
