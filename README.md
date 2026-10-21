# paping

**A small TCP connection monitor with precise results and orderly connection teardown.**

[![CI](https://github.com/Karmahghosting/paping/actions/workflows/ci.yml/badge.svg)](https://github.com/Karmahghosting/paping/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](CMakeLists.txt)

paping repeatedly opens a TCP connection to a single host and port, measures how long connection establishment takes, and reports availability over time. Use it to check a service listener, investigate intermittent connectivity, or collect connection latency from scripts.

Version 2 rebuilds the networking core around explicit socket errors, monotonic deadlines, and a bounded graceful close. It preserves the familiar `paping host -p port` interface and the original project's Git history.

## Quick start

```sh
paping example.com -p 443 -c 5
```

```text
paping 2.0.0 | TCP connection monitor
Original paping: Mike Lovell (c) 2010
TCP rewrite and maintenance: Hopinette (c) 2026
```

Each sample reports a connection result and, when connected, a separate close result. No application payload is sent. A successful connection demonstrates that TCP connection establishment completed; it does not validate HTTP, TLS, authentication, or application health.

## What changed

- **Verified results.** Nonblocking connection completion is checked with `SO_ERROR`; socket readiness alone is never treated as success.
- **Orderly teardown.** Successful probes half-close the sending side and drain incoming data until peer EOF or the close deadline.
- **Clear failures.** Refusals, deadlines, unreachable destinations, and other socket errors are reported separately.
- **Reliable timing.** A monotonic clock measures connection time independently of DNS, cleanup, and the probe interval.
- **IPv4 and IPv6.** Use the system resolver or select an address family explicitly.
- **Scriptable output.** JSON Lines, stable exit codes, and latency statistics based on successful connections only.
- **Portable build.** C++17 and system sockets, with no third-party runtime dependencies.

## TCP behavior

paping uses the operating system's TCP implementation. The kernel performs the handshake, retransmission, sequence handling, congestion control, and TIME-WAIT processing. The application uses ordinary TCP sockets and follows the connection and close semantics described by [RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html).

After a successful connect, paping calls `shutdown(SHUT_WR)` on POSIX or `shutdown(SD_SEND)` on Windows, then receives until EOF. It does not request an abortive zero-linger close or change system TCP settings.

If the peer resets, never closes, or keeps streaming beyond `--close-timeout`, the sample retains its connection result and reports incomplete cleanup separately. Releasing that socket may result in a reset when unread data remains. An application deadline also cancels a pending connect. These are bounded diagnostic operations, not replacements for TCP's own retransmission timers.

This is a TCP diagnostic client, **not a TCP stack or a certification of every RFC requirement**. See [TCP behavior and validation](docs/TCP.md) for the scope and reproducible packet tests.

## Usage

```text
paping [options] destination -p PORT
```

| Option | Description | Default |
| --- | --- | --- |
| `-p`, `--port N` | TCP destination port, `1..65535` | Required |
| `-c`, `--count N` | Number of probes; `0` runs until interrupted | `0` |
| `-t`, `--timeout MS` | Connection establishment deadline | `1000` |
| `-i`, `--interval MS` | Minimum interval between probe starts | `1000` |
| `--close-timeout MS` | Maximum wait for peer EOF after half-close | `1000` |
| `-4` / `-6` | Resolve IPv4 / IPv6 only | System order |
| `--json` | Emit one JSON object per line | Off |
| `--nocolor` | Compatibility option; output is already plain text | — |
| `--version` | Print version and credits | — |
| `-h`, `-?`, `--help` | Print usage | — |

Durations accept integer milliseconds from `1` to `3600000`. Probes run sequentially; a slow connect or close can extend the interval. No interval is added after the final probe.

DNS is resolved once before probing, using the first returned address. That address stays fixed for comparable samples and appears in the output. There is no automatic failover or DNS refresh; use `-4`, `-6`, or a literal IP when needed. The synchronous system resolver is outside the connect deadline and may delay cancellation.

```sh
# Ten IPv4 checks, with a 2.5-second connection deadline
paping example.com -4 -p 443 -c 10 -t 2500

# Probe an IPv6 listener
paping ::1 -6 -p 8080 -c 3

# Collect JSON Lines for a local service
paping 127.0.0.1 -p 8080 -c 20 -i 500 --json > samples.jsonl
```

On PowerShell, run the executable as `.\paping.exe`.

## JSON and exit codes

`--json` emits a `start` event, one `probe` per attempt, and a final `summary`. Setup errors go to stderr. `--version` always prints human-readable credits.

Probe statuses: `connected`, `refused`, `timeout`, `unreachable`, `error`, `interrupted`.

Close statuses: `complete`, `timeout`, `reset`, `error`, `interrupted`, `not_started`. `complete` means peer EOF was observed after local half-close; kernel TCP state may remain afterward.

`connect_ms` includes the wait for success or failure; `close_ms` is separate. Summary latency fields include successful connects only and are `null` when none succeeded. `close_incomplete` counts connected probes whose cleanup did not reach EOF. Interrupted pending connects are separate from failures.

| Code | Meaning |
| --- | --- |
| `0` | All completed probes connected; inspect close results separately |
| `1` | At least one connection attempt failed |
| `2` | Invalid command line |
| `3` | Name resolution failed |
| `4` | Runtime initialization or unexpected runtime error |
| `130` | Ctrl+C / SIGINT; also Ctrl+Break on Windows |
| `143` | SIGTERM |

## Build

Requirements: a C++17 compiler and CMake 3.16 or newer. Python 3 is needed only when tests are enabled.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --config Release
```

The executable is `build/paping` with single-configuration generators, or `build/Release/paping.exe` with Visual Studio. Windows builds require Visual Studio C++ tools and a Windows SDK, or MinGW-w64. Linux and macOS use native socket APIs. MSVC builds use the static C++ runtime.

To install on Linux or macOS:

```sh
cmake --install build --prefix "$HOME/.local"
```

## Test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The integration suite uses real loopback sockets: IPv4/IPv6, refusals, server banners, delayed and streaming closes, resets, repeated connections, argument validation, statistics, and cancellation. Tests do not probe external hosts.

Linux also has an isolated packet suite:

```sh
sudo python3 tests/wire.py "$(pwd)/build/paping"
```

It creates a disposable network namespace, checks the SYN → SYN/ACK → ACK exchange and FIN exchange without RST, then drops SYN packets to test deadlines and interruption. It requires `iproute2` and `nftables`. The host network configuration is not changed.

## Credits and license

- **Mike Lovell** created the original paping. His original copyright notice is preserved verbatim in [COPYRIGHT](COPYRIGHT).
- **Hopinette** maintains this edition and its TCP rewrite: [Karmahghosting](https://github.com/Karmahghosting).
- The baseline is the [9minds/paping](https://github.com/9minds/paping) mirror of the original Google Code project, at commit `705edc2d38fffa3404ef26399504de1128febf9e`.

Distributed under the [MIT License](LICENSE). This edition is independently maintained and does not imply endorsement by the original author.
