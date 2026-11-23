# AGENTS.md

Notes for coding agents (and people) working on ble-ssh. User-facing docs are in
[README.md](README.md) and [clients/python/README.md](clients/python/README.md); this file
covers what isn't obvious from them.

## Layout

| Path | What |
|------|------|
| `src/ble_ssh_server.c` | GATT server: one session per TX subscription, bridged to sshd over TCP |
| `src/ble_ssh_client.c` | Linux C client (BlueZ via bluez_inc), used as ssh `ProxyCommand` |
| `src/ble_ssh.h` | UUIDs and sizes shared by both C programs |
| `clients/python/` | Cross-platform client (`ble-ssh-client`) and tkinter GUI (`ble-ssh-gui`), built on bleak |
| `external/bluez_inc/` | Submodule: our fork of bluez_inc, the BlueZ D-Bus wrapper |
| `systemd/` | Unit template, `/etc/default` example, sysusers |
| `buildroot/` | br2-external tree with the `ble-ssh` package |
| `scripts/install.sh` | Runs on the device (POSIX sh): installs the `usr/` tree next to it, updates, uninstalls. Shipped in every release tarball |
| `scripts/install-target.sh` | Runs `install.sh` on a device over ssh, from a release, a build or an artifact. Each release has a copy that installs that release |

## Build and check

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror   # CI builds with -Werror
cmake --build build
./build/ble_ssh_server --help && ./build/ble_ssh_client --help

pip install ./clients/python
python -c "import ble_ssh_client.cli, ble_ssh_client.gui"
```

CI ([.github/workflows/build.yml](.github/workflows/build.yml)) builds the C code on x86_64
and arm64, on the newest Ubuntu and on Ubuntu 22.04. The 22.04 builds are the artifacts (a
`/usr` tree): they must need no newer glibc than 2.35, and CMake caps the GLib API at
`BLE_SSH_GLIB_API` (2.58), so they also run on older targets. Raising either is a decision,
not a fix for a build error. After a push, the `release` job publishes the same trees with
`install.sh` as `ble-ssh-<arch>.tar.gz`, plus `install-target.sh`, the Python client's wheel
(on main its version gets `+g<commit>`) and `SHA256SUMS`: a tag
`vX.Y.Z` gets its own release, and main updates the pre-release `main-latest`. CI also
shellchecks both scripts, and
smoke-tests the Python client on Linux, Windows and macOS with the oldest and newest
supported Python (3.10 and 3.14). The runners have no Bluetooth, so
anything that touches BLE has to be tested on real hardware (see below).

## Rules

- **bluez_inc wraps all of D-Bus.** ble-ssh calls `binc_*` functions only. When something
  is missing or wrong in BlueZ handling, fix or extend bluez_inc, commit it there first, then
  update the submodule pointer here in a separate commit. The server's only D-Bus code is
  the bus connection and watching the `org.bluez` name.
- **No private names in the repo.** No real MAC addresses, host names, user names or device
  names in code, docs, tests or screenshots: use `AA:BB:CC:DD:EE:FF`, `ssh-server`, `pi`.
  Screenshots show made-up devices.
- **Keep the clients' command lines in step.** The C and Python clients take the same
  options for the same things; a new option goes in both READMEs' option tables.
- **Python 3.10+.** Use no newer syntax, and keep `requires-python` and the CI matrix in step.
- **Versions:** `project.version` in `pyproject.toml`, `__version__` in
  `clients/python/src/ble_ssh_client/__init__.py`, the git tag `vX.Y.Z`, and
  `BLE_SSH_VERSION` in `buildroot/package/ble-ssh/ble-ssh.mk` move together. The release job
  refuses a tag that does not match all three.

## Design points that are easy to break

Read [Sessions and link reuse](README.md#sessions-and-link-reuse) before changing session
handling. In short:

- A session is exactly one TX subscription. The server connects to sshd on the client's
  **first write**, not on subscribe, because clients subscribe briefly and then resubscribe.
- The Python client toggles TX notifications on → off → on, because Windows reuses a link
  that may still be subscribed. It unsubscribes before disconnecting, with timeouts, because
  WinRT calls can hang while a link goes away.
- Transient connect errors are retried until `--timeout`: the WinRT "object has been
  closed" error (-2147483629), BlueZ "in progress" / `Not Connected`, and `CancelledError`
  from an aborted GATT discovery. A real cancellation of the task is not retried.
- `--adapter-name` follows the adapter's Alias at runtime (via bluez_inc's alias callback),
  so the server and the program that sets the name can start in any order.

## Testing on hardware

Needs a Linux machine running the server and a client within BLE range. The machine,
address and login are your own; don't commit them.

- **Run one BLE client at a time.** A scan or a second connection running alongside makes
  Windows fail with "object has been closed". Close the GUI before scripted tests.
- **Use `ssh -n` for non-interactive commands on Windows.** With a console as stdin, Windows
  OpenSSH sometimes never exits after the remote command finishes, over plain TCP too, so
  this is not a ble-ssh bug. Also give every scripted ssh a hard timeout.
- **Server logs:** run the server in the foreground with `-v`. Through a pipe its output
  is block-buffered, so use `stdbuf -oL -eL` to get lines as they happen. The
  "bytes in/out" in the session-closed log line count traffic to and from sshd.
- **Restore what you change.** If you stop the service or change the adapter Alias for a
  test, start the service and set the Alias back afterwards, also when the test fails.
- **Raspberry Pi:** occasionally the server advertises from a random address, without its
  name and not connectable. Registering the advertisement again (restarting the server, or
  an Alias change with `--adapter-name`) brings it back. The cause is not known yet.
- Useful checks after a long run: the server's open fds (`ls /proc/<pid>/fd | wc -l`) and
  RSS, compared with before the run, and error lines in `journalctl -u ble-ssh-server`.
