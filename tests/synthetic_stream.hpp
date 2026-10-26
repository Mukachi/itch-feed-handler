// A valid synthetic TotalView-ITCH stream for tests (notes 006 and 007): a
// directory, trading states, then orders added (some attributed), executed,
// cancelled, deleted and replaced, and statuses; decoded forms, encodable.
#pragma once

#include <itchfh/messages.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace itchfh::test {

// A valid stream: a directory, trading states, then orders added (some
// attributed), executed, cancelled, deleted, replaced; and statuses.
inline std::vector<message> stream(std::size_t n) {
    std::mt19937_64 rng(42);
    std::vector<message> v;
    std::vector<std::uint64_t> live;
    std::uint64_t ref = 1, match = 1;
    const auto msg = [&](char t, std::uint16_t l) {
        message m{};
        m.type = t;
        m.locate = l;
        m.timestamp_ns = v.size();
        return m;
    };
    message s = msg('S', 0);
    s.S.event_code = 'O';
    v.push_back(s);
    for (std::uint16_t l = 1; l <= 8; ++l) {
        message r = msg('R', l);
        r.R.stock = {'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '};
        r.R.round_lot_size = 100;
        v.push_back(r);
        message h = msg('H', l);
        h.H = {r.R.stock, l == 3 ? 'H' : 'T', ' ', {' ', ' ', ' ', ' '}};
        v.push_back(h);
    }
    std::vector<std::pair<std::uint64_t, std::uint16_t>> where;
    while (v.size() < n) {
        const std::uint16_t l = static_cast<std::uint16_t>(1 + rng() % 8);
        const std::uint64_t r = rng() % 100;
        if (r < 40 || live.size() < 20) {
            message a = msg(rng() % 5 ? 'A' : 'F', l);
            a.A = {ref, rng() % 2 ? 'B' : 'S', static_cast<std::uint32_t>(100 + rng() % 900), {'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '},
                   static_cast<std::uint32_t>(100'000 + (rng() % 20) * 100), {'M', 'P', static_cast<char>('A' + rng() % 26), 'D'}};
            if (a.type == 'A') a.A.attribution = {' ', ' ', ' ', ' '};
            live.push_back(ref);
            where.push_back({ref, l});
            ++ref;
            v.push_back(a);
        } else if (r < 80) {
            const std::size_t k = rng() % live.size();
            const std::uint64_t o = live[k];
            std::uint16_t ol = 0;
            for (const auto& [rr, ll] : where)
                if (rr == o) ol = ll;
            if (r < 55) {
                message d = msg('D', ol);
                d.D.ref = o;
                v.push_back(d);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
            } else if (r < 65) {
                message e = msg('E', ol);
                e.E = {o, 1, match++, 0, 0};
                v.push_back(e);
            } else if (r < 70) {
                message x = msg('X', ol);
                x.X = {o, 1};
                v.push_back(x);
            } else {
                message u = msg('U', ol);
                u.U = {o, ref, static_cast<std::uint32_t>(100 + rng() % 900), static_cast<std::uint32_t>(100'000 + (rng() % 20) * 100)};
                live[k] = ref;
                where.push_back({ref, ol});
                ++ref;
                v.push_back(u);
            }
        } else if (r < 85) {
            message y = msg('Y', l);
            y.Y = {{'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '}, "012"[rng() % 3]};
            v.push_back(y);
        } else if (r < 90) {
            message i = msg('I', l);
            i.I.paired_shares = rng() % 1000;
            i.I.direction = 'B';
            v.push_back(i);
        } else if (r < 93) {
            message h = msg('H', l);
            h.H = {{'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '}, "HPQT"[rng() % 4], ' ', {'R', 'S', 'N', ' '}};
            v.push_back(h);
        } else if (r < 95) {
            message o = msg('h', l);
            o.h = {{'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '}, "QBX"[rng() % 3], rng() % 2 ? 'H' : 'T'};
            v.push_back(o);
        } else {
            message nn = msg('N', l);
            nn.N = {{'S', static_cast<char>('0' + l), ' ', ' ', ' ', ' ', ' ', ' '}, "BSAN"[rng() % 4]};
            v.push_back(nn);
        }
    }
    return v;
}


}  // namespace itchfh::test
