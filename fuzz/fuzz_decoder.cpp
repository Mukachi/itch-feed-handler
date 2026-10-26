// Differential fuzz target (note 004, S2): the same bytes through the reference
// decoder and every alternative; any difference in the refusal or in a decoded
// field aborts. The unpadded entries (A, B, C unpadded) read the input in place,
// at its exact length, so AddressSanitizer sees any read past it; C padded
// reads a copy with the contract's 32 bytes after it.
#include "../tests/message_text.hpp"

#include <itchfh/decoder.hpp>
#include <itchfh/reference_decoder.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace itchfh;
    message r{};
    const decode_status rs = decode_reference(data, size, r);
    const std::string want = rs == decode_status::ok ? itchfh::test::message_text(r) : std::string();
    const auto same = [&](decode_status xs, const message& x) {
        if (xs != rs || (rs == decode_status::ok && itchfh::test::message_text(x) != want)) std::abort();
    };
    message x{};
    same(decode_switch(data, size, x), x);
    same(decode_table(data, size, x), x);
    same(decode_simd_unpadded(data, size, x), x);
    std::vector<unsigned char> padded(size + 32, 0);
    if (size) std::memcpy(padded.data(), data, size);
    same(decode_simd(padded.data(), size, x), x);
    return 0;
}
