// This thread's hardware counters, user space only, for telling two runs'
// levels apart (note 005's sequencer runs, Review 2026-10-26): cycles and
// instructions (Broadwell's fixed counters), and branch mispredictions,
// last-level cache misses, L1 data read misses and data-TLB read misses (its
// four general ones), so the six count together without multiplexing. Each is
// read with the kernel's enabled and running times, which must agree (else
// the kernel multiplexed it, and it is marked). Read outside the timing
// windows. Needs perf_event_paranoid 2 or lower (bench-on sets it).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

class counters {
public:
    static constexpr int n = 6;
    static constexpr const char* names[n] = {"cycles", "instructions", "branch_misses", "llc_misses", "l1d_read_misses", "dtlb_read_misses"};

    struct reading {
        std::array<std::uint64_t, n> value{};
        bool multiplexed = false;
        reading operator-(const reading& o) const {
            reading r;
            for (int i = 0; i < n; ++i) r.value[i] = value[i] - o.value[i];
            r.multiplexed = multiplexed || o.multiplexed;
            return r;
        }
    };

    counters() {
        const auto cache = [](std::uint64_t id) {
            return id | (std::uint64_t{PERF_COUNT_HW_CACHE_OP_READ} << 8) | (std::uint64_t{PERF_COUNT_HW_CACHE_RESULT_MISS} << 16);
        };
        const std::pair<std::uint32_t, std::uint64_t> ev[n] = {
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},     {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},  {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
            {PERF_TYPE_HW_CACHE, cache(PERF_COUNT_HW_CACHE_L1D)}, {PERF_TYPE_HW_CACHE, cache(PERF_COUNT_HW_CACHE_DTLB)}};
        for (int i = 0; i < n; ++i) {
            perf_event_attr a{};
            a.type = ev[i].first;
            a.size = sizeof(a);
            a.config = ev[i].second;
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            a.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
            fd_[i] = static_cast<int>(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
        }
    }
    ~counters() {
        for (const int fd : fd_)
            if (fd >= 0) close(fd);
    }
    counters(const counters&) = delete;
    counters& operator=(const counters&) = delete;

    bool ok() const {
        for (const int fd : fd_)
            if (fd < 0) return false;
        return true;
    }
    bool open(int i) const { return fd_[i] >= 0; }
    reading read() const {
        reading r;
        for (int i = 0; i < n; ++i) {
            std::uint64_t v[3] = {};
            if (fd_[i] >= 0 && ::read(fd_[i], v, sizeof(v)) == sizeof(v)) {
                r.value[i] = v[0];
                r.multiplexed |= v[1] != v[2];
            }
        }
        return r;
    }

private:
    int fd_[n] = {-1, -1, -1, -1, -1, -1};
};
