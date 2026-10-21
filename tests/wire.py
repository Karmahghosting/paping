"""Linux-only packet and dropped-SYN tests in a disposable network namespace.

Run: sudo python3 tests/wire.py /absolute/path/to/paping
Requires iproute2, nftables, and CAP_SYS_ADMIN/CAP_NET_ADMIN/CAP_NET_RAW.
Only the new namespace is configured; the host network is never modified.
"""
import json
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest


if "--isolated" not in sys.argv:
    if len(sys.argv) != 2 or not sys.platform.startswith("linux"):
        raise SystemExit("Usage on Linux: sudo python3 tests/wire.py /path/to/paping")
    raise SystemExit(subprocess.call(["unshare", "--net", sys.executable, str(Path(__file__).resolve()),
                                     "--isolated", str(Path(sys.argv[1]).resolve())]))

sys.argv.remove("--isolated")
BINARY = str(Path(sys.argv.pop(1)).resolve())
subprocess.run(["ip", "link", "set", "lo", "up"], check=True)


class WireTests(unittest.TestCase):
    def test_handshake_and_orderly_close_with_banner(self):
        with socket.socket() as server, socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)) as capture:
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            server.settimeout(5)
            port = server.getsockname()[1]
            capture.bind(("lo", 0))
            capture.setblocking(False)
            errors = []

            def respond():
                try:
                    connection, _ = server.accept()
                    with connection:
                        connection.settimeout(3)
                        connection.sendall(b"220 test banner\r\n" * 4096)
                        self.assertEqual(connection.recv(1), b"")
                        connection.shutdown(socket.SHUT_WR)
                except Exception as error:
                    errors.append(error)

            peer = threading.Thread(target=respond, daemon=True)
            peer.start()
            process = subprocess.Popen([BINARY, "127.0.0.1", "-p", str(port), "-c", "1", "--json"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            packets = []
            deadline = time.monotonic() + 5
            finished_at = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    finished_at = finished_at or time.monotonic()
                    if time.monotonic() - finished_at > 0.1:
                        break
                readable, _, _ = select.select([capture], [], [], 0.02)
                if not readable:
                    continue
                packet = capture.recv(65535)
                if len(packet) < 54 or packet[12:14] != b"\x08\x00" or packet[23] != 6:
                    continue
                ip_header = (packet[14] & 15) * 4
                tcp = 14 + ip_header
                source, destination = struct.unpack_from("!HH", packet, tcp)
                if port not in (source, destination):
                    continue
                flags = packet[tcp + 13]
                total = struct.unpack_from("!H", packet, 16)[0]
                payload = total - ip_header - ((packet[tcp + 12] >> 4) * 4)
                packets.append((source, destination, flags, payload))
            stdout, stderr = process.communicate(timeout=2)
            peer.join(3)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(errors, [])
            self.assertFalse(peer.is_alive())
            self.assertEqual(json.loads(stdout.splitlines()[1])["close_status"], "complete")
            self.assertTrue(any(dst == port and flags & 0x12 == 0x02 for _, dst, flags, _ in packets), "Missing SYN")
            self.assertTrue(any(src == port and flags & 0x12 == 0x12 for src, _, flags, _ in packets), "Missing SYN/ACK")
            self.assertTrue(any(dst == port and flags & 0x12 == 0x10 for _, dst, flags, _ in packets), "Missing ACK")
            self.assertTrue(any(dst == port and flags & 1 for _, dst, flags, _ in packets), "Missing client FIN")
            self.assertTrue(any(src == port and flags & 1 for src, _, flags, _ in packets), "Missing server FIN")
            self.assertFalse(any(flags & 4 for _, _, flags, _ in packets), "Unexpected RST")
            self.assertFalse(any(dst == port and payload > 0 for _, dst, _, payload in packets), "Client sent payload")

    def dropped_syn(self, interrupt=False):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            port = listener.getsockname()[1]
            rules = ("table inet paping_test {\n chain output {\n type filter hook output priority 0; policy accept;\n"
                     f" tcp dport {port} tcp flags & (syn | ack) == syn drop\n" + " }\n}\n")
            subprocess.run(["nft", "-f", "-"], input=rules, text=True, check=True)
            try:
                started = time.monotonic()
                process = subprocess.Popen([BINARY, "127.0.0.1", "-p", str(port), "-c", "1", "-t",
                                            "5000" if interrupt else "1200", "--json"],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                if interrupt:
                    time.sleep(0.15)
                    process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=4)
                data = [json.loads(line) for line in stdout.splitlines()]
                if interrupt:
                    self.assertEqual(process.returncode, 130, stderr)
                    self.assertEqual(data[1]["status"], "interrupted")
                    self.assertEqual(data[-1]["interrupted"], 1)
                    self.assertEqual(data[-1]["failed"], 0)
                    self.assertLess(time.monotonic() - started, 1)
                else:
                    self.assertEqual(process.returncode, 1, stderr)
                    self.assertEqual(data[1]["status"], "timeout")
                    self.assertEqual(data[1]["close_status"], "not_started")
                    self.assertGreaterEqual(data[1]["connect_ms"], 1150)
                    self.assertLess(data[1]["connect_ms"], 2000)
                    self.assertEqual(data[-1]["failed"], 1)
            finally:
                if "process" in locals() and process.poll() is None:
                    process.kill()
                    process.communicate()
                subprocess.run(["nft", "delete", "table", "inet", "paping_test"], check=True)

    def test_dropped_syn_reaches_application_deadline(self):
        self.dropped_syn()

    def test_interrupt_pending_connect(self):
        self.dropped_syn(interrupt=True)


unittest.main(verbosity=2)
