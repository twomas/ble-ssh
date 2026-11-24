/*
 * ble_ssh_server - expose a local TCP service (sshd by default) over a BLE GATT tunnel.
 *
 * Every connected central that enables notifications on the TX characteristic gets its
 * own session with its own TCP connection to sshd, so several clients can be logged in
 * at the same time.
 *
 * With --l2cap the server also listens on an LE L2CAP channel (credit based flow control) and
 * publishes its PSM in a GATT characteristic: clients that can open such a channel use it, and
 * every channel is a session of its own (see ble_ssh_l2cap.h). GATT sessions keep working.
 *
 * With --pty CMD (prototype) every session runs CMD (normally /bin/login) on a pseudo terminal
 * of its own instead of connecting to sshd: a serial console over BLE, which any terminal app
 * can use, e.g. on a phone with --nus. The BLE link is then the only protection, so --pty
 * needs --require-bonding.
 *
 * Usage: ble_ssh_server [--port 22] [--host 127.0.0.1] [--name NAME | --adapter-name] [--max-clients 4]
 *                       [--no-advertise] [--no-power-on] [--require-bonding] [--l2cap [--psm N]]
 *                       [--pty CMD] [--nus] [--verbose]
 */

#define _GNU_SOURCE // accept4

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <netdb.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "adapter.h"
#include "advertisement.h"
#include "application.h"
#include "device.h"
#include "logger.h"

#include "ble_ssh.h"
#include "ble_ssh_l2cap.h"
#include "sd_notify.h"

#define TX_HIGH_WATER (32 * 1024)
#define TX_LOW_WATER (8 * 1024)
#define TX_RETRY_MS 3
#define READ_CHUNK 4096

typedef struct {
    gchar *address;     // central's address, upper case
    int fd;             // TCP connection to sshd, or the pty's master with --pty
    GPid pid;           // with --pty: the program on the pty
    guint read_watch;   // fd watch, 0 while paused for back-pressure
    guint retry_source; // pending TX retry timer
    guint16 mtu;
    GByteArray *tx;     // bytes from sshd not yet notified to the central
    gboolean backend_eof; // sshd closed: end the session once tx is sent
    guint64 bytes_in;
    guint64 bytes_out;
} Session;

static gchar *opt_host = NULL;
static gint opt_port = 22;
static gchar *opt_name = NULL;
static gint opt_max_clients = 4;
static gboolean opt_verbose = FALSE;
static gboolean opt_no_advertise = FALSE;
static gboolean opt_no_power_on = FALSE;
static gboolean opt_require_bonding = FALSE;
static gboolean opt_adapter_name = FALSE;
static gboolean opt_l2cap = FALSE;
static gint opt_psm = 0;
static gchar *opt_pty = NULL;
static gboolean opt_pty_insecure = FALSE;
static gboolean opt_nus = FALSE;
static gchar **pty_argv = NULL;

// The tunnel's service and characteristics: ble-ssh's own, or with --nus the Nordic UART Service.
static const char *svc_uuid = BLE_SSH_SERVICE_UUID;
static const char *rx_uuid = BLE_SSH_RX_UUID;
static const char *tx_uuid = BLE_SSH_TX_UUID;

// A session over an L2CAP channel: the channel is the session, so it starts at accept() and
// ends when either side closes. No GATT involved.
typedef struct {
    gchar *address;      // central's address, upper case
    int ch;              // the L2CAP channel
    int fd;              // TCP connection to sshd, or the pty's master with --pty
    GPid pid;            // with --pty: the program on the pty
    guint ch_watch;      // channel readable
    guint out_watch;     // channel writable again, while to_ch waits for credits
    guint fd_watch;      // sshd readable, 0 while paused for back-pressure
    guint16 sndmtu;      // largest SDU the central accepts
    GByteArray *to_ch;   // bytes from sshd not yet sent on the channel
    gboolean backend_eof; // sshd closed: end the session once to_ch is sent
    guint drain_source;  // after backend_eof: waiting for the kernel to send its queue
    guint drain_tries;
    guint64 bytes_in;
    guint64 bytes_out;
} L2Session;

static GMainLoop *loop = NULL;
static Adapter *adapter = NULL;
static Advertisement *advertisement = NULL;
static Application *app = NULL;
static GHashTable *sessions = NULL; // address -> Session*
static GList *l2_sessions = NULL;   // L2Session*
static int l2_listen_fd = -1;
static guint l2_listen_watch = 0;
static guint16 l2_psm = 0;
static int exit_code = 0;

static guint active_sessions(void) {
    return (sessions != NULL ? g_hash_table_size(sessions) : 0) + g_list_length(l2_sessions);
}

static void notify_status(void) {
    const guint active = active_sessions();
    gchar *l2 = opt_l2cap ? g_strdup_printf(" (%u over L2CAP)", g_list_length(l2_sessions)) : g_strdup("");
    sd_notify_printf("STATUS=%u/%d client%s connected%s%s", active, opt_max_clients, opt_max_clients == 1 ? "" : "s",
                     l2, opt_no_advertise ? " (not advertising)" : "");
    g_free(l2);
}

static void session_close(Session *session, const char *reason, gboolean disconnect_central);
static void session_end(Session *session, const char *reason);
static void session_flush_tx(Session *session);

static void msg(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static gsize chunk_size(const Session *session) {
    gsize size = session->mtu > BLE_SSH_ATT_OVERHEAD ? session->mtu - BLE_SSH_ATT_OVERHEAD : BLE_SSH_MIN_CHUNK;
    return CLAMP(size, BLE_SSH_MIN_CHUNK, BLE_SSH_MAX_CHUNK);
}

static int connect_backend(void) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    gchar *port = g_strdup_printf("%d", opt_port);
    int rc = getaddrinfo(opt_host, port, &hints, &res);
    g_free(port);
    if (rc != 0) {
        msg("getaddrinfo %s: %s", opt_host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// --pty: the program ends; reap it (a session that is still open sees EOF on the master).
static void on_pty_child_exited(GPid pid, gint status, gpointer user_data) {
    if (opt_verbose) {
        if (WIFEXITED(status)) msg("pty program %d exited with %d", (int) pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) msg("pty program %d ended by signal %d", (int) pid, WTERMSIG(status));
    }
    g_spawn_close_pid(pid);
}

// --pty: start the program on a new pseudo terminal; returns the master, which the session
// reads and writes like the connection to sshd.
static int spawn_pty(GPid *pid_out) {
    struct winsize size = {.ws_row = 24, .ws_col = 80}; // a client has no way to send its own yet
    int master = -1;
    const pid_t pid = forkpty(&master, NULL, NULL, &size);
    if (pid < 0) return -1;
    if (pid == 0) {
        // The child: a session of its own with the pty as its controlling terminal (forkpty
        // did that). Ignored signals stay ignored across exec: SIGPIPE, which the server
        // ignores, and SIGHUP when it was started with nohup, would leave the program
        // running after its session ends. Start it with default signals and none blocked.
        const int defaults[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU};
        for (gsize i = 0; i < G_N_ELEMENTS(defaults); i++) signal(defaults[i], SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setenv("TERM", "vt100", 1); // what serial terminal apps understand
        execvp(pty_argv[0], pty_argv);
        fprintf(stderr, "ble_ssh_server: cannot run %s: %s\r\n", pty_argv[0], strerror(errno));
        _exit(127);
    }
    g_unix_set_fd_nonblocking(master, FALSE, NULL);
    (void) fcntl(master, F_SETFD, FD_CLOEXEC);
    g_child_watch_add(pid, on_pty_child_exited, NULL);
    *pid_out = pid;
    return master;
}

// The other end of a session: sshd (or --host/--port), or with --pty a new program on a pty.
static int open_backend(GPid *pid) {
    *pid = 0;
    return opt_pty != NULL ? spawn_pty(pid) : connect_backend();
}

static void close_backend(int fd, GPid pid) {
    if (fd >= 0) close(fd);
    // Closing the master hangs the terminal up, which sends SIGHUP to its foreground jobs;
    // send it to the program's process group as well, in case it ignores the hangup.
    if (pid > 0) kill(-pid, SIGHUP);
}

// read() on the backend, with the end of a pty (EIO once the program has exited) as EOF.
static ssize_t read_backend(int fd, void *buf, size_t len) {
    const ssize_t n = read(fd, buf, len);
    return (n < 0 && errno == EIO && opt_pty != NULL) ? 0 : n;
}

static gboolean write_all(int fd, const guint8 *data, gsize len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return FALSE;
        }
        data += n;
        len -= (gsize) n;
    }
    return TRUE;
}

static gboolean on_backend_readable(gint fd, GIOCondition condition, gpointer user_data) {
    Session *session = user_data;
    guint8 buf[READ_CHUNK];
    ssize_t n = read_backend(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        session->read_watch = 0;
        if (n == 0 && session->tx->len > 0) {
            // sshd's last bytes are still queued (its final packets): send them first;
            // session_flush_tx() ends the session once they are out.
            session->backend_eof = TRUE;
            return G_SOURCE_REMOVE;
        }
        session_end(session, n == 0 ? "backend closed" : g_strerror(errno)); // keeps the link for SSH
        return G_SOURCE_REMOVE;
    }
    session->bytes_in += (guint64) n;
    g_byte_array_append(session->tx, buf, (guint) n);
    if (session->tx->len >= TX_HIGH_WATER) {
        // Pause reading from sshd until the BLE side has caught up.
        session->read_watch = 0;
        session_flush_tx(session);
        return G_SOURCE_REMOVE;
    }
    session_flush_tx(session);
    return G_SOURCE_CONTINUE;
}

static void session_resume_reading(Session *session) {
    if (session->read_watch == 0 && session->fd >= 0 && !session->backend_eof && session->tx->len < TX_LOW_WATER) {
        session->read_watch = g_unix_fd_add(session->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_backend_readable, session);
    }
}

static gboolean on_tx_retry(gpointer user_data) {
    Session *session = user_data;
    session->retry_source = 0;
    session_flush_tx(session);
    return G_SOURCE_REMOVE;
}

static void session_flush_tx(Session *session) {
    while (session->tx->len > 0) {
        const gsize len = MIN(session->tx->len, chunk_size(session));
        GByteArray chunk = {.data = session->tx->data, .len = (guint) len};
        const int result = binc_application_notify_device(app, session->address, svc_uuid,
                                                          tx_uuid, &chunk);
        if (result == 0) {
            g_byte_array_remove_range(session->tx, 0, (guint) len);
            session->bytes_out += len;
            continue;
        }
        if (result == EAGAIN || result == EWOULDBLOCK || result == ENOBUFS) {
            if (session->retry_source == 0) {
                session->retry_source = g_timeout_add(TX_RETRY_MS, on_tx_retry, session);
            }
            return;
        }
        if (result == EMSGSIZE && session->mtu > BLE_SSH_MIN_CHUNK + BLE_SSH_ATT_OVERHEAD) {
            session->mtu = BLE_SSH_MIN_CHUNK + BLE_SSH_ATT_OVERHEAD;
            continue;
        }
        session_close(session, g_strerror(result), FALSE);
        return;
    }
    if (session->backend_eof) {
        session_end(session, "backend closed");
        return;
    }
    session_resume_reading(session);
}

static Session *session_open(const char *address, guint16 mtu) {
    if ((gint) active_sessions() >= opt_max_clients) {
        msg("%s: rejected, %d clients already connected", address, opt_max_clients);
        return NULL;
    }
    // The TCP connection to sshd is made on the client's first write, not here: a client may
    // subscribe briefly without sending anything (ours toggle on -> off -> on to get a clean
    // session on a reused link), and sshd's banner from such a throwaway session would land
    // in the next one. SSH clients always send their banner right away, so nothing is lost.
    Session *session = g_new0(Session, 1);
    session->address = g_ascii_strup(address, -1);
    session->fd = -1;
    session->mtu = mtu;
    session->tx = g_byte_array_new();
    g_hash_table_insert(sessions, session->address, session);
    msg("%s: session opened (mtu %u, %u active)", session->address, mtu, active_sessions());
    notify_status();
    return session;
}

static gboolean session_connect_backend(Session *session) {
    if (session->fd >= 0) return TRUE;
    session->fd = open_backend(&session->pid);
    if (session->fd < 0) {
        if (opt_pty != NULL) msg("%s: cannot start %s: %s", session->address, opt_pty, g_strerror(errno));
        else msg("%s: cannot connect to %s:%d: %s", session->address, opt_host, opt_port, g_strerror(errno));
        return FALSE;
    }
    session_resume_reading(session);
    return TRUE;
}

static void session_close(Session *session, const char *reason, gboolean disconnect_central) {
    msg("%s: session closed (%s), %" G_GUINT64_FORMAT " bytes in, %" G_GUINT64_FORMAT " bytes out",
        session->address, reason, session->bytes_in, session->bytes_out);
    if (session->read_watch != 0) g_source_remove(session->read_watch);
    if (session->retry_source != 0) g_source_remove(session->retry_source);
    close_backend(session->fd, session->pid);
    if (disconnect_central && adapter != NULL) {
        // The tunnel has no in-band EOF, so dropping the link tells the client we are done.
        Device *device = binc_adapter_get_device_by_address(adapter, session->address);
        if (device != NULL && binc_device_get_connection_state(device) != BINC_DISCONNECTED) {
            binc_device_disconnect(device);
        }
    }
    g_hash_table_remove(sessions, session->address); // frees session
    notify_status();
}

static void session_free(gpointer data) {
    Session *session = data;
    g_byte_array_free(session->tx, TRUE);
    g_free(session->address);
    g_free(session);
}

static Session *session_lookup(const char *address) {
    gchar *key = g_ascii_strup(address, -1);
    Session *session = g_hash_table_lookup(sessions, key);
    g_free(key);
    return session;
}

#define PTY_END_DISCONNECT_MS 500 // lets the end marker go out before the link drops

static gboolean on_pty_end_timeout(gpointer data) {
    gchar *address = data;
    Device *device = adapter != NULL ? binc_adapter_get_device_by_address(adapter, address) : NULL;
    if (device != NULL && binc_device_get_connection_state(device) == BINC_CONNECTED && session_lookup(address) == NULL) {
        binc_device_disconnect(device);
    }
    g_free(address);
    return G_SOURCE_REMOVE;
}

// The backend ended. SSH ends its session in-band, so keep the link: the client may reuse it
// for its next connection (Windows does). A program on a pty has no such end, and GATT has no
// EOF: send the end marker and drop the link, which is how the client learns of it.
static void session_end(Session *session, const char *reason) {
    if (opt_pty == NULL) {
        session_close(session, reason, FALSE);
        return;
    }
    GByteArray marker = {.data = (guint8 *) BLE_SSH_PTY_END_MARKER, .len = sizeof(BLE_SSH_PTY_END_MARKER) - 1};
    binc_application_notify_device(app, session->address, svc_uuid, tx_uuid, &marker);
    gchar *address = g_strdup(session->address);
    session_close(session, reason, FALSE);
    g_timeout_add(PTY_END_DISCONNECT_MS, on_pty_end_timeout, address);
}

// With --require-bonding the characteristics already demand an encrypted link, but an
// encrypted link can also come from a pairing that wasn't stored. Only accept centrals that
// are bonded, i.e. that the program owning the agent and pairable setting let in for good.
static gboolean central_is_bonded(const char *address) {
    Device *device = binc_adapter_get_device_by_address(adapter, address);
    return device != NULL && binc_device_get_bonded(device); // Bonded on BlueZ >= 5.72, else Paired
}

// ---------------------------------------------------------------------------------------
// L2CAP channels (--l2cap)

static gboolean l2_flush(L2Session *s);

static void l2_close(L2Session *s, const char *reason) {
    msg("%s: L2CAP session closed (%s), %" G_GUINT64_FORMAT " bytes in, %" G_GUINT64_FORMAT " bytes out", s->address,
        reason, s->bytes_in, s->bytes_out);
    if (s->ch_watch != 0) g_source_remove(s->ch_watch);
    if (s->out_watch != 0) g_source_remove(s->out_watch);
    if (s->fd_watch != 0) g_source_remove(s->fd_watch);
    if (s->drain_source != 0) g_source_remove(s->drain_source);
    close(s->ch);
    close_backend(s->fd, s->pid);
    l2_sessions = g_list_remove(l2_sessions, s);
    g_byte_array_free(s->to_ch, TRUE);
    g_free(s->address);
    g_free(s);
    notify_status();
}

static gboolean on_l2_backend_readable(gint fd, GIOCondition condition, gpointer user_data) {
    L2Session *s = user_data;
    guint8 buf[READ_CHUNK];
    ssize_t n = read_backend(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        s->fd_watch = 0;
        if (n == 0) {
            // l2_flush() sends what is left, then waits for the kernel to send it, then closes.
            s->backend_eof = TRUE;
            if (opt_pty != NULL) {
                g_byte_array_append(s->to_ch, (const guint8 *) BLE_SSH_PTY_END_MARKER,
                                    sizeof(BLE_SSH_PTY_END_MARKER) - 1);
            }
            l2_flush(s);
            return G_SOURCE_REMOVE;
        }
        l2_close(s, g_strerror(errno));
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(s->to_ch, buf, (guint) n);
    if (s->to_ch->len >= TX_HIGH_WATER) {
        // Pause reading from sshd until the channel has credits again.
        s->fd_watch = 0;
        l2_flush(s);
        return G_SOURCE_REMOVE;
    }
    return l2_flush(s) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean on_l2_writable(gint fd, GIOCondition condition, gpointer user_data) {
    L2Session *s = user_data;
    s->out_watch = 0;
    l2_flush(s);
    return G_SOURCE_REMOVE;
}

static gboolean on_l2_drain(gpointer user_data) {
    L2Session *s = user_data;
    if (ble_ssh_l2cap_unsent(s->ch) > 0 && ++s->drain_tries < BLE_SSH_L2CAP_DRAIN_TRIES) return G_SOURCE_CONTINUE;
    s->drain_source = 0;
    l2_close(s, s->drain_tries < BLE_SSH_L2CAP_DRAIN_TRIES ? "backend closed" : "backend closed, channel not drained");
    return G_SOURCE_REMOVE;
}

// Send what sshd gave us, one SDU per send(). Returns FALSE if the session was closed.
static gboolean l2_flush(L2Session *s) {
    while (s->to_ch->len > 0) {
        const gsize len = MIN(s->to_ch->len, s->sndmtu);
        const ssize_t n = send(s->ch, s->to_ch->data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Out of credits: the central hasn't taken the earlier SDUs yet.
                if (s->out_watch == 0) s->out_watch = g_unix_fd_add(s->ch, G_IO_OUT, on_l2_writable, s);
                return TRUE;
            }
            l2_close(s, g_strerror(errno));
            return FALSE;
        }
        g_byte_array_remove_range(s->to_ch, 0, (guint) n);
        s->bytes_out += (guint64) n;
    }
    if (s->backend_eof) {
        // All of sshd's bytes are in the kernel now; close once it has sent them.
        if (s->drain_source == 0) s->drain_source = g_timeout_add(BLE_SSH_L2CAP_DRAIN_MS, on_l2_drain, s);
        return TRUE;
    }
    if (s->fd_watch == 0 && s->to_ch->len < TX_LOW_WATER) {
        s->fd_watch = g_unix_fd_add(s->fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_l2_backend_readable, s);
    }
    return TRUE;
}

static gboolean on_l2_channel_readable(gint ch, GIOCondition condition, gpointer user_data) {
    L2Session *s = user_data;
    guint8 buf[BLE_SSH_L2CAP_MTU];
    const ssize_t n = recv(ch, buf, sizeof(buf), MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        s->ch_watch = 0;
        l2_close(s, n == 0 ? "channel closed" : g_strerror(errno));
        return G_SOURCE_REMOVE;
    }
    s->bytes_in += (guint64) n;
    if (!write_all(s->fd, buf, (gsize) n)) {
        s->ch_watch = 0;
        l2_close(s, g_strerror(errno));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_l2_accept(gint lfd, GIOCondition condition, gpointer user_data) {
    struct ble_ssh_sockaddr_l2 peer = {0};
    socklen_t len = sizeof(peer);
    const int ch = accept4(lfd, (struct sockaddr *) &peer, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (ch < 0) {
        if (errno != EAGAIN && errno != EINTR) msg("L2CAP accept: %s", g_strerror(errno));
        return G_SOURCE_CONTINUE;
    }
    char address[18];
    ble_ssh_ba2str(&peer.l2_bdaddr, address);
    if ((gint) active_sessions() >= opt_max_clients) {
        msg("%s: L2CAP channel rejected, %d clients already connected", address, opt_max_clients);
        close(ch);
        return G_SOURCE_CONTINUE;
    }
    // The listening socket demands an encrypted link with --require-bonding; as for GATT, also
    // demand that the central is bonded, not just paired for this link.
    if (opt_require_bonding && !central_is_bonded(address)) {
        msg("%s: L2CAP channel rejected, not bonded (--require-bonding)", address);
        close(ch);
        return G_SOURCE_CONTINUE;
    }
    GPid pid = 0;
    const int fd = open_backend(&pid);
    if (fd < 0) {
        if (opt_pty != NULL) msg("%s: cannot start %s: %s", address, opt_pty, g_strerror(errno));
        else msg("%s: cannot connect to %s:%d: %s", address, opt_host, opt_port, g_strerror(errno));
        close(ch);
        return G_SOURCE_CONTINUE;
    }
    L2Session *s = g_new0(L2Session, 1);
    s->address = g_strdup(address);
    s->ch = ch;
    s->fd = fd;
    s->pid = pid;
    s->sndmtu = ble_ssh_l2cap_sndmtu(ch);
    s->to_ch = g_byte_array_new();
    s->ch_watch = g_unix_fd_add(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_l2_channel_readable, s);
    s->fd_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_l2_backend_readable, s);
    l2_sessions = g_list_append(l2_sessions, s);
    msg("%s: L2CAP session opened (send mtu %u, %u active)", s->address, s->sndmtu, active_sessions());
    notify_status();
    return G_SOURCE_CONTINUE;
}

static gboolean l2_listen(void) {
    const uint8_t security = opt_require_bonding ? BLE_SSH_BT_SECURITY_MEDIUM : BLE_SSH_BT_SECURITY_LOW;
    // opt_psm 0: the kernel picks a free one, 0x80-0xff.
    const int fd = ble_ssh_l2cap_socket(SOCK_NONBLOCK, security, (uint16_t) opt_psm);
    if (fd < 0) {
        msg("L2CAP socket on PSM %d: %s", opt_psm, g_strerror(errno));
        return FALSE;
    }
    if (listen(fd, 4) < 0) {
        msg("L2CAP listen on PSM %d: %s", opt_psm, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    struct ble_ssh_sockaddr_l2 bound = {0};
    socklen_t len = sizeof(bound);
    if (getsockname(fd, (struct sockaddr *) &bound, &len) < 0) {
        msg("L2CAP getsockname: %s", g_strerror(errno));
        close(fd);
        return FALSE;
    }
    l2_psm = ble_ssh_htobs(bound.l2_psm);
    l2_listen_fd = fd;
    l2_listen_watch = g_unix_fd_add(fd, G_IO_IN, on_l2_accept, NULL);
    return TRUE;
}

// ---------------------------------------------------------------------------------------

static void open_or_reject(const char *address, guint16 mtu) {
    if (session_open(address, mtu) == NULL) {
        Device *device = binc_adapter_get_device_by_address(adapter, address);
        if (device != NULL) binc_device_disconnect(device);
    }
}

// A fresh pairing can still be completing when the central subscribes: the link is already
// encrypted, but bluetoothd hasn't reported Bonded yet. Give it a moment before rejecting.
#define BOND_WAIT_INTERVAL_MS 200
#define BOND_WAIT_TRIES 25

typedef struct {
    gchar *address;
    guint16 mtu;
    guint tries;
} BondWait;

static gboolean on_bond_wait(gpointer data) {
    BondWait *wait = data;
    Device *device = binc_adapter_get_device_by_address(adapter, wait->address);
    const gboolean connected = device != NULL && binc_device_get_connection_state(device) == BINC_CONNECTED;
    if (connected && central_is_bonded(wait->address)) {
        open_or_reject(wait->address, wait->mtu);
    } else if (connected && ++wait->tries < BOND_WAIT_TRIES) {
        return G_SOURCE_CONTINUE;
    } else if (connected) {
        msg("%s: rejected, not bonded (--require-bonding)", wait->address);
        binc_device_disconnect(device);
    }
    g_free(wait->address);
    g_free(wait);
    return G_SOURCE_REMOVE;
}

static void on_notify_state(const Application *application, const char *address, const char *service_uuid,
                            const char *char_uuid, gboolean notifying, guint16 mtu) {
    if (!g_str_equal(char_uuid, tx_uuid)) return;
    Session *session = session_lookup(address);
    if (notifying) {
        if (session != NULL) session_close(session, "client restarted notifications", FALSE);
        if (opt_require_bonding && !central_is_bonded(address)) {
            BondWait *wait = g_new0(BondWait, 1);
            wait->address = g_strdup(address);
            wait->mtu = mtu;
            g_timeout_add(BOND_WAIT_INTERVAL_MS, on_bond_wait, wait);
            return;
        }
        open_or_reject(address, mtu);
    } else if (session != NULL) {
        session_close(session, "notifications stopped", FALSE);
    }
}

static const char *on_char_write(const Application *application, const char *address, const char *service_uuid,
                                 const char *char_uuid, GByteArray *byteArray, const guint16 mtu,
                                 const guint16 offset) {
    if (!g_str_equal(char_uuid, rx_uuid)) return BLUEZ_ERROR_REJECTED;
    Session *session = session_lookup(address);
    if (session == NULL) {
        // A session is exactly one TX subscription (notifications on -> off). Never start one
        // from a write: those bytes are the middle of some other SSH stream.
        return BLUEZ_ERROR_NOT_PERMITTED;
    }
    if (mtu > session->mtu) session->mtu = mtu;
    if (!session_connect_backend(session)) {
        session_close(session, "backend unavailable", FALSE);
        return BLUEZ_ERROR_FAILED;
    }
    if (!write_all(session->fd, byteArray->data, byteArray->len)) {
        session_close(session, g_strerror(errno), FALSE);
        return BLUEZ_ERROR_FAILED;
    }
    return NULL;
}

static const char *on_char_read(const Application *application, const char *address, const char *service_uuid,
                                const char *char_uuid, const guint16 mtu, const guint16 offset) {
    // The PSM's value is set once in main(); returning no error serves it.
    if (g_str_equal(char_uuid, BLE_SSH_PSM_UUID)) return NULL;
    return BLUEZ_ERROR_NOT_PERMITTED;
}

static void on_powered_state_changed(Adapter *a, gboolean powered) {
    msg("adapter powered %s", powered ? "on" : "off");
    // BlueZ drops advertisements while the controller is off; put ours back when it returns.
    if (powered && advertisement != NULL) binc_adapter_start_advertising(a, advertisement);
}

static void on_alias_changed(Adapter *a, const char *alias) {
    // --adapter-name: follow the adapter's Alias, so it doesn't matter whether the program that
    // sets it starts before or after us.
    if (alias == NULL || alias[0] == '\0' || g_strcmp0(alias, opt_name) == 0) return;
    msg("adapter name changed to '%s', advertising that", alias);
    g_free(opt_name);
    opt_name = g_strdup(alias);
    if (advertisement != NULL) {
        // BlueZ reads the name when the advertisement is registered: register it again.
        binc_adapter_stop_advertising(a, advertisement);
        binc_advertisement_set_local_name(advertisement, opt_name);
        if (binc_adapter_get_powered_state(a)) binc_adapter_start_advertising(a, advertisement);
    }
}

static void on_central_state_changed(Adapter *a, Device *device) {
    const char *address = binc_device_get_address(device);
    if (opt_verbose) msg("%s: %s", address, binc_device_get_connection_state_name(device));
    if (binc_device_get_connection_state(device) == BINC_DISCONNECTED) {
        Session *session = session_lookup(address);
        if (session != NULL) session_close(session, "central disconnected", FALSE);
    }
    // Keep advertising so more clients can connect.
    if (advertisement != NULL) binc_adapter_start_advertising(a, advertisement);
}

static void add_char(const char *uuid, guint flags, const char *description) {
    binc_application_add_characteristic(app, svc_uuid, uuid, flags);
    binc_application_add_descriptor(app, svc_uuid, uuid, BLE_SSH_CUD_UUID, GATT_CHR_PROP_READ);
    GByteArray *value = g_byte_array_new();
    g_byte_array_append(value, (const guint8 *) description, (guint) strlen(description));
    binc_application_set_desc_value(app, svc_uuid, uuid, BLE_SSH_CUD_UUID, value);
    g_byte_array_free(value, TRUE);
}

static gboolean on_terminate(gpointer data) {
    msg("shutting down");
    sd_notify_send("STOPPING=1");
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void on_bluez_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    // Our GATT service and advertisement lived inside bluetoothd; they are gone now. Exit with
    // an error so the service manager restarts us once bluetoothd is back.
    msg("%s left the bus (bluetoothd stopped or crashed), exiting", name);
    sd_notify_send("STOPPING=1\nSTATUS=bluetoothd went away");
    exit_code = 1;
    g_main_loop_quit(loop);
}

static gchar *default_name(void) {
    char host[64] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') g_strlcpy(host, "linux", sizeof(host));
    return g_strdup_printf("ssh-%s", host);
}

int main(int argc, char **argv) {
    GOptionEntry entries[] = {
        {"host", 'H', 0, G_OPTION_ARG_STRING, &opt_host, "Backend host (default 127.0.0.1)", "HOST"},
        {"port", 'p', 0, G_OPTION_ARG_INT, &opt_port, "Backend TCP port (default 22)", "PORT"},
        {"name", 'n', 0, G_OPTION_ARG_STRING, &opt_name, "Advertised name (default ssh-<hostname>)", "NAME"},
        {"adapter-name", 'N', 0, G_OPTION_ARG_NONE, &opt_adapter_name,
         "Advertise the adapter's own Bluetooth name (its Alias) instead of ssh-<hostname>", NULL},
        {"max-clients", 'm', 0, G_OPTION_ARG_INT, &opt_max_clients, "Max simultaneous clients (default 4)", "N"},
        {"no-power-on", 'P', 0, G_OPTION_ARG_NONE, &opt_no_power_on,
         "Never power on the adapter; leave adapter power to another program", NULL},
        {"require-bonding", 'B', 0, G_OPTION_ARG_NONE, &opt_require_bonding,
         "Only accept bonded centrals over an encrypted link (pairing is left to the agent owner)", NULL},
        {"no-advertise", 'A', 0, G_OPTION_ARG_NONE, &opt_no_advertise,
         "Only register the GATT service; let another program on this adapter advertise", NULL},
        {"l2cap", 'L', 0, G_OPTION_ARG_NONE, &opt_l2cap,
         "Also accept sessions over LE L2CAP channels (clients that can use them do)", NULL},
        {"psm", 0, 0, G_OPTION_ARG_INT, &opt_psm, "With --l2cap: the PSM, 128-255 (default: the kernel picks one)",
         "N"},
        {"pty", 0, 0, G_OPTION_ARG_STRING, &opt_pty,
         "Run CMD (e.g. /bin/login) on a pseudo terminal per session instead of connecting to sshd; "
         "needs --require-bonding",
         "CMD"},
        {"pty-insecure", 0, 0, G_OPTION_ARG_NONE, &opt_pty_insecure,
         "Allow --pty without --require-bonding: anyone in range gets CMD. Only for a bench test", NULL},
        {"nus", 0, 0, G_OPTION_ARG_NONE, &opt_nus,
         "Offer the Nordic UART Service instead of ble-ssh's, for terminal apps (with --pty)", NULL},
        {"verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Verbose logging, including bluez_inc", NULL},
        {NULL}};
    GError *error = NULL;
    GOptionContext *context = g_option_context_new("- SSH over BLE, server side");
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        msg("%s", error->message);
        return 2;
    }
    g_option_context_free(context);
    if (opt_name != NULL && opt_adapter_name) {
        msg("--name and --adapter-name are mutually exclusive");
        return 2;
    }
    if (opt_host == NULL) opt_host = g_strdup("127.0.0.1");
    if (opt_name == NULL && !opt_adapter_name) opt_name = default_name();
    if (opt_max_clients < 1) opt_max_clients = 1;
    if (opt_psm != 0 && (opt_psm < 0x80 || opt_psm > 0xff)) {
        msg("--psm must be between 128 and 255 (LE dynamic range)");
        return 2;
    }
    if (opt_pty != NULL) {
        if (!g_shell_parse_argv(opt_pty, NULL, &pty_argv, &error)) {
            msg("--pty: %s", error->message);
            return 2;
        }
        // Without SSH, the BLE link's encryption is all there is: only bonded centrals on
        // encrypted links may reach a login prompt.
        if (!opt_require_bonding && !opt_pty_insecure) {
            msg("--pty needs --require-bonding: without SSH the BLE link is the only protection "
                "(--pty-insecure overrides this for a bench test)");
            return 2;
        }
        if (!opt_require_bonding) {
            msg("WARNING: --pty-insecure: anyone in Bluetooth range gets '%s', unencrypted", opt_pty);
        }
    } else if (opt_pty_insecure) {
        msg("--pty-insecure only goes with --pty");
        return 2;
    }
    if (opt_nus) {
        svc_uuid = BLE_SSH_NUS_SERVICE_UUID;
        rx_uuid = BLE_SSH_NUS_RX_UUID;
        tx_uuid = BLE_SSH_NUS_TX_UUID;
    }
    gchar *backend = opt_pty != NULL ? g_strdup_printf("'%s' on a pty", opt_pty)
                                     : g_strdup_printf("%s:%d", opt_host, opt_port);

    log_enabled(opt_verbose);
    log_set_level(LOG_DEBUG);
    signal(SIGPIPE, SIG_IGN);

    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (connection == NULL) {
        msg("system bus: %s", error->message);
        return 1;
    }
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_terminate, NULL);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);

    adapter = binc_adapter_get_default(connection);
    if (adapter == NULL) {
        msg("no Bluetooth adapter found");
        return 1;
    }
    if (opt_adapter_name) {
        // The name the device already goes by (bluetoothd's Alias, often set by the program
        // that owns the adapter); on_alias_changed() follows it from here on.
        const char *alias = binc_adapter_get_alias(adapter);
        if (alias != NULL && alias[0] != '\0') {
            opt_name = g_strdup(alias);
        } else {
            opt_name = default_name();
            msg("could not read the adapter's name, advertising '%s'", opt_name);
        }
        if (!opt_no_advertise) binc_adapter_set_alias_changed_cb(adapter, &on_alias_changed);
    }
    binc_adapter_set_powered_state_cb(adapter, &on_powered_state_changed);
    if (!binc_adapter_get_powered_state(adapter)) {
        if (opt_no_power_on) {
            // The GATT service registers fine while off and becomes reachable once powered.
            msg("adapter is powered off; waiting for another program to power it on");
        } else {
            binc_adapter_power_on(adapter);
        }
    }
    binc_adapter_set_remote_central_cb(adapter, &on_central_state_changed);

    sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, session_free);

    app = binc_create_application(adapter);
    binc_application_add_service(app, svc_uuid);
    const guint rx_security = opt_require_bonding ? GATT_CHR_PROP_ENCRYPT_WRITE : 0;
    const guint tx_security = opt_require_bonding ? GATT_CHR_PROP_ENCRYPT_NOTIFY : 0;
    add_char(rx_uuid, GATT_CHR_PROP_WRITE | GATT_CHR_PROP_WRITE_WITHOUT_RESP | rx_security, "ssh rx");
    add_char(tx_uuid, GATT_CHR_PROP_NOTIFY | tx_security, "ssh tx");
    if (opt_l2cap) {
        if (!l2_listen()) return 1;
        add_char(BLE_SSH_PSM_UUID, GATT_CHR_PROP_READ, "l2cap psm");
        const guint8 psm[2] = {(guint8) (l2_psm & 0xff), (guint8) (l2_psm >> 8)};
        GByteArray *value = g_byte_array_new();
        g_byte_array_append(value, psm, sizeof(psm));
        binc_application_set_char_value(app, svc_uuid, BLE_SSH_PSM_UUID, value);
        g_byte_array_free(value, TRUE);
        msg("L2CAP channels on PSM 0x%02x%s", l2_psm, opt_require_bonding ? " (encrypted links only)" : "");
    }
    binc_application_set_char_read_cb(app, &on_char_read);
    binc_application_set_char_write_cb(app, &on_char_write);
    binc_application_set_char_notify_state_cb(app, &on_notify_state);
    binc_adapter_register_application(adapter, app);

    if (opt_no_advertise) {
        // GATT services are shared by everything registered on the adapter, so a central that
        // connects through another program's advertisement still finds this service.
        msg("GATT service%s registered on %s without advertising, forwarding to %s, max %d clients",
            opt_nus ? " (Nordic UART)" : "", binc_adapter_get_address(adapter), backend, opt_max_clients);
    } else {
        GPtrArray *uuids = g_ptr_array_new();
        g_ptr_array_add(uuids, (gpointer) svc_uuid);
        advertisement = binc_advertisement_create();
        binc_advertisement_set_local_name(advertisement, opt_name);
        binc_advertisement_set_services(advertisement, uuids);
        binc_advertisement_set_interval(advertisement, 100, 300);
        binc_adapter_start_advertising(adapter, advertisement);
        g_ptr_array_free(uuids, TRUE);
        msg("advertising '%s'%s on %s, forwarding to %s, max %d clients", opt_name,
            opt_nus ? " (Nordic UART)" : "", binc_adapter_get_address(adapter), backend, opt_max_clients);
    }
    g_free(backend);

    const guint bluez_watch = g_bus_watch_name_on_connection(connection, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             NULL, on_bluez_vanished, NULL, NULL);
    if (opt_require_bonding) msg("only bonded centrals are accepted; pairing is handled by the adapter's agent");
    sd_notify_send("READY=1");
    notify_status();
    if (sd_notify_start_watchdog()) msg("systemd watchdog enabled");

    g_main_loop_run(loop);
    g_bus_unwatch_name(bluez_watch);

    GList *open = g_hash_table_get_values(sessions);
    for (GList *it = open; it != NULL; it = it->next) session_close(it->data, "server stopping", TRUE);
    g_list_free(open);
    g_hash_table_destroy(sessions);
    sessions = NULL;
    while (l2_sessions != NULL) l2_close(l2_sessions->data, "server stopping");
    if (l2_listen_watch != 0) g_source_remove(l2_listen_watch);
    if (l2_listen_fd >= 0) close(l2_listen_fd);

    if (advertisement != NULL) {
        binc_adapter_stop_advertising(adapter, advertisement);
        binc_advertisement_free(advertisement);
    }
    binc_adapter_unregister_application(adapter, app);
    binc_application_free(app);
    binc_adapter_free(adapter);
    g_main_loop_unref(loop);
    g_dbus_connection_close_sync(connection, NULL, NULL);
    g_object_unref(connection);
    g_free(opt_host);
    g_free(opt_name);
    g_free(opt_pty);
    g_strfreev(pty_argv);
    return exit_code;
}
