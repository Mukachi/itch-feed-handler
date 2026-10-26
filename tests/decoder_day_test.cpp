// The decoders over Nasdaq's sample day (note 004, S1), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// Every message of the day through the reference decoder and every
// alternative (A switch, B table, C padded from a buffer that keeps the
// contract, C unpadded), every field compared through a description written
// apart from the decoders' tables.
//
// Usage: decoder_day_test [day.gz]
#include "check.hpp"
#include "message_text.hpp"

#include <itchfh/decoder.hpp>
#include <itchfh/reference_decoder.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace itchfh;
    using itchfh::test::check;
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
    if (!in) {
        check(false, path + ": cannot read (tools/fetch_day.py fetches it)");
        return itchfh::test::result();
    }
    using decode_fn = decode_status (*)(const unsigned char*, std::size_t, message&);
    const struct {
        const char* name;
        decode_fn f;
    } alternatives[] = {{"A switch", decode_switch}, {"B table", decode_table}, {"C SIMD, padded", decode_simd}, {"C SIMD, unpadded", decode_simd_unpadded}};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<unsigned char> buf((1 << 16) + 32, 0);  // the contract: 32 readable bytes after any message
    std::uint64_t n = 0, differing[4] = {}, refused = 0;
    std::string first[4];
    for (;;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf.data(), 1, len, in) != len) break;
        ++n;
        message r{};
        const decode_status rs = decode_reference(buf.data(), len, r);
        refused += rs != decode_status::ok;
        const std::string want = rs == decode_status::ok ? itchfh::test::message_text(r) : std::string();
        for (int a = 0; a < 4; ++a) {
            message x;
            std::memset(&x, 0xA5, sizeof(x));
            const decode_status xs = alternatives[a].f(buf.data(), len, x);
            if (xs != rs || (rs == decode_status::ok && itchfh::test::message_text(x) != want)) {
                if (differing[a]++ == 0) first[a] = "message " + std::to_string(n) + " (" + static_cast<char>(buf[0]) + ")";
            }
        }
    }
    ::pclose(in);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(n > 0 && refused == 0, std::to_string(n) + " messages, every one decoded by the reference decoder");
    for (int a = 0; a < 4; ++a)
        check(differing[a] == 0, std::string("S1: ") + alternatives[a].name + ": equal to the reference decoder on every message of the day" +
                                     (differing[a] ? " (" + std::to_string(differing[a]) + " differ, first " + first[a] + ")" : ""));
    std::printf("(%.0f s)\n", s);
    return itchfh::test::result();
}
