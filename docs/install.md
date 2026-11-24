# Installing ble-ssh

Step by step, with commands to copy: the server on a target, the client on Windows, Linux
or macOS, and how to update and remove both. Everything comes from the
[GitHub releases](https://github.com/twomas/ble-ssh/releases), which need no login. How it
all works is in the [README](../README.md).

Which release:

| Release | What | In the URLs below |
|---------|------|-------------------|
| `main-latest` | Newest build of main, replaced on every push | `releases/download/main-latest` |
| `vX.Y.Z` | A version | `releases/download/vX.Y.Z` |
| newest version | The newest `vX.Y.Z` | `releases/latest/download` |

The commands use `main-latest`; replace it to install a version.

## Server on a target

The target needs:

- Linux with systemd, arm64 or x86_64, glibc 2.35 or newer (Ubuntu 22.04 and newer,
  Debian 12 and newer, Raspberry Pi OS bookworm and newer, buildroot with glibc),
- BlueZ (`bluetoothd`) and an ssh server (OpenSSH or Dropbear),
- root, or a user with sudo.

### From your computer

`install-target.sh` logs in to the target with ssh, picks the build for its architecture,
checks it and runs the installer there. It needs bash, ssh, curl and tar: any Linux, or
Git Bash on Windows. Set up ssh key login to the target first (`ssh-copy-id`).

```bash
curl -fsSLO https://github.com/twomas/ble-ssh/releases/download/main-latest/install-target.sh
bash install-target.sh -n root@target       # dry run: checks only, changes nothing
bash install-target.sh root@target          # install, or update
```

In PowerShell, run it with Git's bash (a plain `bash` there is WSL's):

```powershell
curl.exe -fsSLO https://github.com/twomas/ble-ssh/releases/download/main-latest/install-target.sh
& "C:\Program Files\Git\bin\bash.exe" install-target.sh root@target
```

Useful options:

| Option | |
|--------|-|
| `--opts "..."` | Server options for the service, e.g. `--opts "--no-power-on --require-bonding --no-advertise"` on a target whose own program owns the adapter, or add `--l2cap` to also accept sessions over L2CAP channels ([what that gives](l2cap.md)). Without it, an existing `/etc/default/ble-ssh` is kept |
| `-J user@jumphost` | Reach the target through another machine, e.g. a target on USB behind a Raspberry Pi |
| `--client` | Also install the C client `ble_ssh_client` on the target |
| `--release vX.Y.Z` | Another release than the one the script came from |
| `--help` | All options |

### On the target itself

If the target can reach GitHub:

```sh
curl -fsSLO https://github.com/twomas/ble-ssh/releases/download/main-latest/ble-ssh-arm64.tar.gz
tar -xzf ble-ssh-arm64.tar.gz
sudo ble-ssh-arm64/install.sh -n            # checks only
sudo ble-ssh-arm64/install.sh               # install, or update
```

Use `ble-ssh-x86_64.tar.gz` on a PC. `install.sh` takes the same `--opts`, `--client` and
`--no-start` options.

### Check it

Both ways check that the binary runs on the target before they change anything, and warn
about what the server needs but does not install. Fix the warnings:

- **`ReverseServiceDiscovery = false is not set`**: set it under `[General]` in
  `/etc/bluetooth/main.conf`, then `sudo systemctl restart bluetooth`
  ([why](../README.md#bluez-configuration-required)).
- **`nothing listens on 127.0.0.1:22`**: install or start the ssh server, or point the
  service at it with `--opts "--port N"`.
- **`bluetooth.service not found`**: install BlueZ.

Then:

```sh
systemctl status ble-ssh-server     # "Status: 0/4 clients connected"
journalctl -u ble-ssh-server -f     # log while a client connects
```

## Client on Windows

In PowerShell:

1. Python 3.10 or newer, if it is missing. Open a new PowerShell afterwards.

   ```powershell
   winget install Python.Python.3.12
   ```

2. pipx, which puts the client in its own environment and its commands on `PATH`. Open a
   new PowerShell afterwards.

   ```powershell
   py -m pip install --user pipx
   py -m pipx ensurepath
   ```

3. The client, from the release. Run the same three lines again to update.

   ```powershell
   $b = "https://github.com/twomas/ble-ssh/releases/download/main-latest"
   $whl = (Invoke-RestMethod "$b/SHA256SUMS") -split "\s+" | Where-Object { $_ -like "ble_ssh_client-*.whl" }
   pipx install --force "$b/$whl"
   ```

4. Start the GUI with `ble-ssh-gui`, or put a shortcut on the desktop:

   ```powershell
   $s = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:USERPROFILE\Desktop\BLE SSH.lnk")
   $s.TargetPath = "$env:USERPROFILE\.local\bin\ble-ssh-gui.exe"
   $s.Save()
   ```

Without pipx, `py -m pip install --user --force-reinstall "$b/$whl"` works too; the commands
then land in `%APPDATA%\Python\Python312\Scripts`, which may not be on `PATH`.

## Client on Linux and macOS

The Python client with its GUI, with [pipx](https://pipx.pypa.io/) (on Debian and Ubuntu:
`sudo apt install pipx python3-tk`):

```bash
b=https://github.com/twomas/ble-ssh/releases/download/main-latest
pipx install --force "$b/$(curl -fsSL "$b/SHA256SUMS" | grep -o 'ble_ssh_client-[^ ]*\.whl')"
```

On Linux, the C client needs no Python. It is in the server tarball:

```bash
curl -fsSLO https://github.com/twomas/ble-ssh/releases/download/main-latest/ble-ssh-arm64.tar.gz
tar -xzf ble-ssh-arm64.tar.gz
sudo install -m 0755 ble-ssh-arm64/usr/bin/ble_ssh_client /usr/local/bin/
```

## Connect

The GUI finds servers, connects and opens ssh. From a terminal, with the Python client:

```bash
ble-ssh-client --scan                                   # servers nearby
ble-ssh-client --scan --all                             # also servers started with --no-advertise
ble-ssh-client --ssh-config ssh-server --user pi >> ~/.ssh/config
ssh ssh-server-ble
```

In PowerShell, append to `$env:USERPROFILE\.ssh\config` instead.

The C client, `ble_ssh_client`, has no `--scan` or `--ssh-config`; give it the server's name
or address as `ProxyCommand`. On a target started with `--l2cap`, add `--l2cap` to use its
L2CAP channel, which lets several sessions from one computer share the link:

```bash
ssh -o ProxyCommand="ble_ssh_client --l2cap ssh-server" pi@ssh-server
```

A target with `--require-bonding` only accepts a client that is paired with it. Pair once,
then connect as usual; the first connection right after pairing is refused, the next one
works:

```bash
ssh -o ProxyCommand="ble-ssh-client --pair AA:BB:CC:DD:EE:FF" pi@ssh-server
```

Run one client at a time: a scan or the GUI running next to a connection makes Windows fail
with "object has been closed".

## Update

Run the install again, the same way: `bash install-target.sh root@target`,
`sudo ble-ssh-<arch>/install.sh` with a newer tarball, or the client's `pipx install --force`
lines. Settings in `/etc/default/ble-ssh` are kept.

## Remove

Server, from your computer or on the target:

```bash
bash install-target.sh --uninstall root@target
sudo /var/lib/ble-ssh-install/install.sh --uninstall      # on the target; no tarball needed
```

This puts back what was there before the first install, e.g. a buildroot package's server,
and enables it again if it was. `--purge` also removes `/etc/default/ble-ssh` and the
`ble-ssh` user, if the installer created them. `-n` lists what would happen.

Client:

```bash
pipx uninstall ble-ssh-client
sudo rm /usr/local/bin/ble_ssh_client     # the C client, if you copied it
```
