# L2CAP channels (prototype)

Status: prototype on the `l2cap-prototype` branch, Linux C server and C client only. GATT is
unchanged and stays the default; everything here is opt-in with `--l2cap`.

## What it does

`ble_ssh_server --l2cap` also listens on an LE L2CAP connection-oriented channel (LE credit
based flow control) and publishes its PSM in a read-only characteristic,
`5f2d0004-8c3b-4a7e-b1d9-2e6f4a9c7b10` (2 bytes, little endian). `ble_ssh_client --l2cap`
connects over BLE as before, reads the PSM and opens a channel to it. The channel carries
the SSH bytes as they are; closing it ends the session.

- **One channel, one session.** The server's `accept()` gives a socket per channel, and
  each gets its own connection to sshd.
- **Channels share the link.** A second client on the same machine joins the link that is
  already up, instead of being refused as with GATT. It never disconnects a link it joined.
- **Falls back to GATT** when the server offers no PSM (an older server, or no `--l2cap`),
  or when the channel cannot be opened. Old clients use GATT on a new server.
- **`--require-bonding`** makes the listening socket demand an encrypted link
  (`BT_SECURITY_MEDIUM`), and the server also checks that the central is bonded.
- **Shared limit:** `--max-clients` counts GATT and L2CAP sessions together;
  `systemctl status` shows e.g. `2/4 clients connected (1 over L2CAP)`.
- **`--psm N`** picks the PSM (128-255); by default the kernel picks a free one.

The socket definitions are written out in `src/ble_ssh_l2cap.h`, so neither program needs
libbluetooth. The systemd unit allows `AF_BLUETOOTH` for this.

```bash
ble_ssh_server --l2cap                                  # on the server
ssh -o ProxyCommand="ble_ssh_client --l2cap ssh-server" pi@ssh-server
```

## Measured

Between two Raspberry Pi 5 (Debian 13, kernel 6.12, BlueZ 5.82, client; Ubuntu 26.04,
kernel 7.0, BlueZ 5.85, server), with a test backend instead of sshd. The
connection interval was 48.75 ms (bluetoothd's default 30-50 ms) and the data length 251.

| Test | GATT | L2CAP |
|------|------|-------|
| 1 MiB upload, 3 runs | 51-53 KiB/s | 52-57 KiB/s |
| 1 MiB download, 3 runs | 56-59 KiB/s | 55-58 KiB/s |
| 4 MiB each way | - | 62 KiB/s up, 69 KiB/s down |
| Interval 7.5-15 ms (changed on the live link), 2 MiB down | 15 KiB/s | 22 KiB/s |
| Short sessions while a 4 MiB download runs, same client | refused ("link in use") | 0.6-0.9 s each, download unaffected |

- **Throughput is the same.** At this interval the radio link is the limit, not ATT's
  overhead. A shorter interval was much slower for both on this controller, which shares
  its radio with Wi-Fi.
- **Parallel sessions are the real gain:** a second session from the same computer, e.g. an
  interactive shell next to a large `scp`, which GATT can't do.
- SSH runs through a channel (handshake, key exchange and host key checked; the login was
  refused only because the client had no key there).
- 50 sessions (40 L2CAP, 10 GATT, some in parallel): all completed, the server's file
  descriptors stayed at 8 and its memory flat after the first 25.
- GATT and L2CAP sessions run side by side: a GATT download with L2CAP sessions on the same
  link, and the Windows client's GATT session while another computer downloads over L2CAP.
- Unrelated to L2CAP, and the same with main's server: a GATT download to the Windows
  (Python) client stops after about 20 KiB and the link drops; not investigated yet.

## Learned on the way

- `BT_RCVMTU` and `BT_SNDMTU` fail with `EINVAL` until the socket is bound to an LE address
  type, so the socket is bound first, also on the client.
- `close()` drops what the kernel still has queued for an LE channel (SDUs waiting for
  credits): the last few KiB of a download were lost. Both sides now wait until the queue
  is empty (`TIOCOUTQ` against `SO_SNDBUF`) before they close, at most 5 s.
- BlueZ signals a characteristic's notifications to every process watching the device, so
  a client that joined a shared link ignores TX notifications: they belong to another
  client's GATT session.
- Desktop servers drop the link a few seconds after it comes up when the client side reads
  their BLE-MIDI characteristic and the pairing it starts fails (see
  [Desktop Linux servers](../README.md#desktop-linux-servers-ble-midi)). That hits GATT and
  L2CAP alike; the tests ran with the greeter's WirePlumber stopped.

## Not done

- **No L2CAP on Windows:** WinRT has no public API for LE channels, and bleak has none
  either, so the Python client stays on GATT. macOS, iOS and Android have such APIs.
- **Joined links:** the client that made the link still disconnects it when it exits,
  which ends the channels of clients that joined it later.
- **Bonding:** only the refusal of an unbonded client was tested (the two test Pis are not
  bonded); a bonded client over L2CAP is not.
- **Not on the buildroot target yet.**
