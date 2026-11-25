/*
 * l2cap_echo - the smallest program on l2cap_tunnel: sends back what every central sends it.
 *
 * Usage: l2cap_echo [PSM] [--encrypted]
 *
 * A real server publishes the PSM in a characteristic of its GATT service, or uses a fixed
 * one both sides know; this example only prints it. Without a PSM the kernel picks one.
 */

#include <errno.h>
#include <glib-unix.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "l2cap_tunnel.h"

typedef struct {
    guint64 bytes; // per connection: what we echoed
} Echo;

static gboolean on_accept(L2capTunnel *tunnel, const char *address, gpointer user_data) {
    printf("%s: channel requested (%u open)\n", address, l2cap_tunnel_get_count(tunnel));
    return TRUE; // e.g. return FALSE for a central that isn't bonded
}

static void on_connected(L2capConn *conn, gpointer user_data) {
    l2cap_conn_set_user_data(conn, g_new0(Echo, 1));
    printf("%s: connected, mtu %u\n", l2cap_conn_get_address(conn), l2cap_conn_get_mtu(conn));
    fflush(stdout);
}

static void on_rx(L2capConn *conn, const guint8 *data, gsize len, gpointer user_data) {
    Echo *echo = l2cap_conn_get_user_data(conn);
    echo->bytes += len;
    l2cap_conn_send(conn, data, len);
    if (len >= 4 && memcmp(data, "bye\n", 4) == 0) l2cap_conn_close(conn); // the echo goes out first
}

static void on_disconnected(L2capConn *conn, const char *reason, gpointer user_data) {
    Echo *echo = l2cap_conn_get_user_data(conn);
    printf("%s: disconnected (%s), %" G_GUINT64_FORMAT " bytes echoed\n", l2cap_conn_get_address(conn), reason,
           echo->bytes);
    fflush(stdout);
    g_free(echo);
}

static gboolean on_signal(gpointer loop) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    guint16 psm = 0;
    L2capTunnelSecurity security = L2CAP_TUNNEL_OPEN;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--encrypted") == 0) security = L2CAP_TUNNEL_ENCRYPTED;
        else psm = (guint16) strtoul(argv[i], NULL, 0);
    }
    const L2capTunnelCallbacks callbacks = {
        .accept = on_accept,
        .connected = on_connected,
        .rx = on_rx,
        .disconnected = on_disconnected,
    };
    L2capTunnel *tunnel = l2cap_tunnel_new(psm, security, &callbacks, NULL);
    if (tunnel == NULL) {
        fprintf(stderr, "l2cap_tunnel_new: %s\n", strerror(errno));
        return 1;
    }
    printf("listening on PSM 0x%02x%s\n", l2cap_tunnel_get_psm(tunnel),
           security == L2CAP_TUNNEL_ENCRYPTED ? " (encrypted links only)" : "");
    fflush(stdout);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal, loop);
    g_unix_signal_add(SIGTERM, on_signal, loop);
    g_main_loop_run(loop);
    l2cap_tunnel_free(tunnel); // on_disconnected() for every channel still open
    g_main_loop_unref(loop);
    return 0;
}
