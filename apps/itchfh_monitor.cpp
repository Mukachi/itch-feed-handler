// Reads itchfh's monitoring block (note 009, D6) and prints its counters and
// health, once or every second; exits 1 if the feed is unhealthy (stale,
// lost, silent, a datagram unrecorded, a consumer overrun), for scripts.
// Design: docs/design/009-recording-monitoring.md
//
// Usage: itchfh-monitor [block name, default /itchfh-mon] [--watch]
#include <itchfh/monitoring.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    namespace mon = itchfh::mon;
    std::string name = "/itchfh-mon";
    bool watch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--watch") watch = true;
        else name = a;
    }
    mon::block b;
    std::string why;
    if (!b.attach(name, why)) {
        std::fprintf(stderr, "itchfh-monitor: %s\n", why.c_str());
        return 2;
    }
    static const char* states[] = {"waiting", "live", "gapped", "stale", "lost", "ended"};
    for (;;) {
        mon::values v;
        b.read(v);
        const std::uint32_t h = mon::health(v);
        std::printf("feed %s, %llu ms since a datagram; health %s%s%s%s%s%s%s\n", v[mon::feed_state_now] < 6 ? states[v[mon::feed_state_now]] : "?",
                    static_cast<unsigned long long>(v[mon::since_datagram_ms]), mon::unhealthy(h) ? "UNHEALTHY" : "ok",
                    h & mon::feed_stale ? ", stale" : "", h & mon::feed_lost ? ", lost" : "", h & mon::feed_gapped ? ", gapped" : "",
                    h & mon::silent ? ", silent" : "", h & mon::recorder_unrecorded ? ", datagrams unrecorded" : "",
                    h & mon::publisher_overrun ? ", consumer overrun" : "");
        for (std::uint32_t i = 2; i < mon::n_counters; ++i)
            std::printf("  %-20s %llu\n", mon::names[i], static_cast<unsigned long long>(v[i]));
        std::fflush(stdout);
        if (!watch) return mon::unhealthy(h) ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
