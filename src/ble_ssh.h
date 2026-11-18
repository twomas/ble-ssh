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
#define BLE_SSH_CUD_UUID     "00002901-0000-1000-8000-00805f9b34fb"

#define BLE_SSH_ATT_OVERHEAD 3
#define BLE_SSH_MIN_CHUNK    20
#define BLE_SSH_MAX_CHUNK    512

#endif
