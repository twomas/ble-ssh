# ble-ssh-client (Python)

Cross-platform client for [ble-ssh](../../README.md): use SSH over Bluetooth LE from
Windows, macOS or Linux. Same command line as the C client, built on
[bleak](https://github.com/hbldh/bleak).

```bash
pipx install ./clients/python          # or: pip install ./clients/python
ssh -o ProxyCommand="ble-ssh-client AA:BB:CC:DD:EE:FF" pi@server
```

Find servers and set up an ssh alias:

```console
$ ble-ssh-client --scan
NAME          ADDRESS            RSSI
ssh-server    AA:BB:CC:DD:EE:FF   -48

$ ble-ssh-client --ssh-config ssh-server --user pi >> ~/.ssh/config
$ ssh server-ble
```

On Windows, append with `Add-Content` instead: Windows PowerShell's `>>` writes UTF-16,
which ssh can't read.

```powershell
ble-ssh-client --ssh-config ssh-server --user pi | Add-Content -Encoding ascii $HOME\.ssh\config
```

| Option | |
|--------|-|
| `ADDRESS\|NAME` | Bluetooth address, or the server's advertised name (`ssh-<hostname>`) |
| `-s, --scan` | List nearby ble-ssh servers by signal strength and exit (scans 5 s, or `--timeout`) |
| `-a, --all` | With `--scan`: list every named BLE device and mark the ble-ssh ones. A server started with `--no-advertise` hides behind another program's advertisement: find its address here |
| `--ssh-config` | Print a `~/.ssh/config` block (`Host <name>-ble`) for `ADDRESS\|NAME`, with this client's full path as `ProxyCommand` |
| `--user U`, `--alias A` | With `--ssh-config`: login user and `Host` alias |
| `-p, --pair` | Bond first, then reconnect (once, for servers with `--require-bonding`) |
| `-t, --timeout S` | Seconds to wait for the server (default 20) |
| `-r, --reliable` | Write-with-response (slower) |
| `-v, --verbose` | Progress on stderr, with timestamps |

Platform notes:

- **Windows:** works with Windows' own OpenSSH (`ssh.exe`). Use the full path to
  `ble-ssh-client.exe` in `ProxyCommand` if it isn't on `PATH`.
- **macOS:** CoreBluetooth hides MAC addresses, so connect by advertised name, or by the
  device UUID that `ble-ssh-client -v` prints. The OS pairs on demand, so `--pair` isn't
  needed and is ignored.
- **Linux:** the [C client](../../README.md#client) has no Python dependency, but this one
  works too.
