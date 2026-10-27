// Tests for include/itchfh/recording.hpp, replay.hpp, monitoring.hpp and
// config.hpp. Spec: docs/design/009-recording-monitoring.md: S3 the format
// (every record back as written; a file cut anywhere read to its last whole
// record and reported incomplete; a changed byte detected), S2 the writer
// held back (the feed's side never waits; what was not recorded is counted
// and marked where), S1 in small (a synthetic stream received over loopback
// multicast with losses, recorded, replayed from the file: the same books and
// the same published updates, the recording complete), S4 the monitoring
// block (never torn; health as forced; its counters the components'), and the
// configuration's checks.
#include "check.hpp"
#include "synthetic_stream.hpp"

#include <itchfh/book.hpp>
#include <itchfh/config.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/monitoring.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/recording.hpp>
#include <itchfh/replay.hpp>
#include <itchfh/session_store.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace itchfh;
using itchfh::test::check;

std::string temp(const std::string& what) {
    return (std::filesystem::temp_directory_path() / ("itchfh-rt-" + std::to_string(::getpid()) + "-" + what)).string();
}

udp::datagram synthetic_datagram(std::mt19937_64& rng, std::int64_t stamp) {
    udp::datagram d;
    d.length = 20 + static_cast<std::uint32_t>(rng() % 1400);
    d.kernel_ns = stamp;
    for (std::uint32_t i = 0; i < d.length; ++i) d.bytes[i] = static_cast<unsigned char>(rng());
    return d;
}

void format() {
    const std::string path = temp("format.rec");
    std::mt19937_64 rng(9);
    std::vector<udp::datagram> sent;
    {
        rec::recorder r;
        std::string why;
        if (!r.open(path, 42, -1, why)) {
            check(false, "the recording created: " + why);
            return;
        }
        for (int i = 0; i < 50'000; ++i) {
            sent.push_back(synthetic_datagram(rng, i));
            r.record(sent.back(), static_cast<std::uint8_t>(i & 1));
            while (r.backlog() > 8000) std::this_thread::yield();  // this test's own pace: nothing unrecorded here
        }
        r.close(3, 4);
    }
    rec::reader rd;
    std::string why;
    bool same = rd.open(path, why) && rd.header().config_digest == 42;
    rec::reader::record x;
    std::size_t i = 0;
    for (; same && rd.next(x) == rec::reader::result::record; ++i)
        same = i < sent.size() && x.h.length == sent[i].length && x.h.kernel_ns == sent[i].kernel_ns && x.h.socket == (i & 1) &&
               std::memcmp(x.bytes, sent[i].bytes, x.h.length) == 0;
    check(same && i == sent.size() && rd.has_trailer() && rd.trailer().recorded == sent.size() && rd.trailer().unrecorded == 0 &&
              rd.trailer().kernel_drops == 3 && rd.trailer().answer_drops == 4,
          "S3: 50,000 datagrams of 20 to 1,419 bytes, both sockets, written by the writer thread and read back, every one as it "
          "was, then the trailer with the counts");
    // Cut anywhere: read to the last whole record, then `truncated`.
    const auto size = std::filesystem::file_size(path);
    std::ifstream in(path, std::ios::binary);
    std::vector<char> all(size);
    in.read(all.data(), static_cast<std::streamsize>(size));
    bool cuts = true;
    for (const std::size_t cut : {std::size_t{64}, std::size_t{64 + 10}, size / 3, size / 2 + 7, size - 1}) {
        const std::string p2 = temp("cut.rec");
        std::ofstream(p2, std::ios::binary).write(all.data(), static_cast<std::streamsize>(cut));
        rec::reader r2;
        std::string w2;
        if (!r2.open(p2, w2)) {
            cuts = false;
            continue;
        }
        rec::reader::result got;
        std::size_t n = 0;
        while ((got = r2.next(x)) == rec::reader::result::record) ++n;
        cuts = cuts && got == rec::reader::result::truncated && !r2.has_trailer() && n <= sent.size();  // the last cut loses only the trailer
        std::filesystem::remove(p2);
    }
    // A changed byte in the middle of a record: `corrupt` there.
    std::vector<char> bad = all;
    bad[size / 2] ^= 0x40;
    const std::string p3 = temp("bad.rec");
    std::ofstream(p3, std::ios::binary).write(bad.data(), static_cast<std::streamsize>(size));
    rec::reader r3;
    std::string w3;
    rec::reader::result got = rec::reader::result::record;
    if (r3.open(p3, w3))
        while ((got = r3.next(x)) == rec::reader::result::record) {
        }
    check(cuts && got == rec::reader::result::corrupt,
          "S3: the file cut at five places (in the header's wake, mid-record, a third, a half, a byte short of the end): each "
          "read to its last whole record and reported truncated; a changed byte: the record reported corrupt, not replayed");
    std::filesystem::remove(path);
    std::filesystem::remove(p3);
}

void held_back() {
    const std::string path = temp("held.rec");
    rec::recorder r;
    std::string why;
    if (!r.open(path, 0, -1, why)) return;
    r.hold(true);
    std::mt19937_64 rng(3);
    for (int i = 0; i < 20'000; ++i) r.record(synthetic_datagram(rng, i), 0);  // never waits
    const std::uint64_t lost = r.unrecorded();
    r.hold(false);
    while (r.backlog() > 0) std::this_thread::yield();
    for (int i = 0; i < 10; ++i) r.record(synthetic_datagram(rng, 20'000 + i), 0);
    mon::values v{};
    v[mon::unrecorded] = r.unrecorded();
    const bool flagged = mon::health(v) & mon::recorder_unrecorded;
    r.close(0, 0);
    rec::reader rd;
    rec::reader::record x;
    std::uint64_t n = 0, skipped_at = 0, skipped = 0;
    if (rd.open(path, why))
        while (rd.next(x) == rec::reader::result::record) {
            ++n;
            if (x.h.skipped) {
                skipped_at = n;
                skipped = x.h.skipped;
            }
        }
    check(lost == 20'000 - rec::recorder::ring_slots && n == rec::recorder::ring_slots + 10 && skipped == lost &&
              skipped_at == rec::recorder::ring_slots + 1 && rd.trailer().unrecorded == lost && flagged,
          "S2: the writer held back, 20,000 datagrams offered: the ring's " + std::to_string(rec::recorder::ring_slots) +
              " kept, " + std::to_string(lost) + " not recorded, counted, never waited for; the next record says how many were "
              "skipped before it; the trailer and the monitoring's health say so");
    std::filesystem::remove(path);
}

// S1 in small: live over loopback, recorded; replayed from the file.
struct digest_consumer {
    pub::consumer c;
    pub::books b;
    std::uint64_t h = 1469598103934665603ULL, feed_updates = 0;
    void drain() {
        pub::update u;
        while (c.poll(u)) {
            b.apply(u);
            if (u.k == pub::kind::feed) {
                ++feed_updates;
                continue;
            }
            const auto* p = reinterpret_cast<const unsigned char*>(&u);
            for (std::size_t i = 0; i < sizeof(u); ++i) h = (h ^ p[i]) * 1099511628211ULL;
        }
    }
};

book_config small_book() {
    book_config c;
    c.orders = 50'000;
    c.levels = 50'000;
    c.ref_slots = 1 << 17;
    c.ref_span = 1 << 20;
    c.level_arena = 1 << 18;
    c.prefault = false;
    return c;
}
handler_config small_handler() {
    handler_config c;
    c.matches = 100'000;
    c.participants = 10'000;
    c.prefault = false;
    return c;
}
sequencer_config small_seq() {
    sequencer_config c;
    c.hold_bytes = 16u << 20;
    c.prefault = false;
    c.huge_pages = false;
    return c;
}

bool join(published<>& p, digest_consumer& d, const std::string& name) {
    std::string why;
    if (!d.c.attach(name, why)) return false;
    for (int i = 0; i < 100'000 && d.c.state() != pub::slot_live; ++i) {
        p.tick(0);
        d.drain();
    }
    return d.c.state() == pub::slot_live;
}

void live_and_replayed() {
    const std::vector<message> all = itchfh::test::stream(80'000);
    const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '9'};
    auto store = std::make_unique<session_store>(session, all.size(), all.size() * 64, false);
    unsigned char b[64];
    for (const message& m : all) store->append(b, encode(m, b));
    const std::string path = temp("live.rec"), pub_live = "/itchfh-rt-" + std::to_string(::getpid()) + "-live",
                      pub_replay = "/itchfh-rt-" + std::to_string(::getpid()) + "-replay", mon_name = "/itchfh-rt-" + std::to_string(::getpid()) + "-mon";
    std::string why;
    // Live.
    auto bk = std::make_unique<default_book>(small_book());
    auto p = std::make_unique<published<>>(*bk, small_handler());
    digest_consumer live;
    if (!p->pub.open(pub_live, why) || !join(*p, live, pub_live)) {
        check(false, "S1: the live publisher and its consumer: " + why);
        return;
    }
    udp::receiver rx;
    udp::receiver::options ro;
    ro.port = 26480;
    udp::sender tx;
    if (!rx.open(ro, why) || !tx.open(ro.group, 26480, "127.0.0.1", 26481, why)) {
        check(false, "S1: the sockets: " + why);
        return;
    }
    rx.set_server("127.0.0.1", 26481);
    rec::recorder r;
    if (!r.open(path, 7, -1, why)) return;
    mon::block mb;
    if (!mb.create(mon_name, why)) return;
    sender_config sc;
    sc.packets_per_s = 5'000;
    sc.drop = 0.02;
    sc.heartbeat_ns = 100'000'000;
    mold_sender s(*store, tx, sc);
    std::atomic<bool> stop{false};
    std::thread st([&] { s.run(stop); });
    feed_one<published<>, no_probe, rec::recorder> f(rx, *p, small_seq());
    f.set_recorder(r);
    // The consumer reads on a thread of its own, as a live one would: one pass of the feed can deliver a
    // recovered backlog larger than its ring (seen under TSan in CI, the consumer read between passes).
    std::atomic<bool> fed{false};
    std::thread live_reader([&] {
        while (!fed.load(std::memory_order_acquire)) live.drain();
        live.drain();
    });
    const std::int64_t give_up = monotonic_ns() + 60'000'000'000;
    mon::values v{};
    while (f.seq().state() != feed_state::ended && f.seq().state() != feed_state::lost && monotonic_ns() < give_up) f.poll();
    stop = true;
    st.join();
    fed.store(true, std::memory_order_release);
    live_reader.join();
    r.close(f.counts().kernel_drops, f.counts().answer_drops);
    mon::gather(v, f, *bk, &p->pub, &r, monotonic_ns());
    mb.write(v);
    mon::values back;
    mb.read(back);
    const bool counters = std::equal(std::begin(v), std::end(v), std::begin(back)) && back[mon::datagrams] == f.counts().datagrams &&
                          back[mon::messages] == f.counts().messages && back[mon::recorded] == r.written() && back[mon::unrecorded] == 0 &&
                          back[mon::consumers_live] == 1 && back[mon::feed_state_now] == static_cast<std::uint64_t>(feed_state::ended) &&
                          !mon::unhealthy(mon::health(back));
    // Replayed from the file, into a fresh book and publisher.
    auto bk2 = std::make_unique<default_book>(small_book());
    auto p2 = std::make_unique<published<>>(*bk2, small_handler());
    digest_consumer again;
    if (!p2->pub.open(pub_replay, why) || !join(*p2, again, pub_replay)) return;
    rec::reader rd;
    if (!rd.open(path, why)) return;
    // The replay runs at full speed: its consumer reads alongside, as a live one would.
    std::atomic<bool> replayed{false};
    std::thread reader_thread([&] {
        while (!replayed.load(std::memory_order_acquire)) again.drain();
        again.drain();
    });
    const replay_result rr = replay(rd, *p2, small_seq());
    replayed.store(true, std::memory_order_release);
    reader_thread.join();
    const std::uint64_t d1 = pub::digest_of(*bk), d2 = pub::digest_of(*bk2);
    // Each condition named, so a failure (as in CI under TSan) says which.
    const std::pair<const char*, bool> conds[] = {
        {"live not overrun", !live.c.overrun()}, {"replay not overrun", !again.c.overrun()},
        {"every message live", f.counts().messages == all.size()}, {"sender dropped", s.counts().dropped > 0},
        {"replay complete", rr.complete}, {"every message replayed", rr.messages == all.size()}, {"books live = replay", d1 == d2},
        {"live consumer's books", live.b.digest() == d1}, {"replay consumer's books", again.b.digest() == d2},
        {"updates byte for byte", live.h == again.h}, {"feed updates", live.feed_updates == again.feed_updates},
        {"monitoring counters", counters}};
    std::string failed;
    for (const auto& [name, ok] : conds)
        if (!ok) failed += std::string(failed.empty() ? " (failed: " : ", ") + name;
    if (!failed.empty())
        failed += "; feed updates " + std::to_string(live.feed_updates) + "/" + std::to_string(again.feed_updates) + ", replay state " +
                  std::to_string(static_cast<int>(rr.state)) + ", held " + std::to_string(rr.held) + ", skipped " + std::to_string(rr.skipped) + ")";
    check(failed.empty(),
          "S1 in small: " + std::to_string(all.size()) + " messages over loopback multicast, " + std::to_string(s.counts().dropped) +
              " packets dropped by the sender and recovered, " + std::to_string(r.written()) + " datagrams recorded from both "
              "sockets; replayed from the file: complete, the same books, the same published updates (" +
              std::to_string(live.b.updates) + ", byte for byte); the monitoring block's counters the components' own, healthy at the end" + failed);
    std::filesystem::remove(path);
}

void monitoring() {
    // Never torn: a writer, a reader on another thread.
    const std::string name = "/itchfh-rt-" + std::to_string(::getpid()) + "-mon2";
    mon::block w, rd;
    std::string why;
    if (!w.create(name, why) || !rd.attach(name, why)) {
        check(false, "S4: the block: " + why);
        return;
    }
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::uint64_t torn = 0, writes = 0;
    std::thread t([&] {
        while (!done) {
            mon::values v;
            rd.read(v);
            reads.fetch_add(1, std::memory_order_relaxed);
            for (std::uint32_t i = 1; i < mon::n_counters; ++i)
                if (v[i] != v[0]) ++torn;
        }
    });
    // At least 500,000 writes, and on until the reader has read 20,000 times among them.
    for (std::uint64_t k = 0; k < 500'000 || reads.load(std::memory_order_relaxed) < 20'000; ++k, ++writes) {
        mon::values v;
        for (auto& x : v) x = k;
        w.write(v);
    }
    done = true;
    t.join();
    // Health as forced.
    const auto with = [](mon::counter c, std::uint64_t x) {
        mon::values v{};
        v[mon::feed_state_now] = static_cast<std::uint64_t>(feed_state::live);
        v[mon::datagrams] = 1;
        v[c] = x;
        return mon::health(v);
    };
    const bool states = with(mon::feed_state_now, static_cast<std::uint64_t>(feed_state::stale)) == mon::feed_stale &&
                        with(mon::feed_state_now, static_cast<std::uint64_t>(feed_state::lost)) == mon::feed_lost &&
                        with(mon::feed_state_now, static_cast<std::uint64_t>(feed_state::gapped)) == mon::feed_gapped &&
                        !mon::unhealthy(mon::feed_gapped) && with(mon::since_datagram_ms, 2001) == mon::silent &&
                        with(mon::publisher_overruns, 1) == mon::publisher_overrun && with(mon::unrecorded, 1) == mon::recorder_unrecorded &&
                        with(mon::kernel_drops, 5) == 0;
    check(torn == 0 && reads >= 20'000 && states,
          "S4: the monitoring block, " + std::to_string(writes) + " writes against a reader on another thread (" + std::to_string(reads.load()) +
              " reads): never torn; health as forced: stale, lost, silent 2 s, a consumer overrun, a datagram unrecorded each "
              "unhealthy, gapped (recovery under way) and kernel drops alone not");
}

void configuration() {
    program_config c;
    std::string why;
    const bool good = parse_config("# a feed\ngroup = 239.192.0.9\ninterface=10.99.0.1\nport = 26500 # ours\nread = recvmmsg\n"
                                   "publish = none\nrecord_dir = /var/tmp\nhold_mb = 128\n",
                                   c, why) &&
                      c.group == "239.192.0.9" && c.interface_ip == "10.99.0.1" && c.port == 26500 && c.batch && c.publish.empty() &&
                      c.record_dir == "/var/tmp" && c.hold_mb == 128 && c.digest != 0;
    program_config d1, d2;
    const bool same_digest = parse_config("port = 1\ncpu = 3\n", d1, why) && parse_config("cpu = 3\nport = 1\n", d2, why) && d1.digest == d2.digest;
    program_config x;
    const bool refused = !parse_config("colour = blue\n", x, why) && why.find("unknown key colour") != std::string::npos &&
                         !parse_config("port = 70000\n", x, why) && !parse_config("group = 239.192.0\n", x, why) &&
                         !parse_config("cpu = 2\ncpu = 3\n", x, why) && why.find("twice") != std::string::npos &&
                         !parse_config("publish = no-slash\n", x, why) && !parse_config("just words\n", x, why);
    check(good && same_digest && refused,
          "D7: the configuration: keys, comments and spacing read; the digest the same whatever the order; an unknown key, a "
          "port out of range, a bad address, a key twice, a bad shared-memory name and a line without `=` each refused with its line");
}

}  // namespace

int main() {
    format();
    held_back();
    live_and_replayed();
    monitoring();
    configuration();
    return itchfh::test::result();
}
