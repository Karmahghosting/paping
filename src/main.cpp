// paping, originally created by Mike Lovell (2010).
// Copyright (c) 2026 Hopinette. MIT license; see LICENSE and COPYRIGHT.
#include "net.hpp"
#include "stats.hpp"

#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr const char* version = "2.0.0";
struct Options {
    std::string host;
    int port = 0, count = 0, timeout = 1000, interval = 1000, close_timeout = 1000;
    int family = AF_UNSPEC;
    bool json = false, help = false, show_version = false;
};

void banner() {
    std::cout << "paping " << version << " | TCP connection monitor\n"
              << "Original paping: Mike Lovell (c) 2010\n"
              << "TCP rewrite and maintenance: Hopinette (c) 2026\n\n";
}

void help() {
    std::cout << "Usage: paping [options] destination -p PORT\n\n"
              << "  -p, --port N           TCP port, 1..65535 (required)\n"
              << "  -c, --count N          Probes; 0 means continuous (default 0)\n"
              << "  -t, --timeout MS       Connect deadline (default 1000)\n"
              << "  -i, --interval MS      Minimum interval between starts (default 1000)\n"
              << "      --close-timeout MS Peer EOF wait after half-close (default 1000)\n"
              << "  -4, -6                 Resolve IPv4 or IPv6 only\n"
              << "      --json             Newline-delimited JSON output\n"
              << "      --nocolor          Accepted for compatibility; output is plain text\n"
              << "      --version          Show version and credits\n"
              << "  -h, -?, --help         Show this help\n\n"
              << "Durations: 1..3600000 ms. DNS is resolved once, outside the connect\n"
              << "deadline. The first resolved address is used for every probe.\n"
              << "Exit: 0 all connected; 1 probe failure; 2 usage; 3 DNS; 4 runtime;\n"
              << "      130 interrupted (Ctrl+C); 143 terminated (SIGTERM).\n";
}

int number(const std::string& value, int minimum, int maximum, const std::string& option) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()
        || result < minimum || result > maximum) {
        throw std::invalid_argument(option + " must be an integer in "
                                    + std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return result;
}

Options parse(int argc, char* argv[]) {
    Options options;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!positional && arg == "--") { positional = true; continue; }
        if (!positional && (arg == "-h" || arg == "-?" || arg == "--help")) options.help = true;
        else if (!positional && arg == "--version") options.show_version = true;
        else if (!positional && arg == "--json") options.json = true;
        else if (!positional && arg == "--nocolor") {}
        else if (!positional && (arg == "-4" || arg == "-6")) {
            const int family = arg == "-4" ? AF_INET : AF_INET6;
            if (options.family != AF_UNSPEC && options.family != family)
                throw std::invalid_argument("Choose only one of -4 and -6");
            options.family = family;
        } else if (!positional && (arg == "-p" || arg == "--port" || arg == "-c" || arg == "--count"
                    || arg == "-t" || arg == "--timeout" || arg == "-i" || arg == "--interval"
                    || arg == "--close-timeout")) {
            if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
            if (arg == "-p" || arg == "--port") options.port = number(argv[i], 1, 65535, arg);
            else if (arg == "-c" || arg == "--count") options.count = number(argv[i], 0, 2147483647, arg);
            else if (arg == "-t" || arg == "--timeout") options.timeout = number(argv[i], 1, 3600000, arg);
            else if (arg == "-i" || arg == "--interval") options.interval = number(argv[i], 1, 3600000, arg);
            else options.close_timeout = number(argv[i], 1, 3600000, arg);
        } else if (!positional && !arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("Unknown option: " + arg);
        } else {
            if (!options.host.empty()) throw std::invalid_argument("Specify exactly one destination");
            if (arg.empty()) throw std::invalid_argument("Destination cannot be empty");
            for (char byte : arg) {
                const auto c = static_cast<unsigned char>(byte);
                if (c <= 32 || c == 127) throw std::invalid_argument("Invalid character in destination");
            }
            options.host = arg;
        }
    }
    if (!options.help && !options.show_version && (options.host.empty() || options.port == 0))
        throw std::invalid_argument("Destination and TCP port are required");
    return options;
}

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (char byte : value) {
        const auto c = static_cast<unsigned char>(byte);
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        // Escape non-ASCII bytes too: native Windows error text may use a local code page.
        else if (c < 32 || c >= 127) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(c);
        else out << static_cast<char>(c);
    }
    return out.str() + '"';
}

void summary(const paping::Statistics& stats, bool json) {
    if (json) {
        std::cout << "{\"event\":\"summary\",\"attempted\":" << stats.attempted
                  << ",\"connected\":" << stats.connected << ",\"failed\":" << stats.failed
                  << ",\"interrupted\":" << stats.interrupted << ",\"close_incomplete\":" << stats.close_incomplete;
        if (stats.connected) std::cout << ",\"min_ms\":" << stats.minimum << ",\"max_ms\":" << stats.maximum
                                       << ",\"avg_ms\":" << stats.average();
        else std::cout << ",\"min_ms\":null,\"max_ms\":null,\"avg_ms\":null";
        std::cout << "}\n";
    } else {
        std::cout << "\nConnection statistics:\n  Attempted = " << stats.attempted
                  << ", Connected = " << stats.connected << ", Failed = " << stats.failed
                  << ", Interrupted = " << stats.interrupted
                  << "\n  Incomplete closes = " << stats.close_incomplete << '\n';
        if (stats.connected) std::cout << "Connect time: min=" << stats.minimum << "ms max=" << stats.maximum
                                       << "ms avg=" << stats.average() << "ms\n";
        else std::cout << "Connect time: no successful samples\n";
    }
}

void on_signal(int signal) { paping::stop_signal.store(signal, std::memory_order_relaxed); }
#ifdef _WIN32
BOOL WINAPI on_console_event(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    paping::stop_signal.store(SIGINT, std::memory_order_relaxed);
    return TRUE;
}
#endif
} // namespace

int main(int argc, char* argv[]) {
    std::cout << std::fixed << std::setprecision(3);
    Options options;
    try { options = parse(argc, argv); }
    catch (const std::invalid_argument& e) { std::cerr << "paping: " << e.what() << "\nUse --help for usage.\n"; return 2; }
    if (options.help || options.show_version) {
        banner();
        if (options.help) help();
        return 0;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#endif
    try {
        paping::NetworkRuntime runtime;
        paping::Endpoint endpoint;
        try { endpoint = paping::resolve(options.host, options.port, options.family); }
        catch (const std::runtime_error& e) { std::cerr << "paping: " << e.what() << '\n'; return 3; }
        if (options.json) {
            std::cout << "{\"event\":\"start\",\"version\":" << json_string(version)
                      << ",\"original_author\":\"Mike Lovell\",\"maintainer\":\"Hopinette\",\"host\":"
                      << json_string(options.host) << ",\"address\":" << json_string(endpoint.ip)
                      << ",\"port\":" << options.port << ",\"protocol\":\"TCP\"}\n";
        } else {
            banner();
            std::cout << "Connecting to " << options.host << " [" << endpoint.ip << "] on TCP "
                      << options.port << ":\n\n";
        }
        std::cout.flush();
        paping::Statistics stats;
        for (int index = 0; !paping::stop_signal && (options.count == 0 || index < options.count);) {
            const auto start = paping::Clock::now();
            const auto result = paping::probe(endpoint, options.timeout, options.close_timeout);
            if (options.count != 0) ++index; // Continuous runs cannot overflow this counter.
            if (result.status == "connected") stats.success(result.connect_ms, result.close_status == "complete");
            else if (result.status == "interrupted") stats.cancel();
            else stats.failure();
            if (options.json) {
                std::cout << "{\"event\":\"probe\",\"sequence\":" << stats.attempted
                          << ",\"status\":" << json_string(result.status)
                          << ",\"connect_ms\":" << result.connect_ms << ",\"error_code\":" << result.error_code
                          << ",\"detail\":" << json_string(result.detail)
                          << ",\"close_status\":" << json_string(result.close_status)
                          << ",\"close_ms\":" << result.close_ms << "}\n";
            } else {
                std::cout << (result.status == "connected" ? "Connected to " : "Failed to connect to ")
                          << endpoint.ip << ": time=" << result.connect_ms << "ms protocol=TCP port=" << options.port
                          << " status=" << result.status;
                if (!result.detail.empty()) std::cout << " (" << result.detail << ")";
                if (result.status == "connected") std::cout << " close=" << result.close_status;
                std::cout << '\n';
            }
            std::cout.flush();
            if (options.count != 0 && index >= options.count) break;
            paping::sleep_until(start + std::chrono::milliseconds(options.interval));
        }
        summary(stats, options.json);
        if (paping::stop_signal) return paping::stop_signal == SIGTERM ? 143 : 130;
        return stats.failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "paping: " << e.what() << '\n';
        return 4;
    }
}
