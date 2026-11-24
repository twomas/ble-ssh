# Console on a pty (prototype)

Status: prototype on the `pty-console` branch, in the C server and C client. Off unless the
server is started with `--pty`.

## What it does

With `--pty CMD` the server runs CMD on a pseudo terminal of its own for every session,
instead of connecting to sshd: a serial console over BLE. CMD is normally `/bin/login`.
With `--nus` the server offers the Nordic UART Service instead of ble-ssh's, which terminal
apps on phones understand, so a phone is enough to log in when the network, sshd or the ssh
keys are broken.

```bash
ble_ssh_server --pty /bin/login --require-bonding --nus     # as root: login needs it
ble_ssh_client --nus ssh-server                             # or a NUS terminal app
```

- **Every session gets its own pty** (`forkpty()`): over GATT one per link, over L2CAP one per
  channel, so `--l2cap` gives several consoles from one computer.
- **The terminal** is 24×80 with `TERM=vt100`; a client has no way to send its size yet.
- **The end of a session:** when CMD exits, the server sends `[ble-ssh: session ended]` and
  then drops the link (GATT) or closes the channel (L2CAP). Our clients recognise the marker
  and end quietly; terminal apps show it.
- **When the client goes away**, the server closes the master and sends SIGHUP to CMD's
  process group, so nothing is left running. CMD starts with default signal handling,
  whatever the server inherited (e.g. `nohup`).
- **login's vhangup:** login revokes the terminal before it asks for the password, and until
  it has opened it again the master reads EIO. While CMD is still running the server takes
  that for a hangup, not the end, and reads again after 100 ms; once CMD has exited, EIO is
  the end of the session.

## Security

Without SSH, the BLE link's encryption is the only protection, and the login prompt's
password goes through it. So:

- **`--pty` needs `--require-bonding`:** only bonded centrals on encrypted links reach CMD.
  The server refuses to start without it. `--pty-insecure` overrides that for a bench test
  and prints a warning: anyone in range then gets CMD, unencrypted.
- **Run `/bin/login`, not a shell.** A shell would skip authentication.
- **login needs root,** so this mode can't use the unit's sandbox (`User=ble-ssh`, no
  capabilities). It needs a unit of its own, which the prototype doesn't have yet.

## Tested

Between two Raspberry Pi 5 (server: Ubuntu 26.04; client: Debian 13). Most tests ran the
server as a normal user with `/bin/sh` and `--pty-insecure`; the login tests ran it as root
with `--require-bonding` and the client bonded (`ble_ssh_client --pair`):

| Test | Result |
|------|--------|
| A session over GATT and over L2CAP | Shell on `/dev/pts/N`, `stty size` 24 80, `TERM=vt100` |
| `exit` | Marker shown, client ends without a warning, program reaped |
| Ctrl-C during `sleep 30` | Interrupted at once |
| Two L2CAP sessions at once | `/dev/pts/0` and `/dev/pts/1`, the second on the shared link |
| Client killed during `sleep 300` (GATT and L2CAP) | Shell and sleep end by SIGHUP, nothing left |
| `--nus`, GATT and L2CAP | Advertised as Nordic UART Service, sessions work; a client without `--nus` is told the service is missing |
| 20 sessions | fds 8 before and after, memory flat, no children or zombies left |
| `--require-bonding`, unbonded client | Refused over GATT and L2CAP, no program started |
| `--pty` without `--require-bonding` | Server refuses to start |
| Bonded, `--pty "/bin/login -f USER"`, GATT and L2CAP | Login shell (`-bash`) for the user on `/dev/pts/N`, parent `login`; `logout` ends the session with the marker, login exits with 0 |
| Bonded, `--pty /bin/login` | `login:` prompt shown; the client closing at the prompt ends login by SIGHUP |

## Not done

- **A password typed at `/bin/login`:** the tests can't type one; `/bin/login -f USER`
  (no password) was tested instead, and the prompt of plain `/bin/login`. Nor a phone app.
- **Window size:** fixed at 24×80. A client could send its size on an extra characteristic,
  which terminal apps wouldn't use.
- **A systemd unit** for the console mode, running as root with a sandbox that still allows
  login.
- **The Python client** has no `--nus` (and no `--l2cap`); it reaches a `--pty` server
  without `--nus` over the ble-ssh service.
