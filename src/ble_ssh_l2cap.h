/*
 * LE L2CAP connection-oriented channels (credit based flow control) for the ble-ssh tunnel.
 *
 * The kernel's Bluetooth socket API, with the few definitions it needs written out here so
 * the programs don't need BlueZ's libbluetooth headers (buildroot doesn't always have them).
 * The values are the Linux ABI from <bluetooth/bluetooth.h> and <bluetooth/l2cap.h>.
 *
 * A server that listens on a channel publishes its PSM (2 bytes, little endian) in the
 * BLE_SSH_PSM_UUID characteristic. Every channel is one session, carrying the SSH bytes as
 * they are: no framing; closing the channel ends the session.
 */

#ifndef BLE_SSH_L2CAP_H
#define BLE_SSH_L2CAP_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BLE_SSH_BTPROTO_L2CAP 0
#define BLE_SSH_SOL_BLUETOOTH 274
#define BLE_SSH_BT_SECURITY 4
#define BLE_SSH_BT_SECURITY_LOW 1
#define BLE_SSH_BT_SECURITY_MEDIUM 2 /* encrypted link */
#define BLE_SSH_BT_SNDMTU 12
#define BLE_SSH_BT_RCVMTU 13
#define BLE_SSH_BDADDR_LE_PUBLIC 1
#define BLE_SSH_BDADDR_LE_RANDOM 2

/* The largest SDU we accept; the kernel splits it into PDUs the controller can carry. */
#define BLE_SSH_L2CAP_MTU 8192

typedef struct {
    uint8_t b[6]; /* little endian: b[0] is the last byte of AA:BB:CC:DD:EE:FF */
} __attribute__((packed)) ble_ssh_bdaddr;

struct ble_ssh_sockaddr_l2 {
    sa_family_t l2_family;
    uint16_t l2_psm; /* little endian */
    ble_ssh_bdaddr l2_bdaddr;
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
};

struct ble_ssh_bt_security {
    uint8_t level;
    uint8_t key_size;
};

static inline uint16_t ble_ssh_htobs(uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return (uint16_t) ((v >> 8) | (v << 8));
#endif
}

static inline int ble_ssh_str2ba(const char *str, ble_ssh_bdaddr *ba) {
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) != 6) return -1;
    for (int i = 0; i < 6; i++) ba->b[i] = (uint8_t) b[i];
    return 0;
}

static inline void ble_ssh_ba2str(const ble_ssh_bdaddr *ba, char str[18]) {
    snprintf(str, 18, "%02X:%02X:%02X:%02X:%02X:%02X", ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

/* An LE L2CAP socket bound to every adapter (BDADDR_ANY) and to psm: a server's PSM, or 0
 * for a client (or to let the kernel pick a free one, 0x80-0xff). The kernel only accepts
 * BT_RCVMTU once the socket is bound to an LE address type, hence the bind here. */
static inline int ble_ssh_l2cap_socket(int extra_flags, uint8_t security, uint16_t psm) {
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC | extra_flags, BLE_SSH_BTPROTO_L2CAP);
    if (fd < 0) return -1;
    struct ble_ssh_bt_security sec = {.level = security};
    struct ble_ssh_sockaddr_l2 addr = {0};
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = ble_ssh_htobs(psm);
    addr.l2_bdaddr_type = BLE_SSH_BDADDR_LE_PUBLIC;
    uint16_t mtu = BLE_SSH_L2CAP_MTU;
    if (setsockopt(fd, BLE_SSH_SOL_BLUETOOTH, BLE_SSH_BT_SECURITY, &sec, sizeof(sec)) < 0 ||
        bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
        setsockopt(fd, BLE_SSH_SOL_BLUETOOTH, BLE_SSH_BT_RCVMTU, &mtu, sizeof(mtu)) < 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Bytes this socket has queued in the kernel and not yet handed to the controller (SDUs
 * waiting for credits). close() drops them on an LE channel, so wait for 0 before closing.
 * TIOCOUTQ on a Bluetooth socket is the free send buffer space, sk_sndbuf - sk_wmem_alloc. */
static inline int ble_ssh_l2cap_unsent(int fd) {
    int sndbuf = 0, space = 0;
    socklen_t len = sizeof(sndbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) < 0 || ioctl(fd, TIOCOUTQ, &space) < 0) return 0;
    return sndbuf > space ? sndbuf - space : 0;
}

/* How often and how long to wait for ble_ssh_l2cap_unsent() to reach 0 before closing. */
#define BLE_SSH_L2CAP_DRAIN_MS 20
#define BLE_SSH_L2CAP_DRAIN_TRIES 250

/* The peer's receive MTU: the largest SDU one send() may carry. */
static inline uint16_t ble_ssh_l2cap_sndmtu(int fd) {
    uint16_t mtu = 0;
    socklen_t len = sizeof(mtu);
    if (getsockopt(fd, BLE_SSH_SOL_BLUETOOTH, BLE_SSH_BT_SNDMTU, &mtu, &len) < 0 || mtu == 0) return 23;
    return mtu;
}

#endif
