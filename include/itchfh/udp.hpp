// UDP for the feed (note 007): the receiver's multicast socket and, beside it,
// its request socket, which sends re-requests and takes their unicast answers
// (the MoldUDP64 specification: answers go to the asker). Two sockets, not
// one (D1, amended): on one, an answer queues behind every multicast datagram
// not yet read, and a receiver behind the sender gave up while its answers
// sat in its own buffer. Then the sender's sockets. Datagrams are read in batches (recvmmsg, D1) or one at a
// time (recvmsg, beside it), each into a fixed buffer with room after it for
// the decoder's padding (note 004, D2), with the kernel's receive time
// (SO_TIMESTAMPNS, D8) and its running count of datagrams it dropped for want
// of buffer (SO_RXQ_OVFL, D3). Non-blocking throughout; the caller polls (D2).
// Design: docs/design/007-receiver-framer.md
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace itchfh::udp {

inline constexpr std::size_t datagram_room = 1500 + 64;  // an Ethernet payload, and the decoder's padding

struct datagram {
    std::uint32_t length = 0;
    std::uint32_t drops = 0;        // the kernel's count of datagrams dropped on this socket, so far
    std::int64_t kernel_ns = 0;     // CLOCK_REALTIME, when the kernel took it
    alignas(64) unsigned char bytes[datagram_room];
};

inline bool address(const std::string& ip, std::uint16_t port, sockaddr_in& a) {
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    return ::inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1;
}

class receiver {
public:
    struct options {
        std::string group = "239.192.0.1", interface_ip = "127.0.0.1";
        std::uint16_t port = 26400;
        int buffer_bytes = 4 << 20;  // D3: asked for; the kernel caps it at net.core.rmem_max
        int busy_poll_us = 0;        // D2: SO_BUSY_POLL, 0 none
    };

    receiver() = default;
    receiver(const receiver&) = delete;
    receiver& operator=(const receiver&) = delete;
    ~receiver() {
        if (fd_ >= 0) ::close(fd_);
        if (req_fd_ >= 0) ::close(req_fd_);
    }

    // False, with `why`, if any step fails.
    bool open(const options& o, std::string& why) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return fail(why, "socket");
        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &o.buffer_bytes, sizeof(o.buffer_bytes)) != 0) return fail(why, "SO_RCVBUF");
        socklen_t sl = sizeof(granted_);
        ::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &granted_, &sl);
        if (::setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)) != 0) return fail(why, "SO_RXQ_OVFL");
        if (::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) != 0) return fail(why, "SO_TIMESTAMPNS");
        if (o.busy_poll_us > 0 && ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &o.busy_poll_us, sizeof(o.busy_poll_us)) != 0)
            return fail(why, "SO_BUSY_POLL");
        sockaddr_in a;
        address("0.0.0.0", o.port, a);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return fail(why, "bind");
        ip_mreq m{};
        if (::inet_pton(AF_INET, o.group.c_str(), &m.imr_multiaddr) != 1 || ::inet_pton(AF_INET, o.interface_ip.c_str(), &m.imr_interface) != 1)
            return fail(why, "group or interface address");
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) != 0) return fail(why, "IP_ADD_MEMBERSHIP");
        // The request socket: any free port on the interface; answers come back to it.
        req_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (req_fd_ < 0) return fail(why, "request socket");
        if (::setsockopt(req_fd_, SOL_SOCKET, SO_RCVBUF, &o.buffer_bytes, sizeof(o.buffer_bytes)) != 0) return fail(why, "request SO_RCVBUF");
        if (::setsockopt(req_fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)) != 0) return fail(why, "request SO_RXQ_OVFL");
        if (::setsockopt(req_fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) != 0) return fail(why, "request SO_TIMESTAMPNS");
        address(o.interface_ip, 0, a);
        if (::bind(req_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return fail(why, "bind the request socket");
        return true;
    }
    // Where re-requests go (the re-request server).
    bool set_server(const std::string& ip, std::uint16_t port) { return address(ip, port, server_); }
    int granted_buffer() const { return granted_; }  // what the kernel reports (it doubles what was asked, within the cap)

    // D1: up to `n` datagrams from the group, into `d`; how many (0 if none waiting).
    int receive_batch(datagram* d, int n) { return read_batch(fd_, d, n); }
    // Answers to re-requests, the same way, from the request socket.
    int receive_answers(datagram* d, int n) { return read_batch(req_fd_, d, n); }
    // D1's alternative: one datagram; false if none waiting.
    bool receive_one(datagram& d) {
        iovec v{d.bytes, sizeof(d.bytes)};
        msghdr h{};
        h.msg_iov = &v;
        h.msg_iovlen = 1;
        h.msg_control = control_[0].data();
        h.msg_controllen = control_[0].size();
        const ssize_t n = ::recvmsg(fd_, &h, MSG_DONTWAIT);
        if (n < 0) return false;
        read_control(h, static_cast<unsigned>(n), d);
        return true;
    }
    // A re-request packet to the server, from the request socket.
    bool send_request(const unsigned char* p, std::size_t n) {
        return ::sendto(req_fd_, p, n, 0, reinterpret_cast<const sockaddr*>(&server_), sizeof(server_)) == static_cast<ssize_t>(n);
    }

    static constexpr int max_batch = 64;

private:
    int read_batch(int fd, datagram* d, int n) {
        if (n > max_batch) n = max_batch;
        for (int i = 0; i < n; ++i) {
            iov_[i] = {d[i].bytes, sizeof(d[i].bytes)};
            msgs_[i].msg_hdr = {};
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
            msgs_[i].msg_hdr.msg_control = control_[i].data();
            msgs_[i].msg_hdr.msg_controllen = control_[i].size();
        }
        const int got = ::recvmmsg(fd, msgs_, static_cast<unsigned>(n), MSG_DONTWAIT, nullptr);
        if (got <= 0) return 0;
        for (int i = 0; i < got; ++i) read_control(msgs_[i].msg_hdr, msgs_[i].msg_len, d[i]);
        return got;
    }
    static bool fail(std::string& why, const char* what) {
        why = std::string(what) + ": " + std::strerror(errno);
        return false;
    }
    void read_control(const msghdr& h, unsigned len, datagram& d) {
        d.length = len;
        for (cmsghdr* c = CMSG_FIRSTHDR(const_cast<msghdr*>(&h)); c; c = CMSG_NXTHDR(const_cast<msghdr*>(&h), c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SO_RXQ_OVFL) std::memcpy(&d.drops, CMSG_DATA(c), sizeof(d.drops));
            if (c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec t;
                std::memcpy(&t, CMSG_DATA(c), sizeof(t));
                d.kernel_ns = static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
            }
        }
    }

    int fd_ = -1, req_fd_ = -1, granted_ = 0;
    sockaddr_in server_{};
    mmsghdr msgs_[max_batch]{};
    iovec iov_[max_batch]{};
    std::array<char, 128> control_[max_batch]{};
};

// The sender's side: multicast out on an interface, and a unicast socket for
// re-requests (the re-request server answers to whoever asked).
class sender {
public:
    sender() = default;
    sender(const sender&) = delete;
    sender& operator=(const sender&) = delete;
    ~sender() {
        if (out_ >= 0) ::close(out_);
        if (req_ >= 0) ::close(req_);
    }
    bool open(const std::string& group, std::uint16_t port, const std::string& interface_ip, std::uint16_t request_port, std::string& why) {
        out_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        req_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (out_ < 0 || req_ < 0) return fail(why, "socket");
        in_addr i{};
        if (::inet_pton(AF_INET, interface_ip.c_str(), &i) != 1) return fail(why, "interface address");
        if (::setsockopt(out_, IPPROTO_IP, IP_MULTICAST_IF, &i, sizeof(i)) != 0) return fail(why, "IP_MULTICAST_IF");
        const unsigned char loop = 1, ttl = 1;
        ::setsockopt(out_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        ::setsockopt(out_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        const int big = 4 << 20;
        ::setsockopt(out_, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        if (!address(group, port, group_)) return fail(why, "group address");
        sockaddr_in r;
        address(interface_ip, request_port, r);
        if (::bind(req_, reinterpret_cast<sockaddr*>(&r), sizeof(r)) != 0) return fail(why, "bind the request port");
        return true;
    }
    bool send(const unsigned char* p, std::size_t n) {
        return ::sendto(out_, p, n, 0, reinterpret_cast<const sockaddr*>(&group_), sizeof(group_)) == static_cast<ssize_t>(n);
    }
    // A waiting request, if any: its bytes and who sent it.
    bool request(unsigned char* p, std::size_t room, std::size_t& n, sockaddr_in& from) {
        socklen_t fl = sizeof(from);
        const ssize_t got = ::recvfrom(req_, p, room, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fl);
        if (got < 0) return false;
        n = static_cast<std::size_t>(got);
        return true;
    }
    bool answer(const unsigned char* p, std::size_t n, const sockaddr_in& to) {
        return ::sendto(req_, p, n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == static_cast<ssize_t>(n);
    }

private:
    static bool fail(std::string& why, const char* what) {
        why = std::string(what) + ": " + std::strerror(errno);
        return false;
    }
    int out_ = -1, req_ = -1;
    sockaddr_in group_{};
};

}  // namespace itchfh::udp
