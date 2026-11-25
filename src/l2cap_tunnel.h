/*
 * l2cap_tunnel - LE L2CAP connection-oriented channels as a small server API on a GLib main loop.
 *
 * Two files, l2cap_tunnel.c and this header, that a BLE server can take as they are: they
 * need GLib and a Linux kernel with Bluetooth, nothing else (no BlueZ headers, no D-Bus, no
 * bluez_inc). The program keeps its own GATT server; this only adds the channels.
 *
 * Every channel a central opens is a connection with its own handle (L2capConn):
 *
 *     static void on_rx(L2capConn *c, const guint8 *data, gsize len, gpointer user_data) {
 *         l2cap_conn_send(c, data, len); // echo
 *     }
 *     static const L2capTunnelCallbacks callbacks = {.rx = on_rx};
 *     L2capTunnel *tunnel = l2cap_tunnel_new(0, L2CAP_TUNNEL_ENCRYPTED, &callbacks, NULL);
 *     // publish l2cap_tunnel_get_psm(tunnel) in a characteristic of your GATT service
 *
 * - A byte stream in each direction, like TCP: the protocol on top does its own framing. rx
 *   gets the bytes of one SDU at a time; l2cap_conn_send() queues bytes, which go out in SDUs
 *   of up to the central's MTU (l2cap_conn_get_mtu()), several sends possibly in one SDU.
 * - Sending never blocks: the bytes are queued and go out as the central gives credits. Use
 *   l2cap_conn_get_queued() and the writable callback for back-pressure.
 * - Closing: l2cap_conn_close() sends what is queued first; LE channels have no half close,
 *   and close() on the socket would drop what the kernel still holds.
 * - All callbacks run on the GLib main context the tunnel was created on (the default one).
 *   They may call any l2cap_conn_* function, including l2cap_conn_close(), but not
 *   l2cap_tunnel_free().
 *
 * Only the server side (peripheral) is here: a central opens the channels.
 */

#ifndef L2CAP_TUNNEL_H
#define L2CAP_TUNNEL_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct L2capTunnel L2capTunnel;
typedef struct L2capConn L2capConn;

typedef enum {
    L2CAP_TUNNEL_OPEN = 1,      // no encryption required (BT_SECURITY_LOW)
    L2CAP_TUNNEL_ENCRYPTED = 2, // only encrypted links, i.e. paired centrals (BT_SECURITY_MEDIUM)
} L2capTunnelSecurity;

typedef struct {
    // Optional: decide whether to take a new channel from this central (upper case
    // AA:BB:CC:DD:EE:FF), e.g. to check that it is bonded or to limit the number of
    // connections. Return FALSE to close it at once; NULL accepts every channel.
    gboolean (*accept)(L2capTunnel *tunnel, const char *address, gpointer user_data);
    // A channel is open and ready; l2cap_conn_send() works from here on.
    void (*connected)(L2capConn *conn, gpointer user_data);
    // Bytes from the central (one SDU). Not called any more once l2cap_conn_close() is.
    void (*rx)(L2capConn *conn, const guint8 *data, gsize len, gpointer user_data);
    // The queue fell below the low water mark after having been above the high one.
    void (*writable)(L2capConn *conn, gpointer user_data);
    // The channel is closed: by the central, by an error, or after l2cap_conn_close(). The
    // handle is freed when this returns. reason is for logging.
    void (*disconnected)(L2capConn *conn, const char *reason, gpointer user_data);
} L2capTunnelCallbacks;

// Listen on psm (0x80-0xff), or with psm 0 on one the kernel picks. Returns NULL with errno
// set if the socket can't be made (no Bluetooth, PSM in use, ...). The callbacks are copied.
L2capTunnel *l2cap_tunnel_new(guint16 psm, L2capTunnelSecurity security, const L2capTunnelCallbacks *callbacks,
                              gpointer user_data);

// Stops listening and closes every channel at once, without waiting for queued bytes; each
// gets its disconnected callback (reason "tunnel freed"), so the program can free its data
// there as always.
void l2cap_tunnel_free(L2capTunnel *tunnel);

// The PSM the channels are on: what to publish to the centrals.
guint16 l2cap_tunnel_get_psm(const L2capTunnel *tunnel);

// The connections that are open (or closing), in the order they were opened.
guint l2cap_tunnel_get_count(const L2capTunnel *tunnel);

// Queue len bytes for the central; they are sent from the main loop, so neither this nor
// l2cap_conn_close() ever calls a callback. Returns FALSE (and queues nothing) if the
// connection is closing or closed.
gboolean l2cap_conn_send(L2capConn *conn, const guint8 *data, gsize len);

// Close the channel once the queued bytes have been sent (and the kernel has handed them to
// the controller), at most a few seconds later. disconnected follows. Calling it again, or
// after the central closed, does nothing.
void l2cap_conn_close(L2capConn *conn);

// Bytes queued and not yet handed to the kernel.
gsize l2cap_conn_get_queued(const L2capConn *conn);

// The central's receive MTU: the largest SDU sent to it.
guint16 l2cap_conn_get_mtu(const L2capConn *conn);

// The central's address, upper case AA:BB:CC:DD:EE:FF.
const char *l2cap_conn_get_address(const L2capConn *conn);

L2capTunnel *l2cap_conn_get_tunnel(const L2capConn *conn);

// Your own data for this connection.
void l2cap_conn_set_user_data(L2capConn *conn, gpointer data);
gpointer l2cap_conn_get_user_data(const L2capConn *conn);

// Queue levels for the writable callback; the defaults are 32 KiB and 8 KiB.
void l2cap_tunnel_set_water_marks(L2capTunnel *tunnel, gsize high, gsize low);

G_END_DECLS

#endif
