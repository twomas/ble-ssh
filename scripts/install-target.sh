#!/usr/bin/env bash
# Install ble_ssh_server and its systemd service on a target over ssh, or remove it again.
#
# Runs on Linux and in Git Bash on Windows. The target needs systemd, sh, tar and gzip
# (BusyBox is fine), and root or sudo for the login user. The work on the target is done
# by install.sh (scripts/install.sh, also in every release tarball), which this sends there.
#
#   scripts/install-target.sh --release root@ssh-server            # newest release
#   scripts/install-target.sh --release main-latest root@ssh-server   # newest build of main
#   scripts/install-target.sh root@ssh-server                      # from ./build (Linux)
#   scripts/install-target.sh --artifact ble-ssh-arm64.zip root@ssh-server
#   scripts/install-target.sh --uninstall root@ssh-server
#
# The copy attached to each GitHub release works on its own, without the repository:
#
#   curl -fsSLO https://github.com/<owner>/<repo>/releases/latest/download/install-target.sh
#   bash install-target.sh root@ssh-server                         # installs that release
#
# Run with --help for all options.

set -euo pipefail

# Set by CI in the copy attached to a release: its repository and release tag.
baked_repo=
baked_tag=
if [ -n "$baked_tag" ]; then default_source="--release $baked_tag"; else default_source="--build build"; fi

usage() {
    cat <<EOF
Usage: install-target.sh [options] [user@]host

Installs ble_ssh_server, ble-ssh-server.service and the sysusers.d file on the target,
creates the ble-ssh user, installs /etc/default/ble-ssh if the target has none, then
enables and (re)starts the service. Run it again to update.

Where the files come from (pick one; the default is $default_source):
  --release [TAG]    download ble-ssh-<arch>.tar.gz for the target's architecture from a
                     GitHub release and check it against SHA256SUMS: the newest release,
                     TAG (e.g. v0.1.3), or main-latest (rebuilt on every push to main).
                     Needs no GitHub login. BLE_SSH_RELEASE_URL=<url> uses a mirror that
                     holds the same files instead.
  --build DIR        a CMake build directory (staged with \`cmake --install\`, Linux only)
  --artifact PATH    a CI artifact: ble-ssh-<arch>.zip from a run's page on GitHub (needs a
                     login there), or its extracted folder
  --repo OWNER/NAME  GitHub repository for --release
                     (default: ${baked_repo:-from \`git remote get-url origin\`})

What to do:
  --opts "OPTIONS"   set BLE_SSH_OPTS in /etc/default/ble-ssh, e.g. --opts "--no-advertise"
                     (without it an existing file is kept)
  --client           also install ble_ssh_client
  --no-start         install, but do not enable or restart the service
  --uninstall        stop the service, remove what was installed and put back what was
                     there before (e.g. a buildroot package), enabled again if it was
  --purge            with --uninstall: also remove /etc/default/ble-ssh and the ble-ssh
                     user, if this script created them
  -n, --dry-run      show what would be installed and run the checks on the target
                     (or list what --uninstall would do), change nothing there

ssh:
  -J JUMP            reach the target through a jump host (ssh -J)
  -o OPTION          extra ssh option, may be repeated (e.g. -o Port=2222)
  -h, --help         this help

The target is asked for its architecture first, and the binary must run there before
anything is changed. install.sh also warns (without stopping) when bluetoothd is missing,
ReverseServiceDiscovery = false is not set in /etc/bluetooth/main.conf, or nothing listens
on the backend port. Files that already exist (e.g. from a buildroot package) are saved
first, so --uninstall can put them back. The state, and a copy of install.sh, are in
/var/lib/ble-ssh-install on the target: "sudo /var/lib/ble-ssh-install/install.sh
--uninstall" works there too.
EOF
}

die() { echo "install-target: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# No BASH_SOURCE when the release copy is piped in: curl -fsSL .../install-target.sh | bash -s -- HOST
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)

source_kind=
source_arg=
release_tag=
opts=
have_opts=0
repo=$baked_repo
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
            case $2 in *[\"\'\\\`\$]*) die "--opts: no quotes, backslashes or \$ please" ;; esac
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
if [ -z "$source_kind" ]; then
    # The copy from a release installs that release; in the repository, the local build.
    if [ -n "$baked_tag" ]; then source_kind=release; release_tag=$baked_tag
    else source_kind=build; source_arg=build; fi
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# shellcheck disable=SC2029  # the command strings are meant to be built here
run_ssh() { ssh "${ssh_opts[@]}" "$target" "$@"; }

have() { command -v "$1" > /dev/null 2>&1; }
# The Windows Store python3 stub exists but does not run.
have_python() { have python3 && python3 -c 'import json' > /dev/null 2>&1; }

# ---------------------------------------------------------------------------------------------
# Ask the target what it is before building or downloading anything.

info "Checking $target"
# shellcheck disable=SC2016  # runs on the target
# ssh reads stdin; when this script is piped into bash, that is the rest of the script.
probe=$(run_ssh 'echo "$(uname -m) $(id -u)"' < /dev/null) || die "cannot log in to $target"
read -r target_arch target_uid <<< "$probe"
case $target_arch in
    aarch64|arm64) artifact_arch=arm64; elf_machine=183 ;;
    x86_64|amd64) artifact_arch=x86_64; elf_machine=62 ;;
    *) artifact_arch=; elf_machine= ;;
esac
if [ "$target_uid" = 0 ]; then sudo=; else sudo=sudo; fi
echo "    architecture $target_arch, ${sudo:+not }root${sudo:+ (using sudo)}"

# ---------------------------------------------------------------------------------------------
# Get the /usr (or /usr/local) tree to install.

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

# The payload sent to the target: install.sh, and to install also the usr/ tree and
# ble-ssh.default. Release tarballs and CI artifacts carry their own install.sh, which
# matches their binaries; otherwise the one next to this script is used.
payload=$work/payload
mkdir -p "$payload"
q() { printf " '%s'" "$1"; }    # quote one argument for the target's shell
args=

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

    cp -R "$root/usr" "$payload/"
    if [ -f "$root/install.sh" ]; then cp "$root/install.sh" "$payload/"
    elif [ -f "$script_dir/install.sh" ]; then cp "$script_dir/install.sh" "$payload/"
    else die "no install.sh in $tree or next to this script"; fi
    if [ -f "$root/ble-ssh.default" ]; then cp "$root/ble-ssh.default" "$payload/"
    elif [ -f "$repo_root/systemd/ble-ssh.default" ]; then cp "$repo_root/systemd/ble-ssh.default" "$payload/"
    fi

    # A wrong-architecture binary would also fail the check on the target; say why here.
    server=$(find "$payload/usr" -path '*/bin/ble_ssh_server' -type f | head -n 1)
    if [ -n "$elf_machine" ]; then
        machine=$(od -An -t u2 -j 18 -N 2 "$server" | tr -d ' ')
        [ "$machine" = "$elf_machine" ] || die "the binary is not built for $target_arch (ELF machine $machine)"
    fi

    [ $have_opts -eq 0 ] || args+="$(q --opts)$(q "$opts")"
    [ $with_client -eq 0 ] || args+=$(q --client)
    [ $start -eq 1 ] || args+=$(q --no-start)
else
    # Uninstall needs no files; send install.sh if we have one, else the target's copy runs.
    [ ! -f "$script_dir/install.sh" ] || cp "$script_dir/install.sh" "$payload/"
    args=$(q --uninstall)
    [ $purge -eq 0 ] || args+=$(q --purge)
fi
if [ $dry_run -eq 1 ]; then
    # Only reads on the target, so no sudo.
    args="$(q --dry-run)$args" sudo=''
fi

# ---------------------------------------------------------------------------------------------
# Copy the payload to a temporary directory on the target and run install.sh there.

if [ $dry_run -eq 1 ]; then info "Dry run on $target"; else info "${action^} on $target"; fi
# shellcheck disable=SC2016  # $d and $s are expanded on the target
upload='d=$(mktemp -d) && gzip -dc | tar -xmf - -C "$d" && echo "$d"'
# shellcheck disable=SC2016
pick='if [ -f "$d/install.sh" ]; then s=$d/install.sh; else s=/var/lib/ble-ssh-install/install.sh; fi
    [ -f "$s" ] || { echo "install-target: no install.sh here, and none on the target (not installed with it)" >&2; exit 1; }'
if [ -z "$sudo" ]; then
    # Root (or a dry run): one connection uploads and runs.
    tar -czf - -C "$payload" . | run_ssh "$upload > /dev/null && ( $pick; sh \"\$s\"$args ); rc=\$?; rm -rf \"\$d\"; exit \$rc"
else
    # sudo may ask for a password, which needs the terminal, so upload first, then run.
    dir=$(tar -czf - -C "$payload" . | run_ssh "$upload")
    cmd="d='$dir'; ( $pick; sudo sh \"\$s\"$args ); rc=\$?; rm -rf \"\$d\"; exit \$rc"
    if [ -t 0 ]; then run_ssh -t "$cmd"
    else run_ssh "$cmd" < /dev/null; fi    # piped in: sudo can then not ask for a password
fi
if [ $dry_run -eq 1 ]; then echo "dry run: nothing changed on $target"; else info "Done"; fi
