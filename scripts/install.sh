#!/bin/sh
# Install ble_ssh_server and its systemd service on this device, or remove it again.
#
# Runs on the device as root, from an unpacked release tarball:
#
#   tar -xzf ble-ssh-arm64.tar.gz
#   sudo ble-ssh-arm64/install.sh
#   sudo /var/lib/ble-ssh-install/install.sh --uninstall
#
# POSIX sh: BusyBox systems work too. scripts/install-target.sh runs this over ssh.

set -eu
# A non-login ssh shell, and sudo without secure_path, may leave out the sbin directories,
# where BusyBox keeps addgroup and adduser.
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin${PATH:+:$PATH}
export PATH

usage() {
    cat <<'EOF'
Usage: install.sh [options]
       install.sh --uninstall [--purge]

Installs ble_ssh_server, ble-ssh-server.service and the sysusers.d file from the usr/ tree
next to this script, creates the ble-ssh user, installs /etc/default/ble-ssh if there is
none, then enables and (re)starts the service. Run it again with a newer tree to update.

  --opts "OPTIONS"   set BLE_SSH_OPTS in /etc/default/ble-ssh, e.g. --opts "--no-advertise"
                     (without it an existing file is kept)
  --client           also install ble_ssh_client
  --no-start         install, but do not enable or restart the service
  --uninstall        stop the service, remove what was installed and put back what was
                     there before (e.g. a buildroot package), enabled again if it was
  --purge            with --uninstall: also remove /etc/default/ble-ssh and the ble-ssh
                     user, if this script created them
  -n, --dry-run      run the checks (or list what --uninstall would do), change nothing
  -h, --help         this help

Before it changes anything it checks that the binary runs here with every symbol resolved,
and it warns when bluetoothd is missing, ReverseServiceDiscovery = false is not set in
/etc/bluetooth/main.conf, or nothing listens on the backend port. Files that are already
there are saved first. The state, and a copy of this script for --uninstall, are in
/var/lib/ble-ssh-install.
EOF
}

here=$(cd "$(dirname "$0")" && pwd)
state=/var/lib/ble-ssh-install
manifest=$state/files
backup=$state/backup        # files that were there before the first install (e.g. a package)
created=$state/created      # /etc/default/ble-ssh and the ble-ssh user, if this script made them
was_enabled=$state/was-enabled
unit=ble-ssh-server.service

die() { echo "install.sh: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }
listed() { [ -f "$2" ] && grep -qx "$1" "$2"; }

action=install start=1 client=0 purge=0 dry_run=0 opts='' have_opts=0
while [ $# -gt 0 ]; do
    case $1 in
        --opts) [ $# -ge 2 ] || die "--opts needs a string"; opts=$2; have_opts=1; shift ;;
        --opts=*) opts=${1#*=}; have_opts=1 ;;
        --client) client=1 ;;
        --no-start) start=0 ;;
        --uninstall) action=uninstall ;;
        --purge) purge=1 ;;
        -n|--dry-run) dry_run=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
    shift
done
[ $purge -eq 0 ] || [ "$action" = uninstall ] || die "--purge only goes with --uninstall"
case $opts in *[\"\\\`\$]*) die "--opts: no quotes, backslashes or \$ please" ;; esac

[ -d /run/systemd/system ] || die "systemd is not running on this system"
[ $dry_run -eq 1 ] || [ "$(id -u)" = 0 ] || die "run as root (e.g. with sudo)"

# The BLE_SSH_OPTS the service will run with: --opts, else the existing file, else none.
effective_opts() {
    if [ $have_opts -eq 1 ]; then printf '%s\n' "$opts"
    elif [ -f /etc/default/ble-ssh ]; then
        # shellcheck disable=SC1091  # the device's file
        (. /etc/default/ble-ssh && printf '%s\n' "${BLE_SSH_OPTS:-}")
    fi
}

# Is something listening on TCP port $1 on loopback or on all addresses?
listening() {
    hex=$(printf '%04X' "$1")
    awk -v port="$hex" '
        $4 == "0A" {
            split($2, a, ":")
            if (a[2] == port && (a[1] == "0100007F" || a[1] ~ /^0+$/ || a[1] == "0000000000000000FFFF00000100007F" || a[1] == "00000000000000000000000001000000"))
                found = 1
        }
        END { exit !found }' /proc/net/tcp /proc/net/tcp6 2>/dev/null
}

# What the service needs at run time but this script does not install. Only warnings:
# a device may do these differently on purpose.
check_runtime() {
    if [ -z "$(systemctl list-unit-files --no-legend bluetooth.service 2>/dev/null)" ]; then
        warn "bluetooth.service not found: ble-ssh needs BlueZ (bluetoothd)"
    elif ! systemctl is-active --quiet bluetooth.service; then
        echo "note: bluetooth.service is not running; ble-ssh-server starts it"
    fi

    conf=/etc/bluetooth/main.conf
    if ! grep -Eqi '^[[:space:]]*ReverseServiceDiscovery[[:space:]]*=[[:space:]]*false' "$conf" 2>/dev/null; then
        warn "ReverseServiceDiscovery = false is not set under [General] in $conf;"
        warn "  clients may drop the link before the tunnel is up (README: BlueZ configuration)"
    fi

    # The backend sshd: --host/-H and --port/-p from the options, default 127.0.0.1:22.
    port=22 host=127.0.0.1
    # shellcheck disable=SC2046  # split the options into words, like systemd does
    set -- $(effective_opts)
    while [ $# -gt 0 ]; do
        case $1 in
            --port=*) port=${1#*=} ;;
            --port|-p) port=${2:-22}; [ $# -lt 2 ] || shift ;;
            -p?*) port=${1#-p} ;;
            --host=*) host=${1#*=} ;;
            --host|-H) host=${2:-}; [ $# -lt 2 ] || shift ;;
            -H?*) host=${1#-H} ;;
        esac
        shift
    done
    case $port in ''|*[!0-9]*) warn "cannot read the backend port '$port' from BLE_SSH_OPTS"; return 0 ;; esac
    case $host in
        127.0.0.1|localhost|::1)
            listening "$port" || warn "nothing listens on $host:$port; every BLE session will close at once (no sshd/dropbear?)" ;;
        *) echo "note: backend is $host:$port, not checked from here" ;;
    esac
}

# The files of the tree next to this script that get installed, relative to $here.
payload_files() {
    find usr -type f | sort | while IFS= read -r f; do
        case $f in
            */bin/ble_ssh_server|*/lib/systemd/system/*|*/lib/sysusers.d/*) echo "$f" ;;
            */bin/ble_ssh_client) [ $client -eq 0 ] || echo "$f" ;;
        esac
    done
}

# Everything that is checked before the system is changed. Leaves the cwd in $here.
preflight() {
    cd "$here"
    [ -d usr ] || die "no usr/ tree next to $0 (run the install.sh from an unpacked release)"
    server=$(find usr -path '*/bin/ble_ssh_server' -type f | head -n 1)
    [ -n "$server" ] || die "no usr/bin/ble_ssh_server next to $0"
    # Release archives and CI artifacts may come without the exec bit.
    chmod 0755 "$server" 2>/dev/null || true
    # Check that the binary runs here before touching the system (architecture, glibc, GLib).
    # LD_BIND_NOW resolves every symbol at load, so a too new GLib fails here and not only
    # once the service reaches the missing function.
    err=$(mktemp)
    if ! LD_BIND_NOW=1 "./$server" --help > /dev/null 2>"$err"; then
        cat "$err" >&2; rm -f "$err"
        die "ble_ssh_server does not run on this device; nothing was installed"
    fi
    rm -f "$err"
    prefix=/$(dirname "$(dirname "$server")")
    for other in /usr /usr/local; do
        [ "$other" != "$prefix" ] || continue
        if [ -e "$other/lib/systemd/system/$unit" ] && ! listed "$other/lib/systemd/system/$unit" "$manifest"; then
            echo "note: $other/lib/systemd/system/$unit is also installed (from a package?)." >&2
            echo "      systemd prefers the one in /usr/local; --uninstall brings the other back." >&2
        fi
    done
    check_runtime
}

# Put a file back from $backup, or remove it if there was none before.
restore_or_remove() {
    if [ -f "$backup$1" ]; then
        mv -f "$backup$1" "$1" && echo "restored $1"
    else
        rm -f "$1" && echo "removed $1"
    fi
}

# Keep a copy of a file that is not ours yet, to put back on --uninstall.
save_original() {
    if [ -e "$1" ] && ! listed "$1" "$manifest" && [ ! -e "$backup$1" ]; then
        mkdir -p "$(dirname "$backup$1")"
        cp -p "$1" "$backup$1"
        echo "saved $1"
    fi
}

write_default() {
    if [ -f "$here/ble-ssh.default" ]; then
        install -D -m 0644 "$here/ble-ssh.default" /etc/default/ble-ssh
    else
        mkdir -p /etc/default
        printf '%s\n' "# Options for ble-ssh-server.service, see \`ble_ssh_server --help\`." 'BLE_SSH_OPTS=""' \
            > /etc/default/ble-ssh
        chmod 0644 /etc/default/ble-ssh
    fi
}

install_files() {
    mkdir -p "$state" "$backup"
    touch "$created"
    # Remember how the service was set up before the first install, for --uninstall.
    [ -f "$was_enabled" ] || systemctl is-enabled "$unit" > "$was_enabled" 2>/dev/null || true

    if systemctl is-active --quiet "$unit"; then
        systemctl stop "$unit"
        echo "stopped $unit"
    fi

    : > "$manifest.new"
    payload_files | while IFS= read -r f; do
        save_original "/$f"
        case $f in */bin/*) mode=0755 ;; *) mode=0644 ;; esac
        install -D -m "$mode" "$f" "/$f"
        echo "/$f" >> "$manifest.new"
        echo "installed /$f"
    done
    # Files from an earlier install that this one no longer ships (e.g. another prefix).
    if [ -f "$manifest" ]; then
        while IFS= read -r f; do
            # Options set by an earlier --opts stay until --uninstall.
            if [ "$f" = /etc/default/ble-ssh ]; then echo "$f" >> "$manifest.new"; continue; fi
            listed "$f" "$manifest.new" || restore_or_remove "$f"
        done < "$manifest"
    fi
    mv "$manifest.new" "$manifest"

    f=/etc/default/ble-ssh
    if [ $have_opts -eq 1 ]; then
        # A file this script created is ours already: --purge removes it, nothing to put back.
        if ! grep -qx default "$created"; then
            save_original "$f"
            listed "$f" "$manifest" || echo "$f" >> "$manifest"
        fi
        mkdir -p /etc/default
        {
            echo "# Written by install.sh --opts; see \`ble_ssh_server --help\`."
            printf 'BLE_SSH_OPTS="%s"\n' "$opts"
        } > "$f"
        chmod 0644 "$f"
        echo "set $(tail -n 1 "$f") in $f"
    elif [ ! -e "$f" ]; then
        write_default
        echo default >> "$created"
        echo "installed $f"
    else
        echo "kept existing $f: $(grep '^BLE_SSH_OPTS=' "$f" || true)"
    fi

    if ! id ble-ssh > /dev/null 2>&1; then
        sysusers=$(grep '/sysusers.d/ble-ssh.conf$' "$manifest" || true)
        if command -v systemd-sysusers > /dev/null 2>&1 && [ -n "$sysusers" ]; then
            systemd-sysusers "$sysusers"
        elif command -v useradd > /dev/null 2>&1; then
            useradd --system --no-create-home --home-dir / --shell /sbin/nologin \
                --comment "SSH over Bluetooth LE" ble-ssh
        else
            addgroup -S ble-ssh && adduser -S -D -H -h / -s /sbin/nologin -G ble-ssh ble-ssh
        fi
        id ble-ssh > /dev/null 2>&1 || die "could not create the ble-ssh user"
        echo user >> "$created"
        echo "created user ble-ssh"
    fi

    # Keep this script, so --uninstall works without the release tarball.
    self=$here/$(basename "$0")
    if [ "$self" != "$state/install.sh" ]; then
        install -m 0755 "$self" "$state/install.sh"
    fi

    systemctl daemon-reload
    if [ $start -eq 1 ]; then
        # reenable: the unit may now come from another prefix than the old symlinks point to.
        systemctl reenable "$unit"
        # Type=notify: this returns once the GATT service is registered.
        if ! systemctl restart "$unit"; then
            journalctl -u "$unit" --no-pager -o cat -n 10 >&2 || true
            die "$unit did not start; $state/install.sh --uninstall puts the earlier version back"
        fi
        systemctl --no-pager --lines=5 status "$unit" || true
    else
        echo "not started (--no-start); start with: systemctl enable --now $unit"
    fi
}

uninstall_files() {
    [ -f "$manifest" ] || die "nothing to uninstall: $manifest not found (not installed by this script)"
    systemctl disable --now "$unit" 2>/dev/null || true
    while IFS= read -r f; do
        [ -n "$f" ] && restore_or_remove "$f"
    done < "$manifest"
    before=$(cat "$was_enabled" 2>/dev/null || true)
    if [ $purge -eq 1 ]; then
        # Only what this script created; a package's settings and user stay.
        if grep -qx default "$created" 2>/dev/null; then
            rm -f /etc/default/ble-ssh && echo "removed /etc/default/ble-ssh"
        fi
        if grep -qx user "$created" 2>/dev/null && id ble-ssh > /dev/null 2>&1; then
            if command -v userdel > /dev/null 2>&1; then userdel ble-ssh; else deluser ble-ssh; fi
            echo "removed user ble-ssh"
        fi
    fi
    # Also removes this script's copy there; the shell keeps reading it from the open file.
    rm -rf "$state"
    systemctl daemon-reload
    systemctl reset-failed "$unit" 2>/dev/null || true
    # A unit that was there before (a package's) is enabled and started again as it was.
    if [ -n "$(systemctl list-unit-files --no-legend "$unit" 2>/dev/null)" ]; then
        case $before in
            enabled|enabled-runtime)
                systemctl enable "$unit" && systemctl start "$unit"
                echo "re-enabled and started the earlier $unit"
                systemctl --no-pager --lines=3 status "$unit" || true ;;
            *) echo "the earlier $unit is back (not enabled, as before)" ;;
        esac
    fi
}

list_uninstall() {
    if [ ! -f "$manifest" ]; then echo "    nothing: not installed by this script"; return 0; fi
    sed 's|^|    remove   |' "$manifest"
    if [ -d "$backup" ]; then (cd "$backup" && find . -type f | sed 's|^\.|    put back |'); fi
    echo "    service before install: $(cat "$was_enabled" 2>/dev/null || echo unknown)"
}

case $action/$dry_run in
    install/1)
        preflight
        echo "would install:"; payload_files | sed 's|^|    /|'
        echo "ble_ssh_server runs on this device; dry run, nothing changed" ;;
    install/0) preflight; install_files ;;
    uninstall/1) list_uninstall ;;
    uninstall/0) uninstall_files ;;
esac
