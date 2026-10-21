// Copyright (c) 2026 Hopinette. MIT license; see LICENSE.
#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace paping {
using Clock = std::chrono::steady_clock;
extern std::atomic<int> stop_signal;

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

struct Endpoint {
    sockaddr_storage address{};
    int length = 0;
    int family = 0;
    std::string ip;
};

// Resolve once and pin the first IPv4/IPv6 endpoint for comparable samples.
Endpoint resolve(const std::string& host, int port, int family);

struct Probe {
    std::string status = "error";
    std::string detail;
    int error_code = 0;
    double connect_ms = 0;
    std::string close_status = "not_started";
    double close_ms = 0;
};

Probe probe(const Endpoint& endpoint, int timeout_ms, int close_timeout_ms);
void sleep_until(Clock::time_point deadline);
} // namespace paping
