// The program's configuration (note 009, D7): one file of `key = value`
// lines, `#` to the end of a line a comment. Every key is known and checked;
// an unknown key, a bad value or a key given twice is refused with its line.
// The digest (of the keys and values as read, in key order) goes into each
// recording's header, so a recording can say how it was made.
// Design: docs/design/009-recording-monitoring.md
#pragma once

#include <arpa/inet.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace itchfh {

struct program_config {
    std::string group = "239.192.0.1", interface_ip = "127.0.0.1", server_ip = "127.0.0.1";
    std::uint16_t port = 26400, server_port = 26401;
    int buffer_bytes = 4 << 20, busy_poll_us = 0;
    bool batch = false;  // recvmmsg; the default recvmsg (note 007, D1)
    int cpu = 2, writer_cpu = 1;
    std::uint64_t hold_mb = 64, orders = 4'000'000, levels = 2'000'000;
    std::string publish = "/itchfh-pub", record_dir, monitor = "/itchfh-mon";  // empty: not done
    std::uint64_t digest = 0;
};

// False, with `why` naming the line, if anything is wrong.
inline bool parse_config(const std::string& text, program_config& c, std::string& why) {
    std::map<std::string, std::string> seen;
    std::istringstream in(text);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (const auto h = line.find('#'); h != std::string::npos) line.erase(h);
        const auto trim = [](std::string s) {
            const auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
        };
        line = trim(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        const auto bad = [&](const std::string& what) {
            why = "line " + std::to_string(n) + ": " + what;
            return false;
        };
        if (eq == std::string::npos) return bad("not `key = value`");
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (seen.count(k)) return bad(k + " given twice");
        seen[k] = v;
        const auto number = [&](std::uint64_t lo, std::uint64_t hi, std::uint64_t& out) {
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) return false;
            try {
                out = std::stoull(v);
            } catch (...) {
                return false;
            }
            return out >= lo && out <= hi;
        };
        const auto ip = [&](std::string& out) {
            in_addr a{};
            if (::inet_pton(AF_INET, v.c_str(), &a) != 1) return false;
            out = v;
            return true;
        };
        const auto shm_name = [&](std::string& out) {
            if (v == "none") {
                out.clear();
                return true;
            }
            if (v.size() < 2 || v[0] != '/' || v.find('/', 1) != std::string::npos) return false;
            out = v;
            return true;
        };
        std::uint64_t x = 0;
        if (k == "group") {
            if (!ip(c.group)) return bad("group: not an IPv4 address");
        } else if (k == "interface") {
            if (!ip(c.interface_ip)) return bad("interface: not an IPv4 address");
        } else if (k == "server") {
            if (!ip(c.server_ip)) return bad("server: not an IPv4 address");
        } else if (k == "port" || k == "server_port") {
            if (!number(1, 65535, x)) return bad(k + ": a port, 1 to 65535");
            (k == "port" ? c.port : c.server_port) = static_cast<std::uint16_t>(x);
        } else if (k == "buffer_bytes") {
            if (!number(65536, 1u << 30, x)) return bad("buffer_bytes: 65536 to 2^30");
            c.buffer_bytes = static_cast<int>(x);
        } else if (k == "busy_poll_us") {
            if (!number(0, 1000, x)) return bad("busy_poll_us: 0 to 1000");
            c.busy_poll_us = static_cast<int>(x);
        } else if (k == "read") {
            if (v != "recvmsg" && v != "recvmmsg") return bad("read: recvmsg or recvmmsg");
            c.batch = v == "recvmmsg";
        } else if (k == "cpu" || k == "writer_cpu") {
            if (!number(0, 1023, x)) return bad(k + ": a CPU number");
            (k == "cpu" ? c.cpu : c.writer_cpu) = static_cast<int>(x);
        } else if (k == "hold_mb") {
            if (!number(1, 4096, x)) return bad("hold_mb: 1 to 4096");
            c.hold_mb = x;
        } else if (k == "orders" || k == "levels") {
            if (!number(1000, 1'000'000'000, x)) return bad(k + ": 1000 to 10^9");
            (k == "orders" ? c.orders : c.levels) = x;
        } else if (k == "publish" || k == "monitor") {
            if (!shm_name(k == "publish" ? c.publish : c.monitor)) return bad(k + ": a shared-memory name (/name) or none");
        } else if (k == "record_dir") {
            c.record_dir = v == "none" ? std::string{} : v;
        } else {
            return bad("unknown key " + k);
        }
    }
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& [k, v] : seen)
        for (const char ch : k + "=" + v + "\n") h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    c.digest = h;
    return true;
}

inline bool load_config(const std::string& path, program_config& c, std::string& why) {
    std::ifstream f(path);
    if (!f) {
        why = path + ": cannot read";
        return false;
    }
    std::stringstream s;
    s << f.rdbuf();
    if (!parse_config(s.str(), c, why)) {
        why = path + ": " + why;
        return false;
    }
    return true;
}

}  // namespace itchfh
