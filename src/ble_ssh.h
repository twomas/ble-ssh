/*
 * Shared definitions for the ble-ssh server and client.
 *
 * The tunnel is a single GATT service with two characteristics:
 *   RX  (central -> peripheral): write / write-without-response, bytes go to sshd
 *   TX  (peripheral -> central): notify, bytes coming from sshd
 *
 * The byte stream is passed through unmodified; SSH provides framing,
 * integrity and encryption on top.
 */
#ifndef BLE_SSH_H
#define BLE_SSH_H

#define BLE_SSH_SERVICE_UUID "5f2d0001-8c3b-4a7e-b1d9-2e6f4a9c7b10"
#define BLE_SSH_RX_UUID      "5f2d0002-8c3b-4a7e-b1d9-2e6f4a9c7b10"
#define BLE_SSH_TX_UUID      "5f2d0003-8c3b-4a7e-b1d9-2e6f4a9c7b10"
/* Read-only, only with ble_ssh_server --l2cap: the PSM of its L2CAP channel (ble_ssh_l2cap.h). */
#define BLE_SSH_PSM_UUID     "5f2d0004-8c3b-4a7e-b1d9-2e6f4a9c7b10"
/* Nordic UART Service, which terminal apps on phones speak (--nus): the same roles as the
 * ble-ssh service, RX written by the client and TX notified by the server. */
#define BLE_SSH_NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_SSH_NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_SSH_NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
/* What a --pty server sends when the program on the pty has ended, right before it drops
 * the link (GATT) or closes the channel (L2CAP). Terminal apps show it; our clients take it,
 * as the last bytes before the link drops, for the end of the session rather than a drop. */
#define BLE_SSH_PTY_END_MARKER "\r\n[ble-ssh: session ended]\r\n"
#define BLE_SSH_CUD_UUID     "00002901-0000-1000-8000-00805f9b34fb"

#define BLE_SSH_ATT_OVERHEAD 3
#define BLE_SSH_MIN_CHUNK    20
#define BLE_SSH_MAX_CHUNK    512

#endif
