import hashlib
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from ble_ssh_client.video import download_video, main, stream_video


class VideoCommandTests(unittest.TestCase):
    def ssh_command(self, target, client="ble_ssh_client"):
        with patch("ble_ssh_client.video.sys.platform", "linux"), \
                patch("ble_ssh_client.video.shutil.which", side_effect=lambda command: command), \
                patch("ble_ssh_client.video.stream_video") as stream:
            self.assertEqual(main([target, "/tmp/video.mp4", "--stream", "--user", "pi", "--client", client]), 0)
            return stream.call_args.args[0]

    def test_device_names_are_preserved_without_becoming_invalid_ssh_hosts(self):
        destinations = set()
        client = "/tmp/ble%client"
        for target in ("ssh server", "ssh%server", "ssh's server"):
            with self.subTest(target=target):
                ssh = self.ssh_command(target, client)
                self.assertRegex(ssh[-1], r"^pi@[A-Za-z0-9._:-]+$")
                proxy = next(option for option in ssh if option.startswith("ProxyCommand="))
                self.assertEqual(
                    shlex.split(proxy.removeprefix("ProxyCommand=").replace("%%", "%")),
                    [client, "--l2cap", "--verbose", target],
                )
                self.assertNotIn(ssh[-1], destinations)
                destinations.add(ssh[-1])
                self.assertEqual(ssh, self.ssh_command(target, client))

    def test_existing_safe_hosts_keep_their_ssh_identity(self):
        for target in ("ssh-server", "AA:BB:CC:DD:EE:FF"):
            with self.subTest(target=target):
                self.assertEqual(self.ssh_command(target)[-1], f"pi@{target}")


class VideoDownloadTests(unittest.TestCase):
    def test_verified_download_quotes_remote_path(self):
        payload = b"video\x00\xff"
        checksum = hashlib.sha256(payload).hexdigest()

        def run(argv, **kwargs):
            self.assertEqual(argv[-1], "sha256sum < '/tmp/a b.mp4' && cat -- '/tmp/a b.mp4'")
            kwargs["stdout"].write(checksum.encode() + b"  -\n" + payload)
            return subprocess.CompletedProcess(argv, 0)

        with tempfile.TemporaryDirectory() as folder, patch("ble_ssh_client.video.subprocess.run", side_effect=run):
            destination = Path(folder) / "video.mp4"
            download_video(["ssh", "server"], "/tmp/a b.mp4", destination)
            self.assertEqual(destination.read_bytes(), payload)
            self.assertEqual(list(Path(folder).iterdir()), [destination])

    def test_failed_or_corrupt_transfer_preserves_destination(self):
        for fail in (True, False):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as folder:
                destination = Path(folder) / "video.mp4"
                destination.write_bytes(b"original")

                def run(argv, **kwargs):
                    kwargs["stdout"].write(b"0" * 64 + b"  -\npartial")
                    if fail:
                        raise subprocess.CalledProcessError(255, argv)
                    return subprocess.CompletedProcess(argv, 0)

                with patch("ble_ssh_client.video.subprocess.run", side_effect=run):
                    with self.assertRaises((subprocess.CalledProcessError, ValueError)):
                        download_video(["ssh", "server"], "/tmp/video.mp4", destination)
                self.assertEqual(destination.read_bytes(), b"original")
                self.assertEqual(list(Path(folder).iterdir()), [destination])

    def test_invalid_checksum_never_saves(self):
        with tempfile.TemporaryDirectory() as folder:
            def invalid_header(argv, **kwargs):
                kwargs["stdout"].write(b"not a checksum\nvideo")
                return subprocess.CompletedProcess(argv, 0)

            with patch("ble_ssh_client.video.subprocess.run", side_effect=invalid_header) as run:
                with self.assertRaises(ValueError):
                    download_video(["ssh", "server"], "/tmp/video.mp4", Path(folder) / "video.mp4")
                run.assert_called_once()
            self.assertEqual(list(Path(folder).iterdir()), [])


class VideoStreamTests(unittest.TestCase):
    def test_player_receives_data_before_sender_finishes(self):
        with tempfile.TemporaryDirectory() as folder:
            marker = str(Path(folder) / "playing")
            sender = (
                "import os, sys, time\n"
                "sys.stdout.buffer.write(b'frame'); sys.stdout.buffer.flush()\n"
                "deadline = time.monotonic() + 5\n"
                f"while not os.path.exists({marker!r}):\n"
                "    if time.monotonic() > deadline: sys.exit(9)\n"
                "    time.sleep(0.01)\n"
                "sys.stdout.buffer.write(b'end')\n"
            )
            player = (
                "import pathlib, sys\n"
                "assert sys.stdin.buffer.read(5) == b'frame'\n"
                f"pathlib.Path({marker!r}).touch()\n"
                "assert sys.stdin.buffer.read() == b'end'\n"
            )
            self.run_pipeline(sender, player)
            self.assertTrue(Path(marker).exists())

    def test_sender_failure_is_reported(self):
        for code in (7, 255):
            with self.subTest(code=code), self.assertRaises(subprocess.CalledProcessError) as error:
                self.run_pipeline(f"import sys; sys.exit({code})", "import sys; sys.stdin.buffer.read()")
            self.assertEqual(error.exception.returncode, code)

    def test_player_failure_stops_sender(self):
        with self.assertRaises(subprocess.CalledProcessError) as error:
            self.run_pipeline("import time; time.sleep(30)", "import sys; sys.exit(3)")
        self.assertEqual(error.exception.returncode, 3)

    def test_closing_camera_player_stops_continuous_sender(self):
        self.run_pipeline("import time; time.sleep(30)", "pass", camera=True)

    @unittest.skipUnless(hasattr(signal, "SIGPIPE"), "requires POSIX pipe signals")
    def test_sender_sigpipe_requires_clean_player_exit(self):
        sender = (
            "import os, signal\n"
            "signal.signal(signal.SIGPIPE, signal.SIG_DFL)\n"
            "while True: os.write(1, b'frame' * 1024)\n"
        )
        for code in (0, 3):
            with self.subTest(player_exit=code):
                player = f"import os, sys, time; os.read(0, 5); os.close(0); time.sleep(0.2); sys.exit({code})"
                if code:
                    with self.assertRaises(subprocess.CalledProcessError) as error:
                        self.run_pipeline(sender, player)
                    self.assertEqual(error.exception.returncode, code)
                else:
                    self.run_pipeline(sender, player)

    def run_pipeline(self, sender_code, player_code, *, camera=False):
        popen = subprocess.Popen
        processes = []

        def launch(argv, **kwargs):
            if not processes:
                if camera:
                    self.assertIn("-i http://127.0.0.1:8080/camera", argv[-1])
                    self.assertIn("scale=320:180:force_original_aspect_ratio=decrease", argv[-1])
                    self.assertIn("setsar=1,fps=10' -c:v mpeg4", argv[-1])
                    self.assertNotIn("-re ", argv[-1])
                else:
                    self.assertIn("-re -i '/tmp/a b.mp4'", argv[-1])
                    self.assertIn("-c:v copy -f matroska", argv[-1])
                code = sender_code
            else:
                self.assertEqual(argv[-2:], ["-i", "pipe:0"])
                code = player_code
            process = popen([sys.executable, "-c", code], **kwargs)
            processes.append(process)
            return process

        try:
            with patch("ble_ssh_client.video.subprocess.Popen", side_effect=launch):
                remote = "http://127.0.0.1:8080/camera" if camera else "/tmp/a b.mp4"
                stream_video(["ssh", "server"], remote, camera=camera)
        finally:
            for process in processes:
                self.assertIsNotNone(process.poll(), "streaming process was not cleaned up")


if __name__ == "__main__":
    unittest.main()
