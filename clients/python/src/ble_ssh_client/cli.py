"""ble-ssh-client: connect stdin/stdout to a ble_ssh_server over Bluetooth LE.

Meant to be used as an ssh ProxyCommand:

    ssh -o ProxyCommand="ble-ssh-client AA:BB:CC:DD:EE:FF" user@host
    ssh -o ProxyCommand="ble-ssh-client ssh-raspberrypi" user@host

stdout carries the tunnel, so every diagnostic goes to stderr.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime
import os
import re
import sys
import threading
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

from . import RX_UUID, SERVICE_UUID, TX_UUID, __version__

ATT_OVERHEAD = 3
MIN_CHUNK = 20
MAX_CHUNK = 512
READ_CHUNK = 4096
STDIN_QUEUE_DEPTH = 16  # x READ_CHUNK bytes buffered before stdin reads block

MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")
UUID_RE = re.compile(r"^[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}$")  # macOS device ids

verbose = False
show_progress = False


def progress(message: str) -> None:
    """A short status line for people watching the terminal (--progress); -v says it with timestamps."""
    if verbose:
        debug(message)
    elif show_progress:
        log(message)


def log(message: str) -> None:
    try:
        print(f"ble-ssh-client: {message}", file=sys.stderr, flush=True)
    except (OSError, ValueError):  # ssh already closed our stderr while we tear down
        pass


def debug(message: str) -> None:
    if verbose:
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        log(f"{stamp} {message}")


def is_address(target: str) -> bool:
    return bool(MAC_RE.match(target) or UUID_RE.match(target))


async def find_device(target: str, timeout: float) -> Optional[BLEDevice]:
    if is_address(target):
        # Unfiltered, so servers started with --no-advertise (reached through another
        # program's advertisement) are found too.
        debug(f"scanning for {target}")
        return await BleakScanner.find_device_by_address(target, timeout=timeout)
    debug(f"scanning for {target} (ble-ssh advertisers only)")
    return await BleakScanner.find_device_by_filter(
        lambda device, adv: target in (device.name, adv.local_name) and SERVICE_UUID in adv.service_uuids,
        timeout=timeout,
        service_uuids=[SERVICE_UUID],
    )


def write_stdout(data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(1, view)
        view = view[written:]


def start_stdin_reader(loop: asyncio.AbstractEventLoop, queue: "asyncio.Queue[Optional[bytes]]") -> None:
    # asyncio can't read pipes on Windows, so read stdin in a thread on every platform. The
    # bounded queue blocks the thread when BLE is the bottleneck: that is our back-pressure.
    def reader() -> None:
        while True:
            try:
                data = os.read(0, READ_CHUNK)
            except OSError:
                data = b""
            asyncio.run_coroutine_threadsafe(queue.put(data or None), loop).result()
            if not data:
                return

    threading.Thread(target=reader, name="stdin", daemon=True).start()


async def chunk_size(client: BleakClient) -> int:
    # bleak's BlueZ backend reports the default MTU (23) until it is fetched explicitly, which
    # would cap every write at 20 bytes. Windows and macOS report the negotiated value directly.
    acquire_mtu = getattr(getattr(client, "_backend", None), "_acquire_mtu", None)
    if acquire_mtu is not None:
        try:
            await acquire_mtu()
        except Exception as error:  # older BlueZ without AcquireWrite/AcquireNotify MTU
            debug(f"could not read MTU from BlueZ ({error}), using the default")
    rx = client.services.get_characteristic(RX_UUID)
    sizes = [client.mtu_size - ATT_OVERHEAD, getattr(rx, "max_write_without_response_size", 0)]
    debug(f"mtu {client.mtu_size}, max write without response {sizes[1]}")
    return max(MIN_CHUNK, min(MAX_CHUNK, max(sizes)))


async def pair_and_reconnect(device: BLEDevice, timeout: float) -> None:
    """Bond once, then drop the link.

    bluetoothd on the server only marks a new bond once that first link drops, so a server
    started with --require-bonding would reject this link anyway. The caller connects again,
    and that link is encrypted with the stored keys.
    """
    async with BleakClient(device, timeout=timeout) as client:
        debug(f"pairing with {device.address}")
        try:
            await client.pair()
        except NotImplementedError:
            log("this platform pairs on demand (macOS); continuing without explicit pairing")
            return
    debug("bonded, reconnecting so the server sees the bond")
    await asyncio.sleep(1.0)


class NoService(Exception):
    """The device is up but doesn't run ble_ssh_server: retrying won't help."""


async def subscribe(client: BleakClient, on_notify) -> int:
    if client.services.get_service(SERVICE_UUID) is None:
        raise NoService(f"{client.address} does not offer the ble-ssh service (is ble_ssh_server running there?)")
    # A session on the server is one TX subscription. A link left behind by a previous client
    # (Windows reuses them) may still be subscribed, and bluetoothd doesn't report a CCCD write
    # that changes nothing. Toggling on -> off -> on guarantees the server sees the "off"
    # (ending any leftover session) and then a fresh "on" (starting ours).
    await client.start_notify(TX_UUID, lambda _, __: None)  # anything here belongs to no one
    await client.stop_notify(TX_UUID)
    await client.start_notify(TX_UUID, on_notify)
    return await chunk_size(client)


async def open_tunnel(args: argparse.Namespace, disconnected: asyncio.Event, on_notify):
    """Find, connect and subscribe, retrying everything until the timeout.

    Right after a previous connection the old link may still be going away: Windows fails
    with "the object has been closed", BlueZ with "in progress", or the new link connects
    and then drops ("Not Connected") once the old teardown completes. All of these are
    transient, so start over from the scan.
    """
    loop = asyncio.get_running_loop()
    deadline = loop.time() + args.timeout
    last_error: Optional[BaseException] = None
    active: list = [None]  # the attempt whose disconnect matters

    def on_disconnect(which: BleakClient) -> None:
        # A failed earlier attempt may report its disconnect late; only the current one counts.
        if which is active[0]:
            disconnected.set()

    attempt = 0
    while (remaining := deadline - loop.time()) > 0:
        attempt += 1
        if attempt == 1:
            progress(f"looking for {args.target} over Bluetooth ...")
        device = await find_device(args.target, remaining)
        if device is None:
            break
        debug(f"found {device.address} ({device.name or '?'})")
        progress(f"connecting to {device.address}" + (f" (attempt {attempt})" if attempt > 1 else "") + " ...")
        client = BleakClient(device, timeout=max(remaining, 1.0), disconnected_callback=on_disconnect)
        active[0] = client
        disconnected.clear()
        try:
            await client.connect()
            size = await subscribe(client, on_notify)
            if client.is_connected:
                # On BlueZ, connect() retries le-connection-abort-by-local itself and reports
                # the dropped attempt through our disconnected_callback; that one is history.
                disconnected.clear()
            return client, size
        except NoService as error:
            log(str(error))
            await client.disconnect()
            return None
        except (BleakError, asyncio.TimeoutError, OSError, asyncio.CancelledError) as error:
            # WinRT reports an aborted GATT operation (e.g. while another program scans) as
            # CancelledError. Retry that, but let a real cancellation of this task through.
            current = asyncio.current_task()
            if isinstance(error, asyncio.CancelledError) and current is not None and getattr(current, "cancelling", lambda: 0)():
                raise
            last_error = error if str(error) else BleakError("GATT operation cancelled")
            if verbose:
                debug(f"setup failed ({last_error}), retrying")
            else:
                progress(f"not yet ({last_error}), retrying ...")
            active[0] = None  # whatever this attempt reports from now on is history
            try:
                await asyncio.wait_for(client.disconnect(), 3.0)
            except (BleakError, OSError, asyncio.TimeoutError):
                pass
            await asyncio.sleep(1.0)
    log(f"connect failed: {last_error}" if last_error else f"timed out after {args.timeout:g} s waiting for {args.target}")
    if last_error is not None and "-2147483629" in str(last_error):  # WinRT: "the object has been closed"
        log("Windows kept closing the connection. Another program using Bluetooth at the same time "
            "(a scan, or another connection to this device) is the usual cause; close it and try again.")
    elif last_error is None:
        log("is the server in range and running? Servers started with --no-advertise must be reached by address.")
    return None


async def run(args: argparse.Namespace) -> int:
    loop = asyncio.get_running_loop()

    if args.pair:
        device = await find_device(args.target, args.timeout)
        if device is None:
            log(f"timed out after {args.timeout:g} s waiting for {args.target}")
            return 255
        await asyncio.wait_for(pair_and_reconnect(device, args.timeout), args.timeout * 2)

    disconnected = asyncio.Event()
    if sys.platform != "win32":
        # ssh sends SIGHUP to its ProxyCommand when the session ends; treat it (and SIGTERM)
        # like a closed link so the finally-block below still unsubscribes and disconnects.
        import signal

        for sig in (signal.SIGHUP, signal.SIGTERM):
            loop.add_signal_handler(sig, disconnected.set)
    def on_notify(_: object, data: bytearray) -> None:
        try:
            write_stdout(bytes(data))
        except OSError:  # ssh went away
            disconnected.set()

    opened = await open_tunnel(args, disconnected, on_notify)
    if opened is None:
        return 255
    client, size = opened

    try:
        debug(f"tunnel up (chunk {size} bytes)")
        if not verbose:
            progress("connected, starting the SSH session")

        queue: asyncio.Queue[Optional[bytes]] = asyncio.Queue(maxsize=STDIN_QUEUE_DEPTH)
        start_stdin_reader(loop, queue)

        async def pump() -> None:
            # One write at a time; awaiting it is what paces stdin to the BLE link.
            while (data := await queue.get()) is not None:
                for offset in range(0, len(data), size):
                    await client.write_gatt_char(RX_UUID, data[offset:offset + size], response=args.reliable)
            debug("stdin closed")

        pump_task = asyncio.ensure_future(pump())
        link_task = asyncio.ensure_future(disconnected.wait())
        done, _ = await asyncio.wait({pump_task, link_task}, return_when=asyncio.FIRST_COMPLETED)
        if pump_task in done and pump_task.exception() is not None:
            log(f"write failed: {pump_task.exception()}")
            return 255
        return 0
    except (BleakError, OSError) as error:
        log(str(error))
        return 255
    finally:
        if client.is_connected:
            # Unsubscribe before disconnecting: that is what ends the session on the server.
            # Windows may keep the BLE link up for a while after disconnect() and reuse it for
            # the next connection, so without this the server's session (and sshd) would live
            # on and receive the next client's bytes.
            # WinRT calls can hang when the link is going away; never let teardown block exit.
            debug("unsubscribing and disconnecting")
            try:
                await asyncio.wait_for(client.stop_notify(TX_UUID), 3.0)
            except (BleakError, OSError, ValueError, asyncio.TimeoutError):
                debug("unsubscribe failed or timed out")
            try:
                await asyncio.wait_for(client.disconnect(), 5.0)
            except (BleakError, OSError, asyncio.TimeoutError):
                debug("disconnect failed or timed out")
            debug("disconnected")


async def scan_devices(seconds: float, show_all: bool) -> list:
    """Nearby ble-ssh servers (or, with show_all, every named BLE device), strongest first,
    as (rssi, name, address, is_ble_ssh) tuples."""
    seen: dict = {}
    async with BleakScanner(lambda device, adv: merge_advertisement(seen, device, adv)):
        await asyncio.sleep(seconds)
    return scan_rows(seen, show_all)


def merge_advertisement(seen: dict, device: BLEDevice, adv) -> None:
    """Fold one advertisement into SEEN (address -> entry).

    Many devices alternate between packets with and without their name or service list (for
    example when they multiplex several advertisements), so keeping only the last packet
    would drop them at random.
    """
    entry = seen.setdefault(device.address, {"name": None, "ble_ssh": False, "rssi": adv.rssi})
    entry["name"] = adv.local_name or device.name or entry["name"]
    entry["ble_ssh"] = entry["ble_ssh"] or SERVICE_UUID in (u.lower() for u in adv.service_uuids)
    entry["rssi"] = adv.rssi


def scan_rows(seen: dict, show_all: bool) -> list:
    rows = [(e["rssi"], e["name"] or "?", address, e["ble_ssh"]) for address, e in list(seen.items())
            if e["ble_ssh"] or (show_all and e["name"])]
    rows.sort(reverse=True)
    return rows


async def scan(seconds: float, show_all: bool) -> int:
    """List nearby ble-ssh servers (or, with --all, every named BLE device) by signal strength."""
    rows = await scan_devices(seconds, show_all)
    if not rows:
        print(f"No ble-ssh servers found in {seconds:g} s." if not show_all else f"No named devices found in {seconds:g} s.")
        if not show_all:
            print("Servers started with --no-advertise don't show up here; try --scan --all and connect by address.")
        return 1
    width = max(len(r[1]) for r in rows)
    print(f"{'NAME':<{width}}  {'ADDRESS':<17}  RSSI" + ("  BLE-SSH" if show_all else ""))
    for rssi, name, address, is_ble_ssh in rows:
        marker = ("  yes" if is_ble_ssh else "  -") if show_all else ""
        print(f"{name:<{width}}  {address:<17}  {rssi:>4}{marker}")
    if show_all:
        print("\nBLE-SSH = advertises the ble-ssh service. A server started with --no-advertise runs behind\n"
              "another program's advertisement: connect to it by address.")
    return 0


def client_command() -> str:
    """How ssh should start this client, as a ProxyCommand prefix."""
    import shutil

    exe = shutil.which("ble-ssh-client")
    if exe is None:
        # Not on PATH (e.g. a venv that isn't activated): use the script next to the interpreter.
        scripts = os.path.dirname(sys.executable)
        for candidate in ("ble-ssh-client.exe", "ble-ssh-client"):
            if os.path.exists(os.path.join(scripts, candidate)):
                exe = os.path.join(scripts, candidate)
                break
    if exe is None:
        return f'"{sys.executable}" -m ble_ssh_client'
    exe = os.path.abspath(exe)
    return f'"{exe}"' if " " in exe else exe


def default_alias(target: str) -> str:
    base = re.sub(r"^ssh-", "", target) if not is_address(target) else target.replace(":", "").lower()
    return re.sub(r"[^A-Za-z0-9._-]", "-", base) + "-ble"


def ssh_config_block(target: str, user: Optional[str], alias: Optional[str]) -> str:
    """A ~/.ssh/config block (Host ALIAS) that reaches TARGET over BLE."""
    alias = alias or default_alias(target)
    lines = [
        f"Host {alias}",
        f"    HostName {target}",
    ]
    if user:
        lines.append(f"    User {user}")
    lines += [
        f"    ProxyCommand {client_command()} --progress {target}",
        "    # BLE is slow to (re)connect; notice a dead link instead of hanging",
        "    ServerAliveInterval 15",
        "    ServerAliveCountMax 3",
    ]
    return "\n".join(lines)


def ssh_config(target: str, user: Optional[str], alias: Optional[str]) -> int:
    """Print a ~/.ssh/config block that reaches TARGET over BLE."""
    alias = alias or default_alias(target)
    print(ssh_config_block(target, user, alias))
    again = f"ble-ssh-client --ssh-config {target}{' --user ' + user if user else ''}"
    if sys.platform == "win32":
        # Windows PowerShell's ">>" writes UTF-16, which ssh can't read.
        log(f"append it with:  {again} | Add-Content -Encoding ascii $HOME\\.ssh\\config")
    else:
        log(f"append it with:  {again} >> ~/.ssh/config")
    log(f"then connect with:  ssh {alias}")
    return 0


def parse_args(argv: Optional[list] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="ble-ssh-client",
        description="SSH over BLE, client side. Use as: ssh -o ProxyCommand=\"ble-ssh-client ADDRESS\" user@host",
    )
    parser.add_argument("target", metavar="ADDRESS|NAME", nargs="?",
                        help="server's Bluetooth address (on macOS: the CoreBluetooth device UUID) or advertised name")
    parser.add_argument("-s", "--scan", action="store_true",
                        help="list nearby ble-ssh servers and exit (scans for --timeout seconds, default 5 here)")
    parser.add_argument("-a", "--all", action="store_true",
                        help="with --scan: list every named BLE device, marking the ones that advertise ble-ssh")
    parser.add_argument("--ssh-config", action="store_true",
                        help="print a ~/.ssh/config block for ADDRESS|NAME and exit (append it with >>)")
    parser.add_argument("--user", help="with --ssh-config: login user on the server")
    parser.add_argument("--alias", help="with --ssh-config: Host alias (default: <name>-ble)")
    parser.add_argument("-t", "--timeout", type=float, default=None,
                        help="seconds to wait for the server (default 20), or to scan with --scan (default 5)")
    parser.add_argument("-p", "--pair", action="store_true",
                        help="bond with the server first (needed once for servers started with --require-bonding)")
    parser.add_argument("-r", "--reliable", action="store_true", help="use write-with-response (slower)")
    parser.add_argument("-v", "--verbose", action="store_true", help="print detailed, timestamped progress to stderr")
    parser.add_argument("--progress", action="store_true",
                        help="print a short status line per step (looking, connecting, connected) to stderr")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    args = parser.parse_args(argv)
    if args.timeout is None:
        args.timeout = 5.0 if args.scan else 20.0
    if not args.scan and not args.target:
        parser.error("ADDRESS|NAME is required (or use --scan to find servers)")
    if args.all and not args.scan:
        parser.error("--all only makes sense with --scan")
    if (args.user or args.alias) and not args.ssh_config:
        parser.error("--user and --alias only make sense with --ssh-config")
    return args


WORKER_ENV = "BLE_SSH_CLIENT_WORKER"


def run_detached_worker(argv: list) -> int:
    """Windows: run the real client in a child process that outlives us.

    When the SSH session ends, Windows' ssh.exe kills its ProxyCommand with TerminateProcess,
    which can't be caught, so the client would never unsubscribe or disconnect. Windows then
    keeps the BLE link (and our subscription) alive for a while, and the next connection runs
    into it. ssh only kills this thin parent; the worker sees stdin close and tears the BLE
    connection down properly.
    """
    import subprocess

    env = dict(os.environ, **{WORKER_ENV: "1"})
    command = [sys.executable, "-m", "ble_ssh_client", *argv]
    flags = subprocess.CREATE_NEW_PROCESS_GROUP
    try:  # get out of ssh's job object too, in case it kills the whole job
        child = subprocess.Popen(command, stdin=0, stdout=1, stderr=2, env=env,
                                 creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB)
    except OSError:
        child = subprocess.Popen(command, stdin=0, stdout=1, stderr=2, env=env, creationflags=flags)
    try:
        return child.wait()
    except KeyboardInterrupt:
        return 130


def main(argv: Optional[list] = None) -> int:
    global verbose, show_progress
    argv = sys.argv[1:] if argv is None else argv
    args = parse_args(argv)
    verbose = args.verbose
    show_progress = args.progress
    if args.ssh_config:
        return ssh_config(args.target, args.user, args.alias)
    if args.scan:  # interactive use, not a ProxyCommand: no worker process or binary stdio
        try:
            return asyncio.run(scan(args.timeout, args.all))
        except KeyboardInterrupt:
            return 130
        except (BleakError, OSError) as error:
            log(f"scan failed: {error}")
            return 255
    if sys.platform == "win32":
        if os.environ.get(WORKER_ENV) != "1":
            sys.stderr.flush()
            os._exit(run_detached_worker(argv))
        import msvcrt

        msvcrt.setmode(0, os.O_BINARY)
        msvcrt.setmode(1, os.O_BINARY)
    try:
        code = asyncio.run(run(args))
    except KeyboardInterrupt:
        code = 130
    except asyncio.TimeoutError:
        log(f"timed out after {args.timeout:g} s")
        code = 255
    sys.stderr.flush()
    # The stdin thread may still be blocked in read(); don't wait for it.
    os._exit(code)


if __name__ == "__main__":
    main()
