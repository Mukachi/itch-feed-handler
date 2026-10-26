// Tests for include/itchfh/decoder.hpp. Spec: docs/design/004-decoder.md (S1 in
// small, S4): every alternative (A switch, B table, C padded, C unpadded)
// against the reference decoder, every field compared through a description
// written apart from the decoders' tables: one message of every type with
// every byte distinct, then a million random messages, valid and malformed
// (types, lengths). And S4: at the very end of a page followed by an unmapped
// one, A, B and C unpadded decode; C padded reads past and faults (shown in a
// child process); with the contract's 32 bytes after the message it does not.
#include "check.hpp"
#include "message_text.hpp"

#include <itchfh/decoder.hpp>
#include <itchfh/reference_decoder.hpp>

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace itchfh;
using itchfh::test::check;
using itchfh::test::message_text;

using decode_fn = decode_status (*)(const unsigned char*, std::size_t, message&);
struct alternative {
    const char* name;
    decode_fn f;
};
const alternative alternatives[] = {
    {"A switch", decode_switch}, {"B table", decode_table}, {"C SIMD, padded", decode_simd}, {"C SIMD, unpadded", decode_simd_unpadded}};

const char* const types = "SRHYLVWKJhAFECXDUPQBINO";

// What the reference decoder says, and what an alternative says, for bytes
// given with at least 32 readable bytes after them.
std::string differ(const alternative& a, const unsigned char* p, std::size_t n) {
    message r{}, x{};
    const decode_status rs = decode_reference(p, n, r);
    std::memset(&x, 0xA5, sizeof(x));  // what a decoder leaves unset must not matter
    const decode_status xs = a.f(p, n, x);
    if (rs != xs) return "refusal differs";
    if (rs == decode_status::ok && message_text(r) != message_text(x)) return "fields differ";
    return {};
}

void every_type() {
    for (const alternative& a : alternatives) {
        std::string problem;
        for (const char* t = types; *t && problem.empty(); ++t) {
            std::vector<unsigned char> b(message_length(*t) + 32);
            for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<unsigned char>(i * 37 + 11);  // every byte distinct
            b[0] = static_cast<unsigned char>(*t);
            if (auto d = differ(a, b.data(), message_length(*t)); !d.empty()) problem = std::string(1, *t) + ": " + d;
        }
        check(problem.empty(), std::string(a.name) + ": every type, every byte distinct, equal to the reference decoder" +
                                   (problem.empty() ? "" : " (" + problem + ")"));
    }
}

void random_messages() {
    for (const alternative& a : alternatives) {
        std::mt19937_64 rng(4);
        std::string problem;
        std::size_t ok = 0, refused = 0;
        std::vector<unsigned char> b(64 + 32);
        for (int i = 0; i < 1'000'000 && problem.empty(); ++i) {
            for (auto& x : b) x = static_cast<unsigned char>(rng());
            const std::uint64_t r = rng() % 100;
            if (r < 90) b[0] = static_cast<unsigned char>(types[rng() % 23]);
            std::size_t n = message_length(static_cast<char>(b[0]));
            if (r >= 80 || n == 0) n = rng() % 65;
            if (auto d = differ(a, b.data(), n); !d.empty()) problem = "message " + std::to_string(i) + " type " + std::to_string(b[0]) + " length " + std::to_string(n) + ": " + d;
            message tmp;
            (decode_reference(b.data(), n, tmp) == decode_status::ok ? ok : refused)++;
        }
        check(problem.empty() && ok > 700'000 && refused > 100'000,
              std::string(a.name) + ": a million random messages (" + std::to_string(ok) + " valid, " + std::to_string(refused) +
                  " refused for type or length), equal to the reference decoder" + (problem.empty() ? "" : " (" + problem + ")"));
    }
}

// A page of bytes with an unmapped page after it; the message ends `after`
// bytes before the boundary.
struct guarded {
    unsigned char* base = nullptr;
    long page = sysconf(_SC_PAGESIZE);
    guarded() {
        void* p = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base = static_cast<unsigned char*>(p);
        ::mprotect(base + page, page, PROT_NONE);
    }
    ~guarded() { ::munmap(base, 2 * page); }
    unsigned char* place(char type, std::size_t after) {
        const std::size_t n = message_length(type);
        unsigned char* m = base + page - after - n;
        for (std::size_t i = 0; i < n; ++i) m[i] = static_cast<unsigned char>(i * 13 + 5);
        m[0] = static_cast<unsigned char>(type);
        return m;
    }
};

// True if decoding faults (a child process takes the fault: it dies by the
// signal, or, under AddressSanitizer, whose handler reports it, exits with an
// error).
bool faults(decode_fn f, const unsigned char* p, std::size_t n) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        message m;
        f(p, n, m);
        ::_exit(0);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    return !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

void page_end() {
    guarded g;
    bool all_decode = true;
    for (const char* t = types; *t; ++t) {
        const unsigned char* m = g.place(*t, 0);
        for (const alternative& a : alternatives)
            if (a.f != decode_simd) all_decode &= !faults(a.f, m, message_length(*t)) && differ(a, m, message_length(*t)).empty();
    }
    check(all_decode, "S4: every type ending at an unmapped page: A, B and C unpadded decode, reading nothing past the message");
    const unsigned char* e = g.place('E', 0);  // 31 bytes: C reads its second chunk to byte 32
#if defined(__SSSE3__)
    check(faults(decode_simd, e, message_length('E')), "S4: C padded at an unmapped page reads past the message (it needs the contract)");
#else
    // No SSSE3 (arm64): decode_simd is A, which reads nothing past the message.
    check(!faults(decode_simd, e, message_length('E')), "S4: C padded is A on this architecture, and reads nothing past the message");
#endif
    bool with_contract = true;
    for (const char* t = types; *t; ++t) {
        const unsigned char* m = g.place(*t, 32);
        with_contract &= !faults(decode_simd, m, message_length(*t)) && differ(alternatives[2], m, message_length(*t)).empty();
    }
    check(with_contract, "S4: C padded with the contract's 32 bytes after every type: no fault, equal to the reference decoder");
}

}  // namespace

int main() {
    every_type();
    random_messages();
    page_end();
    return itchfh::test::result();
}
