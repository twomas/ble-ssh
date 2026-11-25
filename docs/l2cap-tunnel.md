# l2cap_tunnel: LE L2CAP channels for any BLE server

`src/l2cap_tunnel.c` and `src/l2cap_tunnel.h` are the server side of LE L2CAP
connection-oriented channels as a small API on a GLib main loop. They need GLib and a Linux
kernel with Bluetooth, nothing else: no BlueZ headers, no D-Bus, no bluez_inc. A program that
already has its own GATT server (and advertising) copies the two files in and gets channels
next to it. `ble_ssh_server --l2cap` is built on them.

## Use

```c
static void on_rx(L2capConn *c, const guint8 *data, gsize len, gpointer user_data) {
    l2cap_conn_send(c, data, len);
}
static const L2capTunnelCallbacks callbacks = {.accept = on_accept, .connected = on_connected,
                                               .rx = on_rx, .disconnected = on_disconnected};
L2capTunnel *t = l2cap_tunnel_new(0, L2CAP_TUNNEL_ENCRYPTED, &callbacks, NULL);
// publish l2cap_tunnel_get_psm(t) in a characteristic of your GATT service, or use a fixed PSM
```

- **One handle (`L2capConn`) per channel:** `connected`, `rx`, `disconnected`, with your own
  data per handle (`l2cap_conn_set_user_data()`). `accept` decides about a new channel by the
  central's address, e.g. to check that it is bonded.
- **A byte stream each way:** the protocol on top does its own framing. Sends are queued and
  go out in SDUs of up to the central's MTU; `writable` and `l2cap_conn_get_queued()` are the
  back-pressure.
- **`l2cap_conn_close()`** sends what is queued first, then closes: `close()` on an LE
  channel drops what the kernel still holds.
- **Security:** `L2CAP_TUNNEL_ENCRYPTED` only takes encrypted links (`BT_SECURITY_MEDIUM`).
- **Advertising is yours:** a central can only open a channel to a peripheral it can connect
  to, so something on the adapter must advertise connectable.
- Callbacks may call any `l2cap_conn_*` function, not `l2cap_tunnel_free()`. Freeing the
  tunnel calls `disconnected` ("tunnel freed") for every channel still open.

`examples/l2cap_echo.c` is the smallest program on it (built as `l2cap_echo`, not installed).

## Tested

Between two Raspberry Pi 5 (server Ubuntu 26.04, client Debian 13), with ble_ssh_server
ported onto the tunnel and with `l2cap_echo`: pty sessions; 2 MB down and 1 MB up with
matching sha256 (back-pressure); two channels at once; a client killed mid-session (nothing
left); 20 sessions (fds 8 before and after, no children); the SSH handshake over L2CAP and
GATT; `--require-bonding` refusing an unbonded client; `l2cap_echo` echoing 200 kB intact,
closing after `bye`, and freeing the tunnel with a channel open.
