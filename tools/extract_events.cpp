// The extraction tool of note 002, D7: turns a recorded TotalView-ITCH 5.0 day
// into a file of the order book's events, in feed order, until the decoder of
// note 004 exists (which must then produce the same events). Not part of the
// handler: it decodes only the fields the book needs, plainly, and is not on
// any measured path. Prints the count of every message type it read.
//
// Input on stdin, as Nasdaq's sample files are once decompressed: each message
// preceded by its length, two bytes, big-endian.
//
// Usage: gzip -dc data/01302020.NASDAQ_ITCH50.gz | extract_events <out.events> <source name>
#include <itchfh/event_file.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::uint64_t be(const unsigned char* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

// The message lengths TotalView-ITCH 5.0 gives for the types read here.
std::size_t expected_length(unsigned char t) {
    switch (t) {
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'H': return 25;
        default: return 0;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: extract_events <out.events> <source name>  (the day's messages on stdin)\n");
        return 2;
    }
    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "extract_events: cannot write %s\n", argv[1]);
        return 1;
    }
    itchfh::event_file_header h = itchfh::make_event_file_header(0, argv[2]);
    std::fwrite(&h, sizeof(h), 1, out);  // the count is written at the end
    std::vector<unsigned char> buf(1 << 16);
    std::vector<itchfh::event> batch;
    batch.reserve(1 << 16);
    std::uint64_t counts[256] = {}, events = 0, bad_length = 0;
    for (;;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, stdin) != 2) break;
        const std::size_t len = be(lb, 2);
        if (len == 0 || std::fread(buf.data(), 1, len, stdin) != len) break;
        const unsigned char* m = buf.data();
        ++counts[m[0]];
        const std::size_t want = expected_length(m[0]);
        if (want == 0) continue;
        if (len != want) {
            ++bad_length;
            continue;
        }
        itchfh::event e{};
        e.locate = static_cast<std::uint16_t>(be(m + 1, 2));
        e.timestamp_ns = be(m + 5, 6);
        switch (m[0]) {
            case 'A':
            case 'F':
                e.type = itchfh::event_type::add;
                e.ref = be(m + 11, 8);
                e.side = static_cast<char>(m[19]);
                e.shares = static_cast<std::uint32_t>(be(m + 20, 4));
                e.price = static_cast<std::uint32_t>(be(m + 32, 4));
                break;
            case 'E':
                e.type = itchfh::event_type::executed;
                e.ref = be(m + 11, 8);
                e.shares = static_cast<std::uint32_t>(be(m + 19, 4));
                break;
            case 'C':
                e.type = itchfh::event_type::executed_price;
                e.ref = be(m + 11, 8);
                e.shares = static_cast<std::uint32_t>(be(m + 19, 4));
                e.price = static_cast<std::uint32_t>(be(m + 32, 4));
                break;
            case 'X':
                e.type = itchfh::event_type::cancel;
                e.ref = be(m + 11, 8);
                e.shares = static_cast<std::uint32_t>(be(m + 19, 4));
                break;
            case 'D':
                e.type = itchfh::event_type::remove;
                e.ref = be(m + 11, 8);
                break;
            case 'U':
                e.type = itchfh::event_type::replace;
                e.ref = be(m + 11, 8);
                e.new_ref = be(m + 19, 8);
                e.shares = static_cast<std::uint32_t>(be(m + 27, 4));
                e.price = static_cast<std::uint32_t>(be(m + 31, 4));
                break;
            case 'H':
                e.type = itchfh::event_type::trading_state;
                e.side = static_cast<char>(m[19]);
                break;
        }
        batch.push_back(e);
        ++events;
        if (batch.size() == batch.capacity()) {
            std::fwrite(batch.data(), sizeof(itchfh::event), batch.size(), out);
            batch.clear();
        }
    }
    std::fwrite(batch.data(), sizeof(itchfh::event), batch.size(), out);
    h.count = events;
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(&h, sizeof(h), 1, out);
    if (std::fclose(out) != 0) {
        std::fprintf(stderr, "extract_events: write failed\n");
        return 1;
    }
    std::uint64_t messages = 0;
    for (const auto c : counts) messages += c;
    std::printf("messages %llu, events %llu, wrong length %llu\n", (unsigned long long)messages, (unsigned long long)events,
                (unsigned long long)bad_length);
    for (int c = 0; c < 256; ++c)
        if (counts[c]) std::printf("  %c %llu\n", c, (unsigned long long)counts[c]);
    return bad_length == 0 ? 0 : 1;
}
