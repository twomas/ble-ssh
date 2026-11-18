#include "sd_notify.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

gboolean sd_notify_send(const char *state) {
    const char *path = g_getenv("NOTIFY_SOCKET");
    if (path == NULL || path[0] == '\0' || state == NULL) return FALSE;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    const size_t len = strlen(path);
    if ((path[0] != '/' && path[0] != '@') || len >= sizeof(addr.sun_path)) return FALSE;
    memcpy(addr.sun_path, path, len);
    if (path[0] == '@') addr.sun_path[0] = '\0'; // abstract namespace
    const socklen_t addr_len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + len);

    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return FALSE;
    ssize_t sent;
    do {
        sent = sendto(fd, state, strlen(state), MSG_NOSIGNAL, (struct sockaddr *) &addr, addr_len);
    } while (sent < 0 && errno == EINTR);
    close(fd);
    return sent >= 0;
}

gboolean sd_notify_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    gchar *state = g_strdup_vprintf(fmt, args);
    va_end(args);
    const gboolean result = sd_notify_send(state);
    g_free(state);
    return result;
}

static gboolean on_watchdog(gpointer data) {
    sd_notify_send("WATCHDOG=1");
    return G_SOURCE_CONTINUE;
}

gboolean sd_notify_start_watchdog(void) {
    const char *usec_str = g_getenv("WATCHDOG_USEC");
    const char *pid_str = g_getenv("WATCHDOG_PID");
    if (usec_str == NULL) return FALSE;
    if (pid_str != NULL && strtoul(pid_str, NULL, 10) != (unsigned long) getpid()) return FALSE;
    const guint64 usec = g_ascii_strtoull(usec_str, NULL, 10);
    if (usec == 0) return FALSE;
    const guint interval_ms = (guint) MAX(usec / 2000, 1);
    g_timeout_add(interval_ms, on_watchdog, NULL);
    return TRUE;
}
