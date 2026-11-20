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

## GUI

`ble-ssh-gui` is installed with the client: a small window to find a server and log in,
without typing addresses or `ProxyCommand`s.

![ble-ssh-gui](../../docs/ble-ssh-gui.png)

```bash
ble-ssh-gui
```

- **Scan** lists ble-ssh servers live as they are heard (20 s; the button turns into **Stop**).
  Tick **All named devices** to see every named BLE device: a server started with
  `--no-advertise` runs behind another program's advertisement, and goes by that device's
  name. A device heard under several names (a server next to the device's own advertising)
  lists them all, as `ssh-server / Gadget-1234`, and is connected by address.
- **Connect** (or double-click a row) opens ssh in a new terminal window, with this client as
  `ProxyCommand` and `--progress`, so the window says what is happening while BLE connects.
  The window stays open after ssh exits, so errors can be read.
- **Add to ~/.ssh/config** saves the server as an ssh alias (it shows the block first and
  refuses an alias that already exists); after that, plain `ssh <alias>` works from any
  terminal. **Copy ssh command** puts the full command on the clipboard.
- The login user is remembered per server. Left empty, ssh uses your local user name, which
  is rarely the right one on the server.

Terminals: `cmd` on Windows, Terminal.app on macOS, and on Linux the desktop's
`x-terminal-emulator`, or ptyxis, gnome-terminal, kgx, konsole, xfce4-terminal or xterm.
tkinter ships with Python on Windows and macOS; on Debian/Ubuntu install `python3-tk`.

## Options

| Option | |
|--------|-|
| `ADDRESS\|NAME` | Bluetooth address, or the server's advertised name (`ssh-<hostname>`, or with `--no-advertise` the name of the program that advertises) |
| `-s, --scan` | List nearby ble-ssh servers by signal strength and exit (scans 5 s, or `--timeout`) |
| `-a, --all` | With `--scan`: list every named BLE device and mark the ble-ssh ones. A server started with `--no-advertise` hides behind another program's advertisement: find its name or address here |
| `--ssh-config` | Print a `~/.ssh/config` block (`Host <name>-ble`) for `ADDRESS\|NAME`, with this client's full path as `ProxyCommand` |
| `--user U`, `--alias A` | With `--ssh-config`: login user and `Host` alias |
| `-p, --pair` | Bond first, then reconnect (once, for servers with `--require-bonding`) |
| `-t, --timeout S` | Seconds to wait for the server (default 20) |
| `-r, --reliable` | Write-with-response (slower) |
| `--progress` | One status line per step on stderr (looking, connecting, connected); `--ssh-config` and the GUI turn it on |
| `-v, --verbose` | Detailed progress on stderr, with timestamps |

## Platform notes

- **Windows:** works with Windows' own OpenSSH (`ssh.exe`). Use the full path to
  `ble-ssh-client.exe` in `ProxyCommand` if it isn't on `PATH`.
- **macOS:** CoreBluetooth hides MAC addresses, so connect by advertised name, or by the
  device UUID that `ble-ssh-client -v` prints. The OS pairs on demand, so `--pair` isn't
  needed and is ignored.
- **Linux:** the [C client](../../README.md#client) has no Python dependency, but this one
  works too.
