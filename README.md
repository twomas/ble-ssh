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
./build/ble_ssh_server --help     # --port, --host, --name, --adapter-name, --max-clients,
                                  # --no-advertise, --no-power-on, --require-bonding, --verbose
```

To advertise under the name the device already has, use `--adapter-name`: the server
advertises the adapter's Bluetooth name (its `Alias`) and follows it when it changes, so it
doesn't matter whether the program that sets the name starts before or after ble-ssh.

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

ble-ssh never changes adapter-wide settings: pairable, discoverable, the pairing agent and
the alias are all left to the other program. The only exception is power: by default the
server powers on an adapter that is off. Add `--no-power-on` if the other program owns
adapter power too (for example a low-power mode). The GATT service is registered while the
adapter is off and becomes reachable when the other program powers it on.

```bash
./build/ble_ssh_server --no-advertise --no-power-on
```

By default the tunnel's characteristics don't require encryption, so ble-ssh never triggers
pairing on its own.

### Bonded clients only (`--require-bonding`)

Off by default. With `--require-bonding` the RX/TX characteristics require an encrypted
link, and the server only opens a session for centrals that bluetoothd reports as **bonded**.
Other centrals are disconnected before any byte reaches sshd.

ble-ssh doesn't pair anyone itself. Whether a new client may bond is decided by whatever
owns the adapter's agent and pairable setting: your own GATT server, or `bluetoothctl`
during development. Revoking a client is `bluetoothctl remove <address>` on the server.

```bash
./build/ble_ssh_server --no-advertise --no-power-on --require-bonding
ble_ssh_client --pair AA:BB:CC:DD:EE:FF      # first time: bond, then connect
ble_ssh_client AA:BB:CC:DD:EE:FF             # later: already bonded
```

On the peripheral side, bluetoothd only reports a new bond once that first link drops. So
`--pair` bonds, disconnects and reconnects, and the second link is the one the server
accepts. Other clients (e.g. a phone app) see the same thing: the first connection after
pairing is rejected, and the next one works. SSH still provides its own end-to-end
encryption and authentication on top.

For the systemd service, set the options in `/etc/default/ble-ssh` (see below).

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

#### Desktop Linux servers: BLE-MIDI

On a desktop install (Raspberry Pi OS with desktop, Ubuntu desktop) WirePlumber exposes a
BLE-MIDI GATT service whose characteristic requires encryption. Clients that connect then
try to pair for it, and without a pairing agent they drop the link before the tunnel comes
up. Unless you use BLE-MIDI, disable it for the desktop user:

```bash
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cat > ~/.config/wireplumber/wireplumber.conf.d/90-disable-ble-midi.conf <<'EOF'
wireplumber.profiles = { main = { monitor.bluez-midi = disabled } }
EOF
systemctl --user restart wireplumber
bluetoothctl show | grep 03b80e5a      # no output = the MIDI service is gone
```

Headless and buildroot systems don't run WirePlumber and aren't affected.

### Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Client prints `CONNECTED`, then `DISCONNECTED` and times out without `tunnel up` | `ReverseServiceDiscovery` is still on. Check `btmon` on the server for `SMP: Pairing Failed` followed by `Disconnect ... Authentication Failure` |
| `does not offer the ble-ssh service` | `ble_ssh_server` isn't running on that host. When run by hand rather than as the service, it also has to be restarted after `bluetoothd` restarts |
| Service fails with `system bus: The connection is closed` | The unit was changed to `DynamicUser=yes`, which `dbus-daemon` rejects. Use the shipped `User=ble-ssh` |
| The server doesn't show up in a scan | Adapter is powered off (`bluetoothctl show`), or the service isn't running (`systemctl status ble-ssh-server`) |
| `session opened` is logged but ssh hangs | sshd isn't listening on the configured `--host`/`--port` |
| Server logs `rejected, not bonded` | The client isn't bonded, or this was the first link right after pairing. Connect again, or use `ble_ssh_client --pair` |
| Client: `CONNECTED`, then `DISCONNECTED`; the client's bluetoothd logs `No agent available` | Something on the server needs encryption, and the client has no pairing agent. On a desktop Linux server this is usually WirePlumber's BLE-MIDI service: disable it with `monitor.bluez-midi = disabled` (see below). A stale bond on one side only does the same; remove it on both sides |

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

There are two clients with the same command line:

| | `ble_ssh_client` (C) | `ble-ssh-client` (Python) |
|---|---|---|
| Platforms | Linux (BlueZ) | Windows, macOS, Linux |
| Dependencies | none at runtime | Python 3.10+, [bleak](https://github.com/hbldh/bleak) |
| Install | built with the server | `pipx install ./clients/python` |
| Use it for | embedded and headless Linux | developer laptops |

The Python client is documented in [clients/python](clients/python/README.md). It can also
find servers and write the ssh alias for you, and from Windows it works with the built-in
OpenSSH:

```powershell
ble-ssh-client --scan
ble-ssh-client --ssh-config ssh-server --user pi | Add-Content -Encoding ascii $HOME\.ssh\config
ssh server-ble
```

Or click through it: `ble-ssh-gui` scans, opens an ssh session in a terminal window and
saves the alias (tkinter; on Debian/Ubuntu `sudo apt install python3-tk`).

The client must run on another machine than the server (one adapter cannot connect to
itself). Use it as an ssh `ProxyCommand`, with the server's Bluetooth address or advertised
name:

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

Options: `--timeout S` (default 20), `--pair` (bond first, needed once for servers with
`--require-bonding`), `--reliable` (write-with-response, slower), `--verbose` (progress on
stderr). `--pair` registers a NoInputNoOutput agent for the duration of the run, so only
Just Works pairing is supported.

## Performance

Expect roughly 10–50 kB/s depending on MTU, PHY and connection interval: fine for a shell,
slow for large `scp` transfers. Measured from Windows (Python client) to a Raspberry Pi 5:
256 KiB in 14 s including connection setup, about 20 kB/s.

## Sessions and link reuse

A session is exactly one subscription to TX: it starts when the client enables
notifications and ends when it disables them or disconnects. The server connects to sshd on
the client's first write.

Ending a session cleanly matters because the BLE link can outlive the client:

- **ssh kills its ProxyCommand** when the session ends: SIGHUP on Linux and macOS,
  `TerminateProcess` on Windows. Both clients catch SIGHUP and then unsubscribe and
  disconnect. `TerminateProcess` can't be caught, so on Windows the Python client runs the
  BLE work in a detached child process that outlives the one ssh kills.
- **Windows keeps the link up** for a few seconds after a disconnect and reuses it for the
  next connection. The Python client therefore toggles notifications off and on when it
  starts, so a leftover subscription can't swallow the new session.
- **Right after a previous connection**, the next attempt can hit the old link while it is
  still going away (`le-connection-abort-by-local`, `Not Connected`, or Windows' "the object
  has been closed"). Both clients retry until `--timeout`.

Tested with 14 connections in a row from Linux (both clients, alternating) and 5 in a row
from Windows, with no pause between them.

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
