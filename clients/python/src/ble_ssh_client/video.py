"""Stream or download a video through the Linux C client's L2CAP tunnel."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Optional, Sequence


def download_video(ssh: Sequence[str], remote: str, destination: Path) -> None:
    """Keep the destination untouched unless both SSH and the checksum succeed."""
    remote_path = shlex.quote(remote)
    temporary: Optional[Path] = None
    try:
        started = time.monotonic()
        with tempfile.TemporaryFile(dir=destination.parent) as received:
            print(f"Downloading {remote} ...", flush=True)
            subprocess.run(
                [*ssh, f"sha256sum < {remote_path} && cat -- {remote_path}"],
                stdout=received, check=True, timeout=1800,
            )
            received.seek(0)
            header = received.readline(128)
            if not re.fullmatch(rb"[0-9a-fA-F]{64}  -\n", header):
                raise ValueError("The server did not return a valid SHA-256 checksum.")
            checksum = header[:64].decode("ascii")
            digest = hashlib.sha256()
            with tempfile.NamedTemporaryFile(dir=destination.parent, suffix=".part", delete=False) as output:
                temporary = Path(output.name)
                for chunk in iter(lambda: received.read(65536), b""):
                    digest.update(chunk)
                    output.write(chunk)
        if digest.hexdigest() != checksum.lower():
            raise ValueError("SHA-256 mismatch: the video was not saved or played.")
        size = temporary.stat().st_size
        os.replace(temporary, destination)
        elapsed = time.monotonic() - started
        print(f"Verified {size:,} bytes in {elapsed:.1f} s: {destination}", flush=True)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def stream_video(ssh: Sequence[str], remote: str, *, camera: bool = False) -> None:
    """Encode a live camera or remux a file on the target, piping directly to the player."""
    input_options = ("-rw_timeout 10000000 -use_wallclock_as_timestamps 1 "
                     "-probesize 32768 -analyzeduration 100000" if camera else "-re")
    filters = ("setpts=PTS-STARTPTS,scale=320:180:force_original_aspect_ratio=decrease,"
               "pad=320:180:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=10")
    codec = (f"-vf {shlex.quote(filters)} -c:v mpeg4 -b:v 160k -maxrate 200k -bufsize 200k -g 10"
             if camera else "-c:v copy")
    command = (f"exec ffmpeg -nostdin -hide_banner -loglevel error {input_options} -i {shlex.quote(remote)} "
               f"-map 0:v:0 -an {codec} -f matroska -cluster_time_limit 250 -flush_packets 1 pipe:1")
    print(f"Streaming {remote}; close the player or press Ctrl+C to stop.", flush=True)
    sender = subprocess.Popen([*ssh, command], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
    assert sender.stdout is not None
    player = None
    pipe_statuses = {-signal.SIGPIPE, 128 + signal.SIGPIPE} if sys.platform != "win32" else set()
    try:
        player = subprocess.Popen(
            ["ffplay", "-autoexit", "-loglevel", "error", "-probesize", "32768",
             "-analyzeduration", "100000", "-window_title", "BLE L2CAP video stream", "-i", "pipe:0"],
            stdin=sender.stdout,
        )
        sender.stdout.close()
        while True:
            sender_result = sender.poll()
            if sender_result is not None and sender_result != 0:
                if sender_result not in pipe_statuses:
                    raise subprocess.CalledProcessError(sender_result, sender.args)
                # Closing the player's input can signal the sender before the player exits.
                result = player.wait(timeout=5)
                break
            try:
                result = player.wait(timeout=0.1)
                break
            except subprocess.TimeoutExpired:
                continue
        if result != 0:
            raise subprocess.CalledProcessError(result, player.args)
        result = sender.poll()
        if result is None:
            print("Player closed; stopping the target stream.", flush=True)
        elif result != 0 and result not in pipe_statuses:
            raise subprocess.CalledProcessError(result, sender.args)
    finally:
        sender.stdout.close()
        for process in (player, sender):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", help="BLE address or advertised name")
    parser.add_argument("remote", help="video file or camera URL as seen from the target")
    parser.add_argument("destination", nargs="?", type=Path)
    parser.add_argument("--user", default="")
    parser.add_argument("--client", default="ble_ssh_client", help="Linux C client executable")
    parser.add_argument("--ssh-option", action="append", default=[], help="extra ssh -o option")
    parser.add_argument("--no-play", action="store_true", help="download and verify only")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--stream", action="store_true", help="play as video arrives; no local file")
    mode.add_argument("--camera", action="store_true", help="stream a live camera URL at a BLE-friendly bitrate")
    args = parser.parse_args(argv)
    streaming = args.stream or args.camera
    if streaming and (args.destination is not None or args.no_play):
        parser.error("streaming cannot be combined with a destination or --no-play.")
    if not streaming and args.destination is None:
        parser.error("a destination is required for downloading (or use --stream / --camera).")
    if sys.platform != "linux":
        parser.error("L2CAP video requires the Linux C client.")
    client = shutil.which(args.client)
    if client is None:
        parser.error(f"C client not found: {args.client}")
    if not args.no_play and shutil.which("ffplay") is None:
        parser.error("ffplay is required for playback (install ffmpeg).")
    if not args.target or args.target.startswith("-") or args.user.startswith("-"):
        parser.error("invalid server or user")
    # SSH expands percent tokens even inside shell quotes.
    proxy = shlex.join([client, "--l2cap", "--verbose", args.target]).replace("%", "%%")
    ssh_target = args.target
    if not re.fullmatch(r"[A-Za-z0-9._:-]+", ssh_target):
        ssh_target = "ble-" + hashlib.sha256(args.target.encode("utf-8")).hexdigest()[:32]
    destination = f"{args.user}@{ssh_target}" if args.user else ssh_target
    ssh = ["ssh", "-T", "-o", f"ProxyCommand={proxy}", "-o", "ConnectTimeout=45",
           "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3"]
    for option in args.ssh_option:
        ssh.extend(["-o", option])
    ssh.append(destination)
    try:
        if streaming:
            stream_video(ssh, args.remote, camera=args.camera)
        else:
            download_video(ssh, args.remote, args.destination)
            if not args.no_play:
                subprocess.run(["ffplay", "-autoexit", "-loglevel", "error", str(args.destination.resolve())],
                               check=True)
    except KeyboardInterrupt:
        print("Video cancelled.", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Video transfer/playback failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
