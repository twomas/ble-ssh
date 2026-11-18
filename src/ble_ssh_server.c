/*
 * ble_ssh_server - expose a local TCP service (sshd by default) over a BLE GATT tunnel.
 *
 * Every connected central that enables notifications on the TX characteristic gets its
 * own session with its own TCP connection to sshd, so several clients can be logged in
 * at the same time.
 *
 * Usage: ble_ssh_server [--port 22] [--host 127.0.0.1] [--name NAME] [--max-clients 4] [--no-advertise] [--verbose]
 */

#include <errno.h>
#include <glib.h>
#include <glib-unix.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "adapter.h"
#include "advertisement.h"
#include "application.h"
#include "device.h"
#include "logger.h"

#include "ble_ssh.h"
#include "sd_notify.h"

#define TX_HIGH_WATER (32 * 1024)
#define TX_LOW_WATER (8 * 1024)
#define TX_RETRY_MS 3
#define READ_CHUNK 4096

typedef struct {
    gchar *address;     // central's address, upper case
    int fd;             // TCP connection to sshd
    guint read_watch;   // fd watch, 0 while paused for back-pressure
    guint retry_source; // pending TX retry timer
    guint16 mtu;
    GByteArray *tx;     // bytes from sshd not yet notified to the central
    guint64 bytes_in;
    guint64 bytes_out;
} Session;

static gchar *opt_host = NULL;
static gint opt_port = 22;
static gchar *opt_name = NULL;
static gint opt_max_clients = 4;
static gboolean opt_verbose = FALSE;
static gboolean opt_no_advertise = FALSE;

static GMainLoop *loop = NULL;
static Adapter *adapter = NULL;
static Advertisement *advertisement = NULL;
static Application *app = NULL;
static GHashTable *sessions = NULL; // address -> Session*
static int exit_code = 0;

static void notify_status(void) {
    const guint active = sessions != NULL ? g_hash_table_size(sessions) : 0;
    sd_notify_printf("STATUS=%u/%d client%s connected%s", active, opt_max_clients, opt_max_clients == 1 ? "" : "s",
                     opt_no_advertise ? " (not advertising)" : "");
}

static void session_close(Session *session, const char *reason, gboolean disconnect_central);
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
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return G_SOURCE_CONTINUE;
    if (n <= 0) {
        session->read_watch = 0;
        session_close(session, n == 0 ? "backend closed" : g_strerror(errno), TRUE);
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
    if (session->read_watch == 0 && session->fd >= 0 && session->tx->len < TX_LOW_WATER) {
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
        const int result = binc_application_notify_device(app, session->address, BLE_SSH_SERVICE_UUID,
                                                          BLE_SSH_TX_UUID, &chunk);
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
        session_close(session, g_strerror(result), TRUE);
        return;
    }
    session_resume_reading(session);
}

static Session *session_open(const char *address, guint16 mtu) {
    if ((gint) g_hash_table_size(sessions) >= opt_max_clients) {
        msg("%s: rejected, %d clients already connected", address, opt_max_clients);
        return NULL;
    }
    const int fd = connect_backend();
    if (fd < 0) {
        msg("%s: cannot connect to %s:%d: %s", address, opt_host, opt_port, g_strerror(errno));
        return NULL;
    }
    Session *session = g_new0(Session, 1);
    session->address = g_ascii_strup(address, -1);
    session->fd = fd;
    session->mtu = mtu;
    session->tx = g_byte_array_new();
    g_hash_table_insert(sessions, session->address, session);
    session_resume_reading(session);
    msg("%s: session opened (mtu %u, %u active)", session->address, mtu, g_hash_table_size(sessions));
    notify_status();
    return session;
}

static void session_close(Session *session, const char *reason, gboolean disconnect_central) {
    msg("%s: session closed (%s), %" G_GUINT64_FORMAT " bytes in, %" G_GUINT64_FORMAT " bytes out",
        session->address, reason, session->bytes_in, session->bytes_out);
    if (session->read_watch != 0) g_source_remove(session->read_watch);
    if (session->retry_source != 0) g_source_remove(session->retry_source);
    if (session->fd >= 0) close(session->fd);
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

static void on_notify_state(const Application *application, const char *address, const char *service_uuid,
                            const char *char_uuid, gboolean notifying, guint16 mtu) {
    if (!g_str_equal(char_uuid, BLE_SSH_TX_UUID)) return;
    Session *session = session_lookup(address);
    if (notifying) {
        if (session != NULL) session_close(session, "client restarted notifications", FALSE);
        if (session_open(address, mtu) == NULL) {
            Device *device = binc_adapter_get_device_by_address(adapter, address);
            if (device != NULL) binc_device_disconnect(device);
        }
    } else if (session != NULL) {
        session_close(session, "notifications stopped", FALSE);
    }
}

static const char *on_char_write(const Application *application, const char *address, const char *service_uuid,
                                 const char *char_uuid, GByteArray *byteArray, const guint16 mtu,
                                 const guint16 offset) {
    if (!g_str_equal(char_uuid, BLE_SSH_RX_UUID)) return BLUEZ_ERROR_REJECTED;
    Session *session = session_lookup(address);
    if (session == NULL) {
        // Clients must subscribe to TX first; that is what opens the session.
        return BLUEZ_ERROR_NOT_PERMITTED;
    }
    if (mtu > session->mtu) session->mtu = mtu;
    if (!write_all(session->fd, byteArray->data, byteArray->len)) {
        session_close(session, g_strerror(errno), TRUE);
        return BLUEZ_ERROR_FAILED;
    }
    return NULL;
}

static const char *on_char_read(const Application *application, const char *address, const char *service_uuid,
                                const char *char_uuid, const guint16 mtu, const guint16 offset) {
    return BLUEZ_ERROR_NOT_PERMITTED;
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
    binc_application_add_characteristic(app, BLE_SSH_SERVICE_UUID, uuid, flags);
    binc_application_add_descriptor(app, BLE_SSH_SERVICE_UUID, uuid, BLE_SSH_CUD_UUID, GATT_CHR_PROP_READ);
    GByteArray *value = g_byte_array_new();
    g_byte_array_append(value, (const guint8 *) description, (guint) strlen(description));
    binc_application_set_desc_value(app, BLE_SSH_SERVICE_UUID, uuid, BLE_SSH_CUD_UUID, value);
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
        {"max-clients", 'm', 0, G_OPTION_ARG_INT, &opt_max_clients, "Max simultaneous clients (default 4)", "N"},
        {"no-advertise", 'A', 0, G_OPTION_ARG_NONE, &opt_no_advertise,
         "Only register the GATT service; let another program on this adapter advertise", NULL},
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
    if (opt_host == NULL) opt_host = g_strdup("127.0.0.1");
    if (opt_name == NULL) opt_name = default_name();
    if (opt_max_clients < 1) opt_max_clients = 1;

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
    if (!binc_adapter_get_powered_state(adapter)) binc_adapter_power_on(adapter);
    binc_adapter_set_remote_central_cb(adapter, &on_central_state_changed);

    sessions = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, session_free);

    app = binc_create_application(adapter);
    binc_application_add_service(app, BLE_SSH_SERVICE_UUID);
    add_char(BLE_SSH_RX_UUID, GATT_CHR_PROP_WRITE | GATT_CHR_PROP_WRITE_WITHOUT_RESP, "ssh rx");
    add_char(BLE_SSH_TX_UUID, GATT_CHR_PROP_NOTIFY, "ssh tx");
    binc_application_set_char_read_cb(app, &on_char_read);
    binc_application_set_char_write_cb(app, &on_char_write);
    binc_application_set_char_notify_state_cb(app, &on_notify_state);
    binc_adapter_register_application(adapter, app);

    if (opt_no_advertise) {
        // GATT services are shared by everything registered on the adapter, so a central that
        // connects through another program's advertisement still finds this service.
        msg("GATT service registered on %s without advertising, forwarding to %s:%d, max %d clients",
            binc_adapter_get_address(adapter), opt_host, opt_port, opt_max_clients);
    } else {
        GPtrArray *uuids = g_ptr_array_new();
        g_ptr_array_add(uuids, BLE_SSH_SERVICE_UUID);
        advertisement = binc_advertisement_create();
        binc_advertisement_set_local_name(advertisement, opt_name);
        binc_advertisement_set_services(advertisement, uuids);
        binc_advertisement_set_interval(advertisement, 100, 300);
        binc_adapter_start_advertising(adapter, advertisement);
        g_ptr_array_free(uuids, TRUE);
        msg("advertising '%s' on %s, forwarding to %s:%d, max %d clients", opt_name,
            binc_adapter_get_address(adapter), opt_host, opt_port, opt_max_clients);
    }

    const guint bluez_watch = g_bus_watch_name_on_connection(connection, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                             NULL, on_bluez_vanished, NULL, NULL);
    sd_notify_send("READY=1");
    notify_status();
    if (sd_notify_start_watchdog()) msg("systemd watchdog enabled");

    g_main_loop_run(loop);
    g_bus_unwatch_name(bluez_watch);

    GList *open = g_hash_table_get_values(sessions);
    for (GList *it = open; it != NULL; it = it->next) session_close(it->data, "server stopping", TRUE);
    g_list_free(open);
    g_hash_table_destroy(sessions);

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
    return exit_code;
}
