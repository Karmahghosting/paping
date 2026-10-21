# TCP behavior and validation

## Scope

paping is a sockets client. TCP wire behavior belongs to the host operating system. The rewrite corrects the application-level connection lifecycle and diagnostics; it does not implement or certify the TCP state machine.

The original networking code called nonblocking `connect()` without checking its return value, interpreted a particular `select()` count as success, and omitted `SO_ERROR`. It also stored Windows socket handles in `int`, represented long timeouts as unnormalized microseconds, and initialized/cleaned up Winsock around individual operations. The rewrite replaces these paths with owned socket handles, process-lifetime Winsock initialization, explicit errors, and monotonic deadlines.

## Connection lifecycle

1. Resolve with `getaddrinfo` and retain the first IPv4/IPv6 address.
2. Create a fresh nonblocking `SOCK_STREAM` / `IPPROTO_TCP` socket per probe.
3. Check the immediate `connect` result; wait only when establishment is pending.
4. POSIX uses `poll`; Windows uses writable and exceptional `select` sets. Read `SO_ERROR` after readiness before accepting success.
5. Stop the connection timer. Half-close the sending direction with `shutdown`.
6. Receive and discard data until EOF, error, interruption, or the close deadline. Send no application request.
7. Release the socket on every path. Leave TIME-WAIT and retransmission state to the kernel.

## Relationship to the standard

[RFC 9293 §3.5](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.5) describes connection establishment. paping relies on a completed kernel active open rather than treating packet reception or socket readiness alone as success.

[RFC 9293 §3.6](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.6) and [§3.9.1.4](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.9.1.4) describe closing the sending direction while continuing to receive. Half-close followed by draining implements that application behavior. The kernel handles FIN acknowledgement and TIME-WAIT.

Resets can be legitimate TCP behavior. Deadline expiry, cancellation, unread data at descriptor release, or a peer reset can prevent orderly closure. These outcomes are exposed. `--timeout` and `--close-timeout` are application budgets; they do not change retransmission algorithms or configure the RFC 5482 TCP User Timeout Option.

## Socket API references

- [Microsoft: nonblocking connect](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-connect)
- [Microsoft: select and failed connects](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select)
- [Microsoft: socket closure](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-closesocket)
- [Linux: connect and SO_ERROR](https://man7.org/linux/man-pages/man2/connect.2.html)

## Reproducible evidence

`tests/integration.py` starts local TCP peers and executes the binary. It checks real results and peer-side EOF, without mocking sockets. `tests/stats.cpp` checks mixed outcomes so failures cannot dilute latency averages or incorrectly initialize minima.

`tests/wire.py` creates a fresh Linux network namespace. Packet capture checks both handshake directions, both FIN directions, absence of RST on a cooperative banner connection, and absence of client payload. Namespace-local nftables rules drop SYN packets to test the actual pending-connect timeout and SIGINT paths. Only test-port traffic in the namespace is inspected.

These tests establish behavior where they run. They do not establish conformance of every OS, network, TCP option, loss-recovery algorithm, or middlebox. DNS is outside probe timing, and a blocked system resolver can delay cancellation.
