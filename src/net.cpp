// Copyright (c) 2026 Hopinette. MIT license; see LICENSE.
#include "net.hpp"

#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace paping {
static_assert(std::atomic<int>::is_always_lock_free, "Signal handling requires a lock-free flag");
std::atomic<int> stop_signal{0};
namespace {
#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
int last_error() { return WSAGetLastError(); }
void close_socket(SocketHandle s) { closesocket(s); }
bool interrupted(int e) { return e == WSAEINTR; }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
bool pending(int e) { return would_block(e) || e == WSAEINPROGRESS || e == WSAEALREADY; }
bool reset(int e) { return e == WSAECONNRESET || e == WSAECONNABORTED; }
constexpr int shutdown_write = SD_SEND;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle invalid_socket = -1;
int last_error() { return errno; }
void close_socket(SocketHandle s) { ::close(s); }
bool interrupted(int e) { return e == EINTR; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool pending(int e) { return e == EINPROGRESS || e == EALREADY || interrupted(e); }
bool reset(int e) { return e == ECONNRESET || e == ECONNABORTED; }
constexpr int shutdown_write = SHUT_WR;
#endif

class Socket {
public:
    explicit Socket(int family) : handle_(::socket(family, SOCK_STREAM, IPPROTO_TCP)) {}
    ~Socket() { if (handle_ != invalid_socket) close_socket(handle_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SocketHandle get() const { return handle_; }
    bool valid() const { return handle_ != invalid_socket; }
    bool nonblocking() const {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(handle_, FIONBIO, &enabled) == 0;
#else
        const int flags = fcntl(handle_, F_GETFL, 0);
        return flags != -1 && fcntl(handle_, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
    }
private:
    SocketHandle handle_;
};

enum class Wait { ready, timeout, stopped, error };

Wait wait_socket(SocketHandle socket, bool connecting, Clock::time_point deadline, int& error) {
    while (!stop_signal) {
        const auto now = Clock::now();
        if (now >= deadline) return Wait::timeout;
        // Round up sub-ms waits and check cancellation at least every 50 ms.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int slice = static_cast<int>(std::min<long long>(50, remaining.count() + 1));
#ifdef _WIN32
        fd_set readable, writable, exceptional;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_ZERO(&exceptional);
        if (connecting) {
            FD_SET(socket, &writable);
            FD_SET(socket, &exceptional); // Winsock reports failed connects here.
        } else {
            FD_SET(socket, &readable);
        }
        timeval timeout{0, slice * 1000L};
        const int result = select(0, connecting ? nullptr : &readable,
                                  connecting ? &writable : nullptr,
                                  connecting ? &exceptional : nullptr, &timeout);
#else
        pollfd descriptor{socket, static_cast<short>(connecting ? POLLOUT : POLLIN), 0};
        const int result = poll(&descriptor, 1, slice);
#endif
        if (stop_signal) return Wait::stopped;
        if (result > 0) return Wait::ready;
        if (result < 0) {
            error = last_error();
            if (!interrupted(error)) return Wait::error;
        }
    }
    return Wait::stopped;
}

std::string classify(int error) {
#ifdef _WIN32
    if (error == WSAECONNREFUSED) return "refused";
    if (error == WSAETIMEDOUT) return "timeout";
    if (error == WSAENETUNREACH || error == WSAEHOSTUNREACH || error == WSAENETDOWN) return "unreachable";
#else
    if (error == ECONNREFUSED) return "refused";
    if (error == ETIMEDOUT) return "timeout";
    if (error == ENETUNREACH || error == EHOSTUNREACH || error == ENETDOWN) return "unreachable";
#endif
    return "error";
}

void set_error(Probe& result, int error) {
    result.status = classify(error);
    result.error_code = error;
    result.detail = std::system_category().message(error);
}

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string orderly_close(SocketHandle socket, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    // Half-close the sending direction, then receive through peer EOF. Do not
    // force SO_LINGER={1,0}: that requests an abortive RST, not an orderly close.
    while (::shutdown(socket, shutdown_write) != 0) {
        const int error = last_error();
        if (stop_signal) return "interrupted";
        if (!interrupted(error)) return reset(error) ? "reset" : "error";
        if (Clock::now() >= deadline) return "timeout";
    }
    char buffer[8192];
    while (!stop_signal) {
        // A peer that streams forever must not extend the cleanup budget.
        if (Clock::now() >= deadline) return "timeout";
        const auto received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received == 0) return "complete";
        if (received > 0) continue;
        int error = last_error();
        if (interrupted(error)) continue;
        if (!would_block(error)) return reset(error) ? "reset" : "error";
        switch (wait_socket(socket, false, deadline, error)) {
        case Wait::ready: break;
        case Wait::timeout: return "timeout";
        case Wait::stopped: return "interrupted";
        case Wait::error: return "error";
        }
    }
    return "interrupted";
}
} // namespace

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
#endif
}
NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

Endpoint resolve(const std::string& host, int port, int family) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const int error = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (error != 0) throw std::runtime_error("Cannot resolve " + host + ": " + gai_strerror(error));
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(addresses, freeaddrinfo);
    for (auto* address = addresses; address; address = address->ai_next) {
        if (address->ai_family != AF_INET && address->ai_family != AF_INET6) continue;
        Endpoint result;
        if (address->ai_addrlen > sizeof(result.address)) continue;
        result.length = static_cast<int>(address->ai_addrlen);
        result.family = address->ai_family;
        std::copy_n(reinterpret_cast<const char*>(address->ai_addr), result.length,
                    reinterpret_cast<char*>(&result.address));
        char ip[NI_MAXHOST]{};
        const int name_error = getnameinfo(address->ai_addr, static_cast<SocketLength>(address->ai_addrlen),
                                           ip, sizeof(ip), nullptr, 0, NI_NUMERICHOST);
        if (name_error != 0) throw std::runtime_error("Cannot format resolved address");
        result.ip = ip;
        return result;
    }
    throw std::runtime_error("No IPv4 or IPv6 address for " + host);
}

Probe probe(const Endpoint& endpoint, int timeout_ms, int close_timeout_ms) {
    Probe result;
    Socket socket(endpoint.family);
    if (!socket.valid() || !socket.nonblocking()) {
        set_error(result, last_error());
        return result;
    }
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    const int connected = ::connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint.address),
                                    static_cast<SocketLength>(endpoint.length));
    bool established = connected == 0;
    if (!established) {
        int error = last_error();
        if (!pending(error)) {
            set_error(result, error);
        } else {
            const auto waited = wait_socket(socket.get(), true, deadline, error);
            if (waited == Wait::ready) {
                int socket_error = 0;
                SocketLength length = sizeof(socket_error);
                // Readiness alone is not evidence of a successful connection.
                if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socket_error), &length) != 0) {
                    set_error(result, last_error());
                } else if (socket_error != 0) {
                    set_error(result, socket_error);
                } else {
                    established = true;
                }
            } else if (waited == Wait::timeout) {
                result.status = "timeout";
                result.detail = "Application connect deadline expired";
            } else if (waited == Wait::stopped) {
                result.status = "interrupted";
            } else {
                set_error(result, error);
            }
        }
    }
    result.connect_ms = elapsed(start);
    if (established) {
        result.status = "connected";
        const auto close_start = Clock::now();
        result.close_status = orderly_close(socket.get(), close_timeout_ms);
        result.close_ms = elapsed(close_start);
    }
    return result; // Socket ownership also covers failures and cancellation.
}

void sleep_until(Clock::time_point deadline) {
    while (!stop_signal && Clock::now() < deadline) {
        std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(50)));
    }
}
} // namespace paping
