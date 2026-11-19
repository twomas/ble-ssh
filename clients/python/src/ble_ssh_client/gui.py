"""ble-ssh-gui: find ble-ssh servers, open an SSH session over BLE, or save an ssh alias.

A thin tkinter front end over the command-line client: scanning uses the same code as
`ble-ssh-client --scan`, and connecting starts the system's ssh in a terminal window with
`ble-ssh-client` as ProxyCommand, exactly as you would by hand.
"""

from __future__ import annotations

import asyncio
import getpass
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading
from typing import List, Optional

try:
    import tkinter as tk
    from tkinter import messagebox, ttk
except ImportError:  # e.g. Debian/Ubuntu without python3-tk
    tk = None

from . import __version__
from bleak import BleakScanner

from .cli import client_command, default_alias, merge_advertisement, scan_rows, ssh_config_block

SCAN_SECONDS = 20.0
SSH_CONFIG = os.path.join(os.path.expanduser("~"), ".ssh", "config")
SETTINGS = os.path.join(os.environ.get("APPDATA") or os.environ.get("XDG_CONFIG_HOME")
                        or os.path.join(os.path.expanduser("~"), ".config"), "ble-ssh", "gui.json")


def load_users() -> dict:
    """Login user remembered per server."""
    try:
        with open(SETTINGS, encoding="utf-8") as settings:
            users = json.load(settings).get("users", {})
        return users if isinstance(users, dict) else {}
    except (OSError, ValueError, AttributeError):
        return {}


def save_user(target: str, user: str) -> None:
    users = load_users()
    if users.get(target) == user:
        return
    users[target] = user
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        with open(SETTINGS, "w", encoding="utf-8") as settings:
            json.dump({"users": users}, settings, indent=2)
    except OSError:
        pass  # a convenience only


def ssh_command(target: str, user: str) -> List[str]:
    """argv for an ssh session to TARGET over BLE."""
    destination = f"{user}@{target}" if user else target
    return ["ssh", "-o", f"ProxyCommand={client_command()} --progress {target}",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", destination]


def windows_cmd_line(switch: str, argv: List[str]) -> str:
    """`cmd.exe /k ARGV` (or /c) as a single command line, quoted the way ssh.exe parses it."""
    return f"cmd.exe {switch} {subprocess.list2cmdline(argv)}"


def open_in_terminal(argv: List[str]) -> None:
    """Run argv in a new terminal window that stays open afterwards, so errors can be read."""
    if sys.platform == "win32":
        # cmd /k keeps the window open after ssh exits. Pass one string: as a list, Popen would
        # quote the inner command again with \" escapes, which cmd doesn't understand.
        subprocess.Popen(windows_cmd_line("/k", argv), creationflags=subprocess.CREATE_NEW_CONSOLE)
        return
    line = shlex.join(argv) + "; echo; read -r -p 'Press Enter to close' _"
    if sys.platform == "darwin":
        script = line.replace("\\", "\\\\").replace('"', '\\"')
        subprocess.Popen(["osascript", "-e", f'tell application "Terminal" to do script "{script}"',
                          "-e", 'tell application "Terminal" to activate'])
        return
    shell = ["bash", "-c", line]
    # x-terminal-emulator is Debian's pick of the desktop's terminal; the rest cover other
    # distributions (ptyxis is the default on recent Ubuntu and Fedora, kgx is GNOME Console).
    terminals = (("x-terminal-emulator", ["-e"]), ("ptyxis", ["--"]), ("gnome-terminal", ["--"]),
                 ("kgx", ["--"]), ("konsole", ["-e"]), ("xfce4-terminal", ["-x"]), ("xterm", ["-e"]))
    for terminal, prefix in terminals:
        if shutil.which(terminal):
            subprocess.Popen([terminal, *prefix, *shell])
            return
    raise OSError("no terminal emulator found (tried " + ", ".join(name for name, _ in terminals) + ")")


def config_has_host(alias: str, path: Optional[str] = None) -> bool:
    path = path or SSH_CONFIG
    try:
        with open(path, encoding="utf-8", errors="replace") as config:
            return any(re.match(rf"^\s*Host\s+(.*\s)?{re.escape(alias)}(\s|$)", line, re.I) for line in config)
    except FileNotFoundError:
        return False


def append_to_config(block: str, path: Optional[str] = None) -> None:
    """Append BLOCK to the ssh config as plain ASCII/UTF-8 without BOM (ssh can't read UTF-16)."""
    path = path or SSH_CONFIG
    os.makedirs(os.path.dirname(path), exist_ok=True)
    prefix = ""
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as config:
            config.seek(-1, os.SEEK_END)
            prefix = "\n" if config.read(1) == b"\n" else "\n\n"
    with open(path, "a", encoding="utf-8", newline="\n") as config:
        config.write(prefix + block + "\n")


class App:
    def __init__(self, root: "tk.Tk") -> None:
        self.root = root
        self.rows: dict = {}  # address -> (rssi, name, address, is_ble_ssh), as shown
        self.seen: dict = {}  # everything heard in the current scan
        self.scanning = False
        self.after_scan = None  # e.g. a Connect waiting for the scan to stop
        root.title(f"BLE SSH {__version__}")
        root.minsize(520, 360)

        frame = ttk.Frame(root, padding=10)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(1, weight=1)

        top = ttk.Frame(frame)
        top.grid(row=0, column=0, sticky="ew")
        self.scan_button = ttk.Button(top, text="Scan", command=self.scan)
        self.scan_button.pack(side="left")
        self.show_all = tk.BooleanVar(value=False)
        ttk.Checkbutton(top, text="All named devices (for servers with --no-advertise)",
                        variable=self.show_all, command=self.render).pack(side="left", padx=10)

        columns = ("name", "address", "rssi", "ble_ssh")
        self.tree = ttk.Treeview(frame, columns=columns, show="headings", selectmode="browse", height=8)
        for column, heading, width, anchor in (("name", "Name", 200, "w"), ("address", "Address", 150, "w"),
                                               ("rssi", "RSSI", 60, "e"), ("ble_ssh", "ble-ssh", 70, "center")):
            self.tree.heading(column, text=heading)
            self.tree.column(column, width=width, anchor=anchor, stretch=column == "name")
        self.tree.grid(row=1, column=0, sticky="nsew", pady=(8, 8))
        self.tree.bind("<<TreeviewSelect>>", lambda _: self.on_select())
        self.tree.bind("<Double-1>", lambda _: self.connect())

        form = ttk.Frame(frame)
        form.grid(row=2, column=0, sticky="ew")
        form.columnconfigure(1, weight=1)
        self.target = tk.StringVar()
        self.user = tk.StringVar()
        self.alias = tk.StringVar()
        try:
            local_user = getpass.getuser()
        except Exception:
            local_user = "your local user name"
        for row, (label, variable) in enumerate((("Server (name or address)", self.target),
                                                 ("User", self.user), ("ssh alias", self.alias))):
            ttk.Label(form, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=2)
            ttk.Entry(form, textvariable=variable).grid(row=row, column=1, sticky="ew", pady=2)
        # Like ssh itself: an empty user means the local one, which is rarely right on the server.
        ttk.Label(form, text=f"empty = {local_user}", foreground="gray").grid(row=1, column=2, sticky="w", padx=(8, 0))
        self.target.trace_add("write", lambda *_: self.on_target_changed())

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(buttons, text="Connect", command=self.connect).pack(side="left")
        ttk.Button(buttons, text="Add to ~/.ssh/config", command=self.save_alias).pack(side="left", padx=6)
        ttk.Button(buttons, text="Copy ssh command", command=self.copy_command).pack(side="left")

        self.status = tk.StringVar(value="Press Scan to look for ble-ssh servers.")
        ttk.Label(frame, textvariable=self.status, foreground="gray").grid(row=4, column=0, sticky="w", pady=(8, 0))

    # Scanning runs bleak in its own thread and event loop so the window stays responsive. The
    # list fills in live: devices at the edge of range are often only heard now and then.
    def scan(self) -> None:
        if self.scanning:  # the button reads "Stop" while scanning
            self.stop_scan()
            return
        self.scanning = True
        self.scan_button.configure(text="Stop")
        self.seen = {}
        seen = self.seen
        self.scan_error: Optional[BaseException] = None
        self.scan_stop: Optional[tuple] = None  # (loop, event) once the scan thread runs
        self.stop_requested = False

        async def listen() -> None:
            stop = asyncio.Event()
            self.scan_stop = (asyncio.get_running_loop(), stop)
            if self.stop_requested:  # Stop was clicked before this thread got going
                return
            async with BleakScanner(lambda device, adv: merge_advertisement(seen, device, adv)):
                try:
                    await asyncio.wait_for(stop.wait(), SCAN_SECONDS)
                except asyncio.TimeoutError:
                    pass

        def worker() -> None:
            try:
                asyncio.run(listen())
            except Exception as error:  # bleak raises various types when Bluetooth is off
                self.scan_error = error

        thread = threading.Thread(target=worker, name="scan", daemon=True)
        thread.start()
        self.root.after(300, self.scan_tick, thread)

    def stop_scan(self) -> None:
        self.stop_requested = True
        if self.scan_stop is not None:
            loop, stop = self.scan_stop
            try:
                loop.call_soon_threadsafe(stop.set)
            except RuntimeError:  # the scan just finished on its own
                pass
        self.scan_button.state(["disabled"])  # until the scanner has actually stopped
        self.status.set("Stopping scan ...")

    def scan_tick(self, thread: threading.Thread) -> None:
        self.render()
        if thread.is_alive():
            self.root.after(500, self.scan_tick, thread)
            return
        self.scanning = False
        self.scan_button.configure(text="Scan")
        self.scan_button.state(["!disabled"])
        if self.scan_error is not None:
            self.status.set(f"Scan failed: {self.scan_error} (is Bluetooth on?)")
        else:
            self.render()
        pending, self.after_scan = self.after_scan, None
        if pending is not None:
            pending()

    def render(self) -> None:
        rows = scan_rows(getattr(self, "seen", {}), self.show_all.get())
        self.rows = {row[2]: row for row in rows}
        # Update rows in place: rebuilding would re-fire the selection and overwrite the form.
        shown = set(self.tree.get_children())
        for index, (rssi, name, address, is_ble_ssh) in enumerate(rows):
            values = (name, address, rssi, "yes" if is_ble_ssh else "-")
            if address in shown:
                self.tree.item(address, values=values)
                self.tree.move(address, "", index)
            else:
                self.tree.insert("", index, iid=address, values=values)
        gone = shown - set(self.rows)
        if gone:
            self.tree.delete(*gone)
        servers_found = [row for row in rows if row[3]]
        if servers_found and not self.tree.selection() and not self.target.get().strip():
            self.tree.selection_set(servers_found[0][2])  # preselect the strongest ble-ssh server only
        servers = sum(1 for row in rows if row[3])
        found = f"{servers} ble-ssh server(s)" + (f" among {len(rows)} devices" if self.show_all.get() else "")
        if self.scanning:
            if "disabled" not in self.scan_button.state():  # not while stopping
                self.status.set(f"Scanning ({SCAN_SECONDS:g} s) ... {found} so far")
        elif rows:
            self.status.set(f"Found {found}.")
        else:
            self.status.set("No ble-ssh servers found. Servers with --no-advertise show up under 'All named devices'.")

    def on_select(self) -> None:
        selection = self.tree.selection()
        if not selection or selection[0] not in self.rows:
            return
        _, name, address, is_ble_ssh = self.rows[selection[0]]
        # Advertised names are what the name filter matches; anything else is reached by address.
        # macOS hides addresses, but its device UUIDs work the same way.
        target = name if is_ble_ssh and name != "?" and sys.platform != "darwin" else address
        if self.target.get() != target:
            self.target.set(target)

    def on_target_changed(self) -> None:
        target = self.target.get().strip()
        self.alias.set(default_alias(target) if target else "")
        remembered = load_users().get(target)
        if remembered:
            self.user.set(remembered)

    def current_target(self) -> Optional[str]:
        target = self.target.get().strip()
        if not target:
            messagebox.showinfo("BLE SSH", "Pick a server from the list or type its name or address.")
            return None
        return target

    def connect(self) -> None:
        target = self.current_target()
        if target is None:
            return
        user = self.user.get().strip()
        if user:
            save_user(target, user)
        if self.scanning:
            # A scan running while the client connects makes Windows abort GATT discovery
            # ("the object has been closed"); finish the scan first.
            self.stop_scan()
            self.status.set("Stopping scan before connecting ...")
            self.after_scan = lambda: self.launch(target, user)
            return
        self.launch(target, user)

    def launch(self, target: str, user: str) -> None:
        try:
            open_in_terminal(ssh_command(target, user))
            self.status.set(f"Opened ssh to {target} in a new window.")
        except OSError as error:
            messagebox.showerror("BLE SSH", f"Could not start ssh: {error}")

    def save_alias(self) -> None:
        target = self.current_target()
        if target is None:
            return
        alias = self.alias.get().strip() or default_alias(target)
        if re.search(r"\s", alias):
            messagebox.showerror("BLE SSH", "The alias can't contain spaces.")
            return
        if config_has_host(alias):
            messagebox.showinfo("BLE SSH", f"{SSH_CONFIG} already has a 'Host {alias}' entry.\n"
                                           "Pick another alias, or edit the file.")
            return
        block = ssh_config_block(target, self.user.get().strip() or None, alias)
        if not messagebox.askokcancel("BLE SSH", f"Append to {SSH_CONFIG}:\n\n{block}"):
            return
        try:
            append_to_config(block)
        except OSError as error:
            messagebox.showerror("BLE SSH", f"Could not write {SSH_CONFIG}: {error}")
            return
        self.status.set(f"Saved. Connect from any terminal with: ssh {alias}")

    def copy_command(self) -> None:
        target = self.current_target()
        if target is None:
            return
        argv = ssh_command(target, self.user.get().strip())
        line = subprocess.list2cmdline(argv) if sys.platform == "win32" else shlex.join(argv)
        self.root.clipboard_clear()
        self.root.clipboard_append(line)
        self.status.set("ssh command copied to the clipboard.")


def main() -> int:
    if tk is None:
        print("ble-ssh-gui needs tkinter (Debian/Ubuntu: sudo apt install python3-tk)", file=sys.stderr)
        return 1
    root = tk.Tk()
    app = App(root)
    root.after(200, app.scan)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
