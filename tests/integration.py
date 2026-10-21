"""Black-box tests against real, loopback-only TCP peers. No dependencies."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class Peer:
    def __init__(self, mode="normal", family=socket.AF_INET, count=1, delay=0):
        self.mode, self.count, self.delay = mode, count, delay
        self.listener = socket.socket(family, socket.SOCK_STREAM)
        self.listener.bind(("::1" if family == socket.AF_INET6 else "127.0.0.1", 0))
        self.listener.listen(64)
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.host = self.listener.getsockname()[0]
        self.received = bytearray()
        self.eofs = 0
        self.errors = []
        self.accepted = threading.Event()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def serve(self):
        try:
            for _ in range(self.count):
                connection, _ = self.listener.accept()
                self.accepted.set()
                with connection:
                    connection.settimeout(3)
                    if self.mode == "reset":
                        layout = "HH" if os.name == "nt" else "ii"
                        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack(layout, 1, 0))
                        continue
                    if self.mode == "banner":
                        connection.sendall(b"220 loopback test service\r\n" * 4096)
                    while True:
                        data = connection.recv(8192)
                        if not data:
                            self.eofs += 1
                            break
                        self.received.extend(data)
                    if self.mode == "stream":
                        while not self.done.is_set():
                            try:
                                connection.sendall(b"x" * 8192)
                            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                                break
                    if self.delay:
                        self.done.wait(self.delay)
                    if self.mode != "stream":
                        connection.shutdown(socket.SHUT_WR)
        except Exception as error:
            if not self.done.is_set():
                self.errors.append(error)
        finally:
            self.listener.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.listener.close()
        self.thread.join(6)
        if self.thread.is_alive():
            raise AssertionError("Peer thread did not stop")


def run(*args, timeout=8, **kwargs):
    return subprocess.run([BINARY, *map(str, args)], capture_output=True, text=True, timeout=timeout, **kwargs)


def events(result):
    return [json.loads(line) for line in result.stdout.splitlines()]


class Integration(unittest.TestCase):
    def probe(self, peer, *extra):
        return run(peer.host, "-p", peer.port, "-c", 1, "-i", 1, "--json", *extra)

    def test_version_credits(self):
        result = run("--version")
        self.assertEqual(result.returncode, 0)
        self.assertIn("Mike Lovell", result.stdout)
        self.assertIn("Hopinette", result.stdout)
        self.assertIn("2.0.0", result.stdout)

    def test_help(self):
        for option in ("--help", "-h", "-?"):
            with self.subTest(option=option):
                result = run(option)
                self.assertEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stdout)

    def test_invalid_arguments(self):
        invalid = [[], ["127.0.0.1"], ["-p", "80"], ["--unknown"], ["a", "b", "-p", "80"],
                   ["-4", "-6", "localhost", "-p", "80"], ["localhost", "-p"],
                   ["", "-p", "80"], ["x\nforged", "-p", "80"]]
        for option, values in (("-p", ["0", "65536", "-1", "80junk", "1.5", "999999999999"]),
                               ("-t", ["0", "-1", "3600001", "1000x"]),
                               ("-c", ["-1", "2147483648", "x"]),
                               ("-i", ["0", "-5"]), ("--close-timeout", ["0", "x"])):
            invalid.extend([["127.0.0.1", "-p", "80", option, value] for value in values])
        for args in invalid:
            with self.subTest(args=args):
                result = run(*args)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertTrue(result.stderr)

    def test_ipv4_orderly_close_no_payload(self):
        with Peer() as peer:
            result = self.probe(peer, "-4")
            self.assertEqual(result.returncode, 0, result.stderr)
            data = events(result)
            self.assertEqual(data[0]["maintainer"], "Hopinette")
            self.assertEqual(data[1]["status"], "connected")
            self.assertEqual(data[1]["close_status"], "complete")
            self.assertEqual(data[-1]["connected"], 1)
        self.assertEqual(peer.eofs, 1)
        self.assertEqual(peer.received, b"")
        self.assertEqual(peer.errors, [])

    def test_ipv6(self):
        try:
            peer = Peer(family=socket.AF_INET6)
        except OSError as error:
            self.skipTest(str(error))
        with peer:
            result = self.probe(peer, "-6")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(events(result)[0]["address"], "::1")
            self.assertEqual(events(result)[1]["close_status"], "complete")

    def test_dns_localhost(self):
        with Peer() as peer:
            result = run("localhost", "-4", "-p", peer.port, "-c", 1, "--json")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(events(result)[0]["address"], "127.0.0.1")

    def test_dns_error(self):
        # A malformed numeric address with IPv4 forced fails without external DNS.
        result = run("::1", "-4", "-p", 80, "-c", 1)
        self.assertEqual(result.returncode, 3)
        self.assertIn("Cannot resolve", result.stderr)

    def test_refused_is_not_timeout_or_success(self):
        # Linux/Windows reject a bound, non-listening TCP port. Darwin can
        # silently drop SYNs in that state, so release its reservation first.
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
            if sys.platform == "darwin":
                reserved.close()
            result = run("127.0.0.1", "-p", port, "-c", 1, "-t", 2500, "--json")
        self.assertEqual(result.returncode, 1)
        data = events(result)
        self.assertEqual(data[1]["status"], "refused", result.stdout)
        self.assertNotEqual(data[1]["error_code"], 0)
        self.assertIsNone(data[-1]["avg_ms"])
        self.assertEqual(data[-1]["failed"], 1)

    def test_large_server_banner_is_drained(self):
        with Peer(mode="banner") as peer:
            result = self.probe(peer)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(events(result)[1]["close_status"], "complete")
        self.assertEqual(peer.errors, [])
        self.assertEqual(peer.eofs, 1)
        self.assertEqual(peer.received, b"")

    def test_connect_latency_excludes_close(self):
        with Peer(delay=0.25) as peer:
            result = self.probe(peer)
            sample = events(result)[1]
            self.assertEqual(result.returncode, 0)
            self.assertEqual(sample["close_status"], "complete")
            self.assertGreater(sample["close_ms"], 180)
            self.assertLess(sample["connect_ms"], sample["close_ms"])

    def test_close_deadline(self):
        with Peer(delay=1) as peer:
            started = time.monotonic()
            result = self.probe(peer, "--close-timeout", 120)
            duration = time.monotonic() - started
            sample = events(result)[1]
            self.assertEqual(result.returncode, 0)
            self.assertEqual(sample["status"], "connected")
            self.assertEqual(sample["close_status"], "timeout")
            self.assertEqual(events(result)[-1]["close_incomplete"], 1)
            self.assertGreaterEqual(sample["close_ms"], 100)
            self.assertLess(duration, 0.9)

    def test_streaming_peer_cannot_extend_deadline(self):
        with Peer(mode="stream") as peer:
            started = time.monotonic()
            result = self.probe(peer, "--close-timeout", 100)
            self.assertEqual(events(result)[1]["close_status"], "timeout")
            self.assertLess(time.monotonic() - started, 1)

    def test_peer_reset(self):
        with Peer(mode="reset") as peer:
            result = self.probe(peer)
            sample = events(result)[1]
            # A reset can race with connect completion; neither outcome is a timeout.
            self.assertIn(sample["status"], ("connected", "error"))
            if sample["status"] == "connected":
                self.assertIn(sample["close_status"], ("reset", "error"))
                self.assertEqual(events(result)[-1]["close_incomplete"], 1)

    def test_multiple_samples_and_statistics(self):
        with Peer(count=5) as peer:
            result = self.probe(peer, "-c", 5, "-i", 10)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = events(result)
            samples, totals = data[1:-1], data[-1]
            self.assertEqual(len(samples), 5)
            self.assertEqual([s["sequence"] for s in samples], list(range(1, 6)))
            self.assertEqual(totals["connected"], 5)
            self.assertEqual(totals["failed"], 0)
            self.assertAlmostEqual(totals["avg_ms"], sum(s["connect_ms"] for s in samples) / 5, delta=0.002)
        self.assertEqual(peer.eofs, 5)

    def test_mixed_success_failure_exit(self):
        with Peer() as peer:
            result = self.probe(peer, "-c", 3, "-i", 80)
            self.assertEqual(result.returncode, 1)
            totals = events(result)[-1]
            self.assertEqual(totals["connected"], 1)
            self.assertEqual(totals["failed"], 2)
            self.assertEqual(totals["avg_ms"], events(result)[1]["connect_ms"])

    def test_no_sleep_after_last_sample(self):
        with Peer() as peer:
            start = time.monotonic()
            result = self.probe(peer, "-i", 3600000)
            self.assertEqual(result.returncode, 0)
            self.assertLess(time.monotonic() - start, 1)

    def test_timeout_over_one_second_is_valid(self):
        with Peer() as peer:
            result = self.probe(peer, "-t", 2500)
            self.assertEqual(result.returncode, 0)

    def test_plain_compatible_output(self):
        with Peer() as peer:
            result = run(peer.host, "--port", peer.port, "--count", 1, "--nocolor")
            self.assertEqual(result.returncode, 0)
            self.assertIn("Connected to", result.stdout)
            self.assertIn("Hopinette", result.stdout)
            self.assertNotIn("\x1b", result.stdout)

    @unittest.skipIf(os.name == "nt", "POSIX signals")
    def test_interrupt_sleep_and_close(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            for delay in (0, 5):
                with self.subTest(signal=signum, close_delay=delay), Peer(delay=delay) as peer:
                    process = subprocess.Popen([BINARY, peer.host, "-p", str(peer.port), "-i", "5000",
                                                "--close-timeout", "5000", "--json"],
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        self.assertTrue(peer.accepted.wait(3))
                        time.sleep(0.1)
                        started = time.monotonic()
                        process.send_signal(signum)
                        stdout, stderr = process.communicate(timeout=2)
                        self.assertLess(time.monotonic() - started, 1)
                        self.assertEqual(process.returncode, 128 + signum, stderr)
                        self.assertEqual(json.loads(stdout.splitlines()[-1])["event"], "summary")
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.communicate()

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux descriptor semantics")
    def test_socket_above_fd_setsize(self):
        descriptors = []
        try:
            for _ in range(1050):
                descriptors.append(os.open("/dev/null", os.O_RDONLY))
            with Peer() as peer:
                result = run(peer.host, "-p", peer.port, "-c", 1, "--json", pass_fds=tuple(descriptors))
                self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            for descriptor in descriptors:
                os.close(descriptor)

    @unittest.skipUnless(os.name == "nt", "Windows console events")
    def test_windows_ctrl_break(self):
        # Give the test child its own hidden console. A separate sender attaches
        # only to that console; it cannot signal the caller's console or apps.
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with Peer(delay=5) as peer:
            process = subprocess.Popen([BINARY, peer.host, "-p", str(peer.port), "--close-timeout", "5000", "--json"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                       creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
            try:
                self.assertTrue(peer.accepted.wait(3))
                sender = r'''
import ctypes, sys, time
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.FreeConsole()
if not kernel.AttachConsole(int(sys.argv[1])):
    raise ctypes.WinError(ctypes.get_last_error())
callback = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)(lambda event: 1)
kernel.SetConsoleCtrlHandler(callback, True)
if not kernel.GenerateConsoleCtrlEvent(1, 0):
    raise ctypes.WinError(ctypes.get_last_error())
time.sleep(0.1)
kernel.FreeConsole()
'''
                event = subprocess.run([sys.executable, "-c", sender, str(process.pid)],
                                       capture_output=True, text=True, timeout=3)
                self.assertEqual(event.returncode, 0, event.stderr)
                stdout, stderr = process.communicate(timeout=2)
                self.assertEqual(process.returncode, 130, stderr)
                self.assertEqual(json.loads(stdout.splitlines()[-1])["event"], "summary")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_repeated_connections_release_sockets(self):
        # A leak is caught quickly under a deliberately small POSIX descriptor limit.
        kwargs = {}
        if os.name != "nt":
            import resource

            def limit():
                _, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
                resource.setrlimit(resource.RLIMIT_NOFILE, (64, hard))
            kwargs["preexec_fn"] = limit
        with Peer(count=150) as peer:
            result = run(peer.host, "-p", peer.port, "-c", 150, "-i", 1, "--json", **kwargs)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(events(result)[-1]["connected"], 150)


if __name__ == "__main__":
    unittest.main(verbosity=2)
