/*
 * l2cap_tunnel - LE L2CAP connection-oriented channels on a GLib main loop. See l2cap_tunnel.h.
 */

#define _GNU_SOURCE // accept4

#include "l2cap_tunnel.h"

#include <errno.h>
#include <glib-unix.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// The kernel's Bluetooth socket API, written out so that no BlueZ headers are needed. The
// values are the Linux ABI from <bluetooth/bluetooth.h> and <bluetooth/l2cap.h>.
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define L2T_BTPROTO_L2CAP 0
#define L2T_SOL_BLUETOOTH 274
#define L2T_BT_SECURITY 4
#define L2T_BT_SNDMTU 12
#define L2T_BT_RCVMTU 13
#define L2T_BDADDR_LE_PUBLIC 1

#define L2T_RCV_MTU 8192        // the largest SDU we take; the kernel reassembles it from PDUs
#define L2T_DEFAULT_HIGH (32 * 1024)
#define L2T_DEFAULT_LOW (8 * 1024)
#define L2T_DRAIN_MS 20         // closing: how often to look whether the kernel has sent its queue
#define L2T_DRAIN_TRIES 250     // and for how long (5 s)

typedef struct {
    uint8_t b[6]; // little endian: b[0] is the last byte of AA:BB:CC:DD:EE:FF
} __attribute__((packed)) l2t_bdaddr;

struct l2t_sockaddr {
    sa_family_t l2_family;
    uint16_t l2_psm; // little endian
    l2t_bdaddr l2_bdaddr;
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
};

struct l2t_security {
    uint8_t level;
    uint8_t key_size;
};

static uint16_t l2t_le16(uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return (uint16_t) ((v >> 8) | (v << 8));
#endif
}

struct L2capTunnel {
    int fd;
    guint watch;
    guint16 psm;
    L2capTunnelCallbacks cb;
    gpointer user_data;
    GList *conns; // L2capConn*
    gsize high, low;
};

struct L2capConn {
    L2capTunnel *tunnel;
    int ch;
    char address[18];
    guint16 mtu;
    guint in_watch;     // channel readable
    guint out_watch;    // channel writable again (credits) while the queue waits
    guint flush_idle;   // queued bytes to send from the main loop
    guint drain_source; // closing: waiting for the kernel to send its part
    guint drain_tries;
    GByteArray *queue;
    gboolean above_high; // the queue went over the high water mark: call writable once below low
    gboolean closing;
    gboolean dead;
    gint refs;
    gpointer user_data;
};

static void conn_unref(L2capConn *conn) {
    if (--conn->refs > 0) return;
    g_byte_array_free(conn->queue, TRUE);
    g_free(conn);
}

static void conn_remove_sources(L2capConn *conn) {
    if (conn->in_watch != 0) g_source_remove(conn->in_watch);
    if (conn->out_watch != 0) g_source_remove(conn->out_watch);
    if (conn->flush_idle != 0) g_source_remove(conn->flush_idle);
    if (conn->drain_source != 0) g_source_remove(conn->drain_source);
    conn->in_watch = conn->out_watch = conn->flush_idle = conn->drain_source = 0;
}

// The end of a connection: close the channel, tell the program, and let go of the handle.
static void conn_teardown(L2capConn *conn, const char *reason, gboolean notify) {
    if (conn->dead) return;
    conn->dead = TRUE;
    conn_remove_sources(conn);
    close(conn->ch);
    conn->ch = -1;
    L2capTunnel *tunnel = conn->tunnel;
    tunnel->conns = g_list_remove(tunnel->conns, conn);
    if (notify && tunnel->cb.disconnected != NULL) tunnel->cb.disconnected(conn, reason, tunnel->user_data);
    conn_unref(conn);
}

// Bytes the kernel holds for this channel and hasn't handed to the controller yet (SDUs
// waiting for credits). TIOCOUTQ on a Bluetooth socket is the free send buffer space.
static int conn_unsent(const L2capConn *conn) {
    int sndbuf = 0, space = 0;
    socklen_t len = sizeof(sndbuf);
    if (getsockopt(conn->ch, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) < 0 || ioctl(conn->ch, TIOCOUTQ, &space) < 0) {
        return 0;
    }
    return sndbuf > space ? sndbuf - space : 0;
}

static gboolean on_drain(gpointer data) {
    L2capConn *conn = data;
    if (conn_unsent(conn) > 0 && ++conn->drain_tries < L2T_DRAIN_TRIES) return G_SOURCE_CONTINUE;
    conn->drain_source = 0;
    conn_teardown(conn, conn->drain_tries < L2T_DRAIN_TRIES ? "closed" : "closed, queue not drained", TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean on_writable(gint fd, GIOCondition condition, gpointer data);

static void conn_flush(L2capConn *conn) {
    conn->refs++; // the writable callback may close it
    while (conn->queue->len > 0) {
        const gsize len = MIN(conn->queue->len, conn->mtu);
        const ssize_t n = send(conn->ch, conn->queue->data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Out of credits: the central hasn't taken the earlier SDUs yet.
                if (conn->out_watch == 0) conn->out_watch = g_unix_fd_add(conn->ch, G_IO_OUT, on_writable, conn);
                break;
            }
            conn_teardown(conn, g_strerror(errno), TRUE);
            break;
        }
        g_byte_array_remove_range(conn->queue, 0, (guint) n);
    }
    if (!conn->dead && conn->above_high && conn->queue->len < conn->tunnel->low) {
        conn->above_high = FALSE;
        if (conn->tunnel->cb.writable != NULL && !conn->closing) conn->tunnel->cb.writable(conn, conn->tunnel->user_data);
    }
    if (!conn->dead && conn->closing && conn->queue->len == 0 && conn->drain_source == 0) {
        // All bytes are in the kernel; close once it has sent them.
        conn->drain_source = g_timeout_add(L2T_DRAIN_MS, on_drain, conn);
    }
    conn_unref(conn);
}

static gboolean on_writable(gint fd, GIOCondition condition, gpointer data) {
    L2capConn *conn = data;
    conn->out_watch = 0;
    conn_flush(conn);
    return G_SOURCE_REMOVE;
}

static gboolean on_flush_idle(gpointer data) {
    L2capConn *conn = data;
    conn->flush_idle = 0;
    conn_flush(conn);
    return G_SOURCE_REMOVE;
}

static void conn_schedule_flush(L2capConn *conn) {
    // While out_watch is set, the kernel has no credits; it calls conn_flush() when it has.
    if (conn->flush_idle == 0 && conn->out_watch == 0) conn->flush_idle = g_idle_add(on_flush_idle, conn);
}

static gboolean on_readable(gint fd, GIOCondition condition, gpointer data) {
    L2capConn *conn = data;
    guint8 buf[L2T_RCV_MTU];
    const ssize_t n = recv(conn->ch, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        conn->in_watch = 0;
        conn_teardown(conn, n == 0 ? "channel closed by the central" : g_strerror(errno), TRUE);
        return G_SOURCE_REMOVE;
    }
    if (conn->closing || conn->tunnel->cb.rx == NULL) return G_SOURCE_CONTINUE; // read on to see the close
    conn->refs++;
    conn->tunnel->cb.rx(conn, buf, (gsize) n, conn->tunnel->user_data);
    const gboolean alive = !conn->dead;
    conn_unref(conn);
    return alive ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean on_accept(gint fd, GIOCondition condition, gpointer data) {
    L2capTunnel *tunnel = data;
    struct l2t_sockaddr peer = {0};
    socklen_t len = sizeof(peer);
    const int ch = accept4(fd, (struct sockaddr *) &peer, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (ch < 0) return G_SOURCE_CONTINUE; // EAGAIN, or the central gave up already
    char address[18];
    const uint8_t *b = peer.l2_bdaddr.b;
    snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
    if (tunnel->cb.accept != NULL && !tunnel->cb.accept(tunnel, address, tunnel->user_data)) {
        close(ch);
        return G_SOURCE_CONTINUE;
    }
    L2capConn *conn = g_new0(L2capConn, 1);
    conn->tunnel = tunnel;
    conn->ch = ch;
    g_strlcpy(conn->address, address, sizeof(conn->address));
    uint16_t mtu = 0;
    socklen_t mlen = sizeof(mtu);
    conn->mtu = getsockopt(ch, L2T_SOL_BLUETOOTH, L2T_BT_SNDMTU, &mtu, &mlen) == 0 && mtu > 0 ? mtu : 23;
    conn->queue = g_byte_array_new();
    conn->refs = 1;
    conn->in_watch = g_unix_fd_add(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_readable, conn);
    tunnel->conns = g_list_append(tunnel->conns, conn);
    if (tunnel->cb.connected != NULL) tunnel->cb.connected(conn, tunnel->user_data);
    return G_SOURCE_CONTINUE;
}

L2capTunnel *l2cap_tunnel_new(guint16 psm, L2capTunnelSecurity security, const L2capTunnelCallbacks *callbacks,
                              gpointer user_data) {
    if (psm != 0 && (psm < 0x80 || psm > 0xff)) {
        errno = EINVAL; // LE dynamic PSMs
        return NULL;
    }
    const int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, L2T_BTPROTO_L2CAP);
    if (fd < 0) return NULL;
    struct l2t_security sec = {.level = (uint8_t) security};
    struct l2t_sockaddr addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = l2t_le16(psm); // every adapter (BDADDR_ANY); 0: the kernel picks a PSM
    addr.l2_bdaddr_type = L2T_BDADDR_LE_PUBLIC;
    uint16_t mtu = L2T_RCV_MTU;
    struct l2t_sockaddr bound = {0};
    socklen_t len = sizeof(bound);
    // The kernel only takes BT_RCVMTU once the socket is bound to an LE address type.
    if (setsockopt(fd, L2T_SOL_BLUETOOTH, L2T_BT_SECURITY, &sec, sizeof(sec)) < 0 ||
        bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
        setsockopt(fd, L2T_SOL_BLUETOOTH, L2T_BT_RCVMTU, &mtu, sizeof(mtu)) < 0 || listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *) &bound, &len) < 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    L2capTunnel *tunnel = g_new0(L2capTunnel, 1);
    tunnel->fd = fd;
    tunnel->psm = l2t_le16(bound.l2_psm);
    if (callbacks != NULL) tunnel->cb = *callbacks;
    tunnel->user_data = user_data;
    tunnel->high = L2T_DEFAULT_HIGH;
    tunnel->low = L2T_DEFAULT_LOW;
    tunnel->watch = g_unix_fd_add(fd, G_IO_IN, on_accept, tunnel);
    return tunnel;
}

void l2cap_tunnel_free(L2capTunnel *tunnel) {
    if (tunnel == NULL) return;
    g_source_remove(tunnel->watch);
    close(tunnel->fd);
    while (tunnel->conns != NULL) conn_teardown(tunnel->conns->data, "tunnel freed", TRUE);
    g_free(tunnel);
}

guint16 l2cap_tunnel_get_psm(const L2capTunnel *tunnel) {
    return tunnel->psm;
}

guint l2cap_tunnel_get_count(const L2capTunnel *tunnel) {
    return g_list_length(tunnel->conns);
}

void l2cap_tunnel_set_water_marks(L2capTunnel *tunnel, gsize high, gsize low) {
    tunnel->high = MAX(high, 1);
    tunnel->low = MIN(low, tunnel->high);
}

gboolean l2cap_conn_send(L2capConn *conn, const guint8 *data, gsize len) {
    if (conn->dead || conn->closing) return FALSE;
    g_byte_array_append(conn->queue, data, (guint) len);
    if (conn->queue->len >= conn->tunnel->high) conn->above_high = TRUE;
    conn_schedule_flush(conn);
    return TRUE;
}

void l2cap_conn_close(L2capConn *conn) {
    if (conn->dead || conn->closing) return;
    conn->closing = TRUE;
    conn_schedule_flush(conn); // starts the drain once the queue is empty
}

gsize l2cap_conn_get_queued(const L2capConn *conn) {
    return conn->queue->len;
}

guint16 l2cap_conn_get_mtu(const L2capConn *conn) {
    return conn->mtu;
}

const char *l2cap_conn_get_address(const L2capConn *conn) {
    return conn->address;
}

L2capTunnel *l2cap_conn_get_tunnel(const L2capConn *conn) {
    return conn->tunnel;
}

void l2cap_conn_set_user_data(L2capConn *conn, gpointer data) {
    conn->user_data = data;
}

gpointer l2cap_conn_get_user_data(const L2capConn *conn) {
    return conn->user_data;
}
