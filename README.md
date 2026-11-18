# ble-ssh

SSH over Bluetooth Low Energy. Handy for reaching a headless Linux box (e.g. a Raspberry Pi)
when the network is down or not configured yet.

```
ssh ──stdin/stdout── ble_ssh_client  ~~~ BLE GATT ~~~  ble_ssh_server ──TCP── sshd :22
     (ProxyCommand)     (central)                       (peripheral)
```

The tunnel only moves bytes; authentication and encryption are plain SSH, so nothing about
your sshd setup changes. The server handles several clients at once: every central that
subscribes gets its own TCP connection to sshd.

Built on [bluez_inc](https://github.com/twomas/bluez_inc) (submodule in `external/bluez_inc`).

## Demo

A Raspberry Pi 5 logging in to another Pi over Bluetooth LE only:

```console
pi@client:~$ ssh -o ProxyCommand="ble_ssh_client AA:BB:CC:DD:EE:FF" pi@server
pi@server's password:
Linux server 6.12.109+rpt-rpi-2712 #1 SMP PREEMPT Debian 1:6.12.109-1+rpt1 aarch64
...
pi@server:~ $ echo $SSH_CONNECTION
127.0.0.1 41628 127.0.0.1 22
```

sshd sees the session coming from `127.0.0.1`: the bytes arrived over BLE, not the network.

## Build

Requires Linux with BlueZ 5.x, CMake >= 3.16 and GLib/GIO headers.

```bash
sudo apt install build-essential cmake pkg-config libglib2.0-dev
git clone --recurse-submodules <repo-url> ble-ssh
cd ble-ssh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Server

Try it in the foreground first:

```bash
./build/ble_ssh_server            # advertises as ssh-<hostname>, forwards to 127.0.0.1:22
./build/ble_ssh_server --help     # --port, --host, --name, --max-clients, --no-advertise, --verbose
```

### Next to another BLE peripheral

If another program on the same adapter already advertises (your product's own GATT server,
for example), run ble-ssh with `--no-advertise`:

```bash
./build/ble_ssh_server --no-advertise
```

BlueZ shares one GATT database between all applications on an adapter. A central that
connects through the other program's advertisement therefore sees both services, and the
two programs don't interfere with each other. Clients then have to connect by Bluetooth
address (`ble_ssh_client AA:BB:CC:DD:EE:FF`): the advertised name and service UUID belong
to the other program, so lookup by name won't find the server.

For the systemd service, set it in `/etc/default/ble-ssh` (see below).

### Run as a systemd service

```bash
sudo cmake --install build                  # binaries, unit and sysusers.d file under the prefix
sudo systemd-sysusers                       # creates the ble-ssh system user
sudo install -m 0644 systemd/ble-ssh.default /etc/default/ble-ssh   # optional settings
sudo systemctl daemon-reload
sudo systemctl enable --now ble-ssh-server
systemctl status ble-ssh-server             # shows e.g. "Status: 1/4 clients connected"
journalctl -u ble-ssh-server -f
```

With the default prefix `/usr/local` the unit lands in `/usr/local/lib/systemd/system`, which
systemd searches. Distribution and buildroot packages use `-DCMAKE_INSTALL_PREFIX=/usr`; the
unit's `ExecStart` is generated from the prefix, so it always points at the installed binary.

What the unit does:

| Concern | How |
|---------|-----|
| Startup | `Type=notify`: the service counts as started only once the GATT service is registered and advertising has started. `STATUS=` reports connected clients in `systemctl status`. |
| Hang detection | `WatchdogSec=30`: the GLib main loop pings systemd. If it stalls, systemd restarts the service. |
| bluetoothd lifecycle | The GATT service and advertisement live inside `bluetoothd`. `PartOf=bluetooth.service` restarts and stops ble-ssh together with it, and `WantedBy=bluetooth.service` starts it whenever bluetoothd starts. If bluetoothd crashes, the server notices `org.bluez` leaving the bus, exits with an error, and `Restart=always` brings it back. |
| Options | `/etc/default/ble-ssh` sets `BLE_SSH_OPTS`, e.g. `--no-advertise` or `--port 2222`. The file is optional. |
| Privileges | Runs as the static system user `ble-ssh` (from `sysusers.d`) in a strict sandbox (`systemd-analyze security` rates it 1.3, "OK"). It only needs the system D-Bus and a loopback TCP connection to sshd. `DynamicUser=` isn't used because the classic `dbus-daemon` rejects dynamic users on the system bus. |

### BlueZ configuration (required)

The server host needs two settings in `/etc/bluetooth/main.conf`. Edit the existing file
and keep everything else in it; don't replace it. Debian, Ubuntu and Raspberry Pi OS ship
both options commented out, so uncomment them and set the values under the section shown:

```ini
[General]
ReverseServiceDiscovery = false

[Policy]
AutoEnable = true
```

- **`ReverseServiceDiscovery = false`:** this one is required. By default `bluetoothd` on the
  server explores the client's own GATT services as soon as the client connects. If the
  client exposes a service that needs encryption (BlueZ's built-in BLE-MIDI service does,
  for example) and the client isn't pairable, the server asks for pairing, the pairing
  fails, and the kernel drops the link with "Authentication Failure" before the tunnel
  comes up. The tunnel never needs the client's services, so turning this off is safe.
- **`AutoEnable = true`:** powers the adapter on at boot, so the service can advertise
  without anyone logging in.

Keep a backup and apply the change:

```bash
sudo cp /etc/bluetooth/main.conf /etc/bluetooth/main.conf.bak
sudoedit /etc/bluetooth/main.conf
sudo systemctl restart bluetooth        # ble-ssh-server restarts with it (PartOf=)
```

### Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Client prints `CONNECTED`, then `DISCONNECTED` and times out without `tunnel up` | `ReverseServiceDiscovery` is still on. Check `btmon` on the server for `SMP: Pairing Failed` followed by `Disconnect ... Authentication Failure` |
| `does not offer the ble-ssh service` | `ble_ssh_server` isn't running on that host. When run by hand rather than as the service, it also has to be restarted after `bluetoothd` restarts |
| Service fails with `system bus: The connection is closed` | The unit was changed to `DynamicUser=yes`, which `dbus-daemon` rejects. Use the shipped `User=ble-ssh` |
| The server doesn't show up in a scan | Adapter is powered off (`bluetoothctl show`), or the service isn't running (`systemctl status ble-ssh-server`) |
| `session opened` is logged but ssh hangs | sshd isn't listening on the configured `--host`/`--port` |

### Notes

- sshd sees every BLE client as coming from `127.0.0.1`. The server logs which Bluetooth
  address owns which session, so you can correlate with `journalctl`.
- Anyone in radio range can reach the sshd login prompt, exactly as anyone on your network
  can. Use key-based login (`PasswordAuthentication no`).
- No BLE pairing is required; SSH already provides end-to-end encryption and host-key checks.
- Sessions are per BLE link, so "several clients" means several client machines. Two ssh
  processes on the same client machine would share one link to the server.

## Buildroot

`buildroot/` is a `BR2_EXTERNAL` tree with a `ble-ssh` package. It depends only on BlueZ
(`bluez5-utils`) and GLib, not on whatever else runs on the device.

```bash
make BR2_EXTERNAL=/path/to/ble-ssh/buildroot menuconfig   # External options → ble-ssh
```

| Option | Effect |
|--------|--------|
| `BR2_PACKAGE_BLE_SSH` | Builds `ble_ssh_server`. With systemd, installs the service enabled and creates the `ble-ssh` user. |
| `BR2_PACKAGE_BLE_SSH_OPTS` | Written to `/etc/default/ble-ssh`, e.g. `--no-advertise` when your own GATT server advertises. |
| `BR2_PACKAGE_BLE_SSH_CLIENT` | Also installs `ble_ssh_client` on the target. |

You still need to:

- select an SSH server (`BR2_PACKAGE_OPENSSH` or `BR2_PACKAGE_DROPBEAR`),
- ship `/etc/bluetooth/main.conf` with `ReverseServiceDiscovery = false` from your rootfs
  overlay (see [BlueZ configuration](#bluez-configuration-required)).

The package builds the release tag in `BLE_SSH_VERSION`. To build a local checkout instead,
add `BLE_SSH_OVERRIDE_SRCDIR = /path/to/ble-ssh` to `local.mk` in your output directory.

## Client

The client runs on another Linux machine with BlueZ (one adapter cannot connect to itself).
Use it as an ssh `ProxyCommand`, with the server's Bluetooth address or advertised name:

```bash
ssh -o ProxyCommand="ble_ssh_client AA:BB:CC:DD:EE:FF" pi@raspberrypi
ssh -o ProxyCommand="ble_ssh_client ssh-raspberrypi" pi@raspberrypi
```

or in `~/.ssh/config`:

```
Host pi-ble
    HostName raspberrypi
    User pi
    ProxyCommand ble_ssh_client ssh-raspberrypi
```

Options: `--timeout S` (default 20), `--reliable` (write-with-response, slower), `--verbose`
(progress on stderr).

## Performance

Expect roughly 10–50 kB/s depending on MTU, PHY and connection interval: fine for a shell,
slow for large `scp` transfers.

## Protocol

| UUID | Name | Properties | Direction |
|------|------|------------|-----------|
| `5f2d0001-8c3b-4a7e-b1d9-2e6f4a9c7b10` | service | | |
| `5f2d0002-8c3b-4a7e-b1d9-2e6f4a9c7b10` | RX | write, write-without-response | client → sshd |
| `5f2d0003-8c3b-4a7e-b1d9-2e6f4a9c7b10` | TX | notify | sshd → client |

A session opens when the client enables notifications on TX and closes when it disables
them or disconnects. When sshd closes the connection the server drops the BLE link.

## License

MIT, see [LICENSE](LICENSE). bluez_inc is MIT licensed as well.
