#!/usr/bin/env bash
# Install ble_ssh_server and its systemd service on a target over ssh, or remove it again.
#
# Runs on Linux and in Git Bash on Windows. The target needs systemd, sh, tar and gzip
# (BusyBox is fine), and root or sudo for the login user.
#
#   scripts/install-target.sh --release root@ssh-server            # newest release
#   scripts/install-target.sh --release main-latest root@ssh-server   # newest build of main
#   scripts/install-target.sh root@ssh-server                      # from ./build (Linux)
#   scripts/install-target.sh --artifact ble-ssh-arm64.zip root@ssh-server
#   scripts/install-target.sh --uninstall root@ssh-server
#
# Run with --help for all options.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: install-target.sh [options] [user@]host

Installs ble_ssh_server, ble-ssh-server.service and the sysusers.d file on the target,
creates the ble-ssh user, installs /etc/default/ble-ssh if the target has none, then
enables and (re)starts the service.

Where the files come from (pick one; the default is --build build):
  --release [TAG]    download ble-ssh-<arch>.tar.gz for the target's architecture from a
                     GitHub release and check it against SHA256SUMS: the newest release,
                     TAG (e.g. v0.1.3), or main-latest (rebuilt on every push to main).
                     Needs no GitHub login. BLE_SSH_RELEASE_URL=<url> uses a mirror that
                     holds the same files instead.
  --build DIR        a CMake build directory (staged with `cmake --install`, Linux only)
  --artifact PATH    a CI artifact: ble-ssh-<arch>.zip from a run's page on GitHub (needs a
                     login there), or its extracted folder
  --repo OWNER/NAME  GitHub repository for --release
                     (default: from `git remote get-url origin`)

What to do:
  --opts "OPTIONS"   set BLE_SSH_OPTS in /etc/default/ble-ssh, e.g. --opts "--no-advertise"
                     (without it an existing file is kept, else the repo's example is used)
  --client           also install ble_ssh_client
  --no-start         install, but do not enable or restart the service
  --uninstall        stop the service, remove what was installed and put back what was
                     there before (e.g. a buildroot package), enabled again if it was
  --purge            with --uninstall: also remove /etc/default/ble-ssh and the ble-ssh
                     user, if this script created them
  -n, --dry-run      show what would be installed and run the checks on the target,
                     change nothing there

ssh:
  -J JUMP            reach the target through a jump host (ssh -J)
  -o OPTION          extra ssh option, may be repeated (e.g. -o Port=2222)
  -h, --help         this help

The target is asked for its architecture first, and the binary must run there before
anything is changed. It also warns (without stopping) when bluetoothd is missing,
ReverseServiceDiscovery = false is not set in /etc/bluetooth/main.conf, or nothing listens
on the backend port (--host/--port in BLE_SSH_OPTS, default 127.0.0.1:22). Files that already exist (e.g. from a buildroot package) are saved
first, so --uninstall can put them back. The state is in /var/lib/ble-ssh-install.
EOF
}

die() { echo "install-target: $*" >&2; exit 1; }
info() { echo "==> $*"; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

source_kind=build
source_arg=build
release_tag=
opts=
have_opts=0
repo=
with_client=0
start=1
action=install
purge=0
dry_run=0
ssh_opts=(-o ConnectTimeout=15)
target=

while [ $# -gt 0 ]; do
    case $1 in
        --build) [ $# -ge 2 ] || die "--build needs a directory"
            source_kind=build; source_arg=$2; shift 2 ;;
        --artifact) [ $# -ge 2 ] || die "--artifact needs a path"
            source_kind=artifact; source_arg=$2; shift 2 ;;
        --release) source_kind=release
            # Optional tag: a word that is not an option and not the last argument (the target).
            if [ $# -ge 3 ] && [[ $2 != -* ]]; then release_tag=$2; shift; fi
            shift ;;
        --repo) [ $# -ge 2 ] || die "--repo needs OWNER/NAME"; repo=$2; shift 2 ;;
        --client) with_client=1; shift ;;
        --opts) [ $# -ge 2 ] || die "--opts needs a string"
            case $2 in *[\"\\\`\$]*) die "--opts: no quotes, backslashes or \$ please" ;; esac
            opts=$2; have_opts=1; shift 2 ;;
        --no-start) start=0; shift ;;
        --uninstall) action=uninstall; shift ;;
        --purge) purge=1; shift ;;
        -n|--dry-run) dry_run=1; shift ;;
        -J) [ $# -ge 2 ] || die "-J needs a host"; ssh_opts+=(-J "$2"); shift 2 ;;
        -o) [ $# -ge 2 ] || die "-o needs an option"; ssh_opts+=(-o "$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) [ -z "$target" ] || die "only one target, got '$target' and '$1'"
            target=$1; shift ;;
    esac
done
[ -n "$target" ] || { usage >&2; exit 2; }
[ $purge -eq 0 ] || [ "$action" = uninstall ] || die "--purge only goes with --uninstall"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# shellcheck disable=SC2029  # the command strings are meant to be built here
run_ssh() { ssh "${ssh_opts[@]}" "$target" "$@"; }

# ---------------------------------------------------------------------------------------------
# The part that runs on the target, as root. POSIX sh, so BusyBox systems work too.
# Arguments: install START CLIENT | uninstall PURGE. Its own directory holds the payload.
cat > "$work/remote.sh" <<'REMOTE'
#!/bin/sh
set -eu
# A non-login ssh shell, and sudo without secure_path, may leave out the sbin directories,
# where BusyBox keeps addgroup and adduser.
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin${PATH:+:$PATH}
export PATH
here=$(cd "$(dirname "$0")" && pwd)
state=/var/lib/ble-ssh-install
manifest=$state/files
unit=ble-ssh-server.service
die() { echo "target: $*" >&2; exit 1; }

[ -d /run/systemd/system ] || die "systemd is not running on this system"

backup=$state/backup        # files that were there before the first install (e.g. a package)
created=$state/created      # /etc/default/ble-ssh and the ble-ssh user, if this script made them
was_enabled=$state/was-enabled

listed() { [ -f "$2" ] && grep -qx "$1" "$2"; }
warn() { echo "warning: $*" >&2; }

# The BLE_SSH_OPTS the service will run with: --opts, else the existing file, else none.
effective_opts() {
    if [ -f "$here/opts" ]; then cat "$here/opts"
    elif [ -f /etc/default/ble-ssh ]; then
        # shellcheck disable=SC1091  # the target's file
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

# Put a file back from $backup, or remove it if there was none before.
restore_or_remove() {
    if [ -f "$backup$1" ]; then
        mv -f "$backup$1" "$1" && echo "restored $1"
    else
        rm -f "$1" && echo "removed $1"
    fi
}

# Everything that is checked before the system is changed. Leaves the cwd in the payload root.
preflight() {
    cd "$here/root"
    server=$(find . -path '*/bin/ble_ssh_server' -type f | head -n 1)
    [ -n "$server" ] || die "payload has no bin/ble_ssh_server"
    chmod 0755 "$server"
    # Check that the binary runs here before touching the system (architecture, glibc, GLib).
    # LD_BIND_NOW resolves every symbol at load, so a too new GLib fails here and not only
    # once the service reaches the missing function.
    if ! LD_BIND_NOW=1 "$server" --help > /dev/null 2>"$here/check.err"; then
        cat "$here/check.err" >&2
        die "ble_ssh_server does not run on this target; nothing was installed"
    fi
    prefix=$(dirname "$(dirname "${server#.}")")
    for other in /usr /usr/local; do
        [ "$other" != "$prefix" ] || continue
        if [ -e "$other/lib/systemd/system/$unit" ] && ! listed "$other/lib/systemd/system/$unit" "$manifest"; then
            echo "note: $other/lib/systemd/system/$unit is also installed (from a package?)." >&2
            echo "      systemd prefers the one in /usr/local; --uninstall brings the other back." >&2
        fi
    done
    check_runtime
}

case $1 in
check)
    # Dry run: read-only, also without root.
    preflight
    echo "ble_ssh_server runs on this target"
    ;;
install)
    start=$2
    preflight

    mkdir -p "$state" "$backup"
    touch "$created"
    # Remember how the service was set up before the first install, for --uninstall.
    [ -f "$was_enabled" ] || systemctl is-enabled "$unit" > "$was_enabled" 2>/dev/null || true

    if systemctl is-active --quiet "$unit"; then
        systemctl stop "$unit"
        echo "stopped $unit"
    fi

    : > "$manifest.new"
    find . -type f | sort | while IFS= read -r f; do
        f=${f#.}
        # A file that is not ours yet belongs to someone else: keep a copy to put back.
        if [ -e "$f" ] && ! listed "$f" "$manifest" && [ ! -e "$backup$f" ]; then
            mkdir -p "$(dirname "$backup$f")"
            cp -p "$f" "$backup$f"
            echo "saved $f"
        fi
        case $f in */bin/*) mode=0755 ;; *) mode=0644 ;; esac
        install -D -m "$mode" ".$f" "$f"
        echo "$f" >> "$manifest.new"
        echo "installed $f"
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

    if [ -f "$here/opts" ]; then
        f=/etc/default/ble-ssh
        if [ -e "$f" ] && ! listed "$f" "$manifest" && [ ! -e "$backup$f" ]; then
            mkdir -p "$(dirname "$backup$f")"
            cp -p "$f" "$backup$f"
            echo "saved $f"
        fi
        mkdir -p /etc/default
        {
            echo "# Written by install-target.sh --opts; see \`ble_ssh_server --help\`."
            printf 'BLE_SSH_OPTS="%s"\n' "$(cat "$here/opts")"
        } > "$f"
        chmod 0644 "$f"
        listed "$f" "$manifest" || echo "$f" >> "$manifest"
        echo "set $(tail -n 1 "$f") in $f"
    elif [ ! -e /etc/default/ble-ssh ]; then
        install -D -m 0644 "$here/ble-ssh.default" /etc/default/ble-ssh
        echo default >> "$created"
        echo "installed /etc/default/ble-ssh"
    else
        echo "kept existing /etc/default/ble-ssh: $(grep '^BLE_SSH_OPTS=' /etc/default/ble-ssh || true)"
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

    systemctl daemon-reload
    if [ "$start" = 1 ]; then
        # reenable: the unit may now come from another prefix than the old symlinks point to.
        systemctl reenable "$unit"
        # Type=notify: this returns once the GATT service is registered.
        if ! systemctl restart "$unit"; then
            journalctl -u "$unit" --no-pager -o cat -n 10 >&2 || true
            die "$unit did not start; --uninstall puts the earlier version back"
        fi
        systemctl --no-pager --lines=5 status "$unit" || true
    else
        echo "not started (--no-start); start with: systemctl enable --now $unit"
    fi
    ;;
uninstall)
    purge=$2
    [ -f "$manifest" ] || die "nothing to uninstall: $manifest not found (not installed by this script)"
    systemctl disable --now "$unit" 2>/dev/null || true
    while IFS= read -r f; do
        [ -n "$f" ] && restore_or_remove "$f"
    done < "$manifest"
    before=$(cat "$was_enabled" 2>/dev/null || true)
    if [ "$purge" = 1 ]; then
        # Only what this script created; a package's settings and user stay.
        if grep -qx default "$created" 2>/dev/null; then
            rm -f /etc/default/ble-ssh && echo "removed /etc/default/ble-ssh"
        fi
        if grep -qx user "$created" 2>/dev/null && id ble-ssh > /dev/null 2>&1; then
            if command -v userdel > /dev/null 2>&1; then userdel ble-ssh; else deluser ble-ssh; fi
            echo "removed user ble-ssh"
        fi
    fi
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
    ;;
*) die "unknown action $1" ;;
esac
REMOTE

# ---------------------------------------------------------------------------------------------
# Ask the target what it is before building or downloading anything.

info "Checking $target"
# shellcheck disable=SC2016  # runs on the target
probe=$(run_ssh 'echo "$(uname -m) $(id -u)"') || die "cannot log in to $target"
read -r target_arch target_uid <<< "$probe"
case $target_arch in
    aarch64|arm64) artifact_arch=arm64; elf_machine=183 ;;
    x86_64|amd64) artifact_arch=x86_64; elf_machine=62 ;;
    *) artifact_arch=; elf_machine= ;;
esac
if [ "$target_uid" = 0 ]; then sudo=; else sudo=sudo; fi
echo "    architecture $target_arch, ${sudo:+not }root${sudo:+ (using sudo)}"

# ---------------------------------------------------------------------------------------------
# Collect the /usr (or /usr/local) tree to install into $work/payload/root.

payload=$work/payload
mkdir -p "$payload"
cp "$work/remote.sh" "$payload/remote.sh"

have() { command -v "$1" > /dev/null 2>&1; }
# The Windows Store python3 stub exists but does not run.
have_python() { have python3 && python3 -c 'import json' > /dev/null 2>&1; }

extract_zip() {  # zip dir
    if have unzip; then unzip -q "$1" -d "$2"
    elif have_python; then python3 -m zipfile -e "$1" "$2"
    elif have bsdtar; then bsdtar -xf "$1" -C "$2"
    else die "need unzip, python3 or bsdtar to unpack $1"; fi
}

github_repo() {
    [ -z "$repo" ] || return 0
    local url
    url=$(git -C "$repo_root" remote get-url origin 2>/dev/null) || die "no git remote; use --repo OWNER/NAME"
    repo=$(sed -E 's#^.*github\.com[:/]([^/]+/[^/]+)$#\1#; s#\.git$##' <<< "$url")
    [[ $repo =~ ^[^/:]+/[^/:]+$ ]] || die "origin ($url) is not on GitHub; use --repo OWNER/NAME"
}

sha256_check() {  # runs in the directory with SHA256SUMS; checks the one file $1
    local line
    line=$(grep " \*\?$1\$" SHA256SUMS) || die "$1 is not in SHA256SUMS"
    if have sha256sum; then sha256sum -c --quiet <<< "$line"
    elif have shasum; then shasum -a 256 -c --quiet <<< "$line"
    else die "need sha256sum or shasum to check the download"; fi
}

fetch_release() {
    [ -n "$artifact_arch" ] || die "no release build for architecture $target_arch"
    local name=ble-ssh-$artifact_arch dir=$work/release base
    if [ -n "${BLE_SSH_RELEASE_URL:-}" ]; then
        base=${BLE_SSH_RELEASE_URL%/}
    else
        github_repo
        if [ -z "$release_tag" ]; then base=https://github.com/$repo/releases/latest/download
        else base=https://github.com/$repo/releases/download/$release_tag; fi
    fi
    mkdir -p "$dir"
    info "Downloading $name.tar.gz from $base" >&2
    curl -fsSL -o "$work/$name.tar.gz" "$base/$name.tar.gz" \
        || die "no $name.tar.gz at $base (is there a ${release_tag:-published} release?)"
    curl -fsSL -o "$work/SHA256SUMS" "$base/SHA256SUMS" || die "no SHA256SUMS at $base"
    (cd "$work" && sha256_check "$name.tar.gz") || die "$name.tar.gz does not match SHA256SUMS"
    tar -xzf "$work/$name.tar.gz" -C "$dir"
    echo "$dir"
}

# Print the directory that contains usr/ (the root of an installed tree).
find_root() {
    local f
    f=$(find "$1" -path '*/bin/ble_ssh_server' -type f | head -n 1)
    [ -n "$f" ] || die "no bin/ble_ssh_server under $1"
    # .../ROOT/usr/bin/ble_ssh_server or .../ROOT/usr/local/bin/ble_ssh_server
    local prefix=${f%/bin/ble_ssh_server}
    case $prefix in
        */usr/local) echo "${prefix%/usr/local}" ;;
        */usr) echo "${prefix%/usr}" ;;
        *) die "$f is not under a usr/ or usr/local/ prefix" ;;
    esac
}

if [ "$action" = install ]; then
    case $source_kind in
        build)
            [ -f "$source_arg/CMakeCache.txt" ] || die "$source_arg is not a CMake build directory (use --release, --build or --artifact)"
            have cmake || die "cmake not found (on Windows use --release)"
            info "Staging $source_arg with cmake --install"
            DESTDIR=$work/stage cmake --install "$source_arg" > /dev/null
            tree=$work/stage ;;
        artifact)
            if [ -d "$source_arg" ]; then tree=$source_arg
            elif [ -f "$source_arg" ]; then
                mkdir -p "$work/artifact"; extract_zip "$source_arg" "$work/artifact"; tree=$work/artifact
            else die "$source_arg not found"; fi ;;
        release) tree=$(fetch_release) ;;
    esac
    root=$(find_root "$tree")

    mkdir -p "$payload/root"
    cp -R "$root"/. "$payload/root/"
    # Only the server and what it needs; the client only on request.
    [ $with_client -eq 1 ] || find "$payload/root" -name ble_ssh_client -type f -delete
    find "$payload/root" -type f ! -path '*/bin/*' ! -path '*/lib/systemd/system/*' \
        ! -path '*/lib/sysusers.d/*' -delete
    cp "$repo_root/systemd/ble-ssh.default" "$payload/ble-ssh.default"
    [ $have_opts -eq 0 ] || printf '%s\n' "$opts" > "$payload/opts"

    # A wrong-architecture binary would also fail the check on the target; say why here.
    server=$(find "$payload/root" -path '*/bin/ble_ssh_server' -type f)
    if [ -n "$elf_machine" ]; then
        machine=$(od -An -t u2 -j 18 -N 2 "$server" | tr -d ' ')
        [ "$machine" = "$elf_machine" ] || die "the binary is not built for $target_arch (ELF machine $machine)"
    fi

    info "Files for $target"
    (cd "$payload/root" && find . -type f | sed 's#^\.#    #' | sort)
    if [ $dry_run -eq 1 ]; then
        # Run the checks on the target; they only read, so no sudo.
        remote_args=check sudo='' action=check
    else
        remote_args="install $start"
    fi
else
    if [ $dry_run -eq 1 ]; then
        info "Would remove from $target:"
        # shellcheck disable=SC2016  # runs on the target
        run_ssh 's=/var/lib/ble-ssh-install
            if [ ! -f "$s/files" ]; then echo "    nothing: not installed by this script"; exit 0; fi
            sed "s|^|    remove   |" "$s/files"
            [ -d "$s/backup" ] && (cd "$s/backup" && find . -type f | sed "s|^\.|    put back |")
            echo "    service before install: $(cat "$s/was-enabled" 2>/dev/null || echo unknown)"'
        exit 0
    fi
    remote_args="uninstall $purge"
fi

# ---------------------------------------------------------------------------------------------
# Copy the payload to a temporary directory on the target and run remote.sh there.

if [ "$action" = check ]; then info "Checks on $target (dry run)"; else info "${action^} on $target"; fi
# shellcheck disable=SC2016  # $d is expanded on the target
upload='d=$(mktemp -d) && gzip -dc | tar -xmf - -C "$d" && echo "$d"'
if [ -z "$sudo" ]; then
    # Root: one connection uploads and runs.
    tar -czf - -C "$payload" . | run_ssh "$upload > /dev/null && sh \"\$d/remote.sh\" $remote_args; rc=\$?; rm -rf \"\$d\"; exit \$rc"
else
    # sudo may ask for a password, which needs the terminal, so upload first, then run.
    dir=$(tar -czf - -C "$payload" . | run_ssh "$upload")
    tty=()
    [ -t 0 ] && tty=(-t)
    run_ssh "${tty[@]}" "sudo sh '$dir/remote.sh' $remote_args; rc=\$?; rm -rf '$dir'; exit \$rc"
fi
if [ "$action" = check ]; then echo "dry run: nothing changed on $target"; else info "Done"; fi
