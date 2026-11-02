// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace Kernel::Nx2SleepStats {

// EDEN_NX2_SLEEP_STATS: how late guest sleeps end, in two parts, logged every 10 s of guest time.
// "timer" is how late the kernel hardware timer fires after its deadline; "resume" is how late a
// thread in svcSleepThread runs again after its deadline (the timer plus waking its core and
// switching to it).
inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_SLEEP_STATS") != nullptr;
    return enabled;
}

struct Histogram {
    static constexpr std::array<s64, 8> Limits{5'000,   10'000,  20'000,  50'000,
                                               100'000, 200'000, 500'000, INT64_MAX};
    std::array<std::atomic<u64>, Limits.size()> counts{};
    std::atomic<u64> total_ns{};

    void Add(s64 late_ns) {
        late_ns = late_ns < 0 ? 0 : late_ns;
        for (size_t i = 0; i < Limits.size(); ++i) {
            if (late_ns < Limits[i]) {
                counts[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        total_ns.fetch_add(static_cast<u64>(late_ns), std::memory_order_relaxed);
    }

    std::string Take() {
        static constexpr std::array<const char*, Limits.size()> names{
            "<5", "<10", "<20", "<50", "<100", "<200", "<500", ">=500"};
        std::string text;
        u64 count = 0;
        for (size_t i = 0; i < Limits.size(); ++i) {
            const u64 n = counts[i].exchange(0, std::memory_order_relaxed);
            count += n;
            if (n != 0) {
                text += fmt::format(" {} us: {}", names[i], n);
            }
        }
        const u64 total = total_ns.exchange(0, std::memory_order_relaxed);
        return fmt::format("{} ({} total, {} us late on average)", text, count,
                           count != 0 ? total / count / 1000 : 0);
    }
};

inline Histogram& Timer() {
    static Histogram histogram;
    return histogram;
}

inline Histogram& Resume() {
    static Histogram histogram;
    return histogram;
}

// From the timer ending a sleep to the thread running again, by whether its core was idle then.
inline Histogram& WakeIdle() {
    static Histogram histogram;
    return histogram;
}

inline Histogram& WakeBusy() {
    static Histogram histogram;
    return histogram;
}

// The kernel scheduler lock: how long taking it waits, and how long it is held.
inline Histogram& LockWait() {
    static Histogram histogram;
    return histogram;
}

inline Histogram& LockHold() {
    static Histogram histogram;
    return histogram;
}

// The timer callback's parts: taking the scheduler lock, running the expired timer tasks, and
// leaving the lock (which reschedules and interrupts cores).
inline Histogram& TimerLock() {
    static Histogram histogram;
    return histogram;
}

inline Histogram& TimerTasks() {
    static Histogram histogram;
    return histogram;
}

inline Histogram& TimerUnlock() {
    static Histogram histogram;
    return histogram;
}

inline void MaybeLog(s64 now_ns) {
    static std::atomic<s64> next{0};
    s64 due = next.load(std::memory_order_relaxed);
    if (now_ns < due || !next.compare_exchange_strong(due, now_ns + 10'000'000'000LL)) {
        return;
    }
    if (due == 0) {
        Timer().Take();
        Resume().Take();
        WakeIdle().Take();
        WakeBusy().Take();
        LockWait().Take();
        LockHold().Take();
        TimerLock().Take();
        TimerTasks().Take();
        TimerUnlock().Take();
        return;
    }
    LOG_INFO(Kernel, "sleep stats: timer fires late{}", Timer().Take());
    LOG_INFO(Kernel, "sleep stats: sleeping threads resume late{}", Resume().Take());
    LOG_INFO(Kernel, "sleep stats: woken to running, core idle{}", WakeIdle().Take());
    LOG_INFO(Kernel, "sleep stats: woken to running, core busy{}", WakeBusy().Take());
    LOG_INFO(Kernel, "sleep stats: scheduler lock waits{}", LockWait().Take());
    LOG_INFO(Kernel, "sleep stats: scheduler lock holds{}", LockHold().Take());
    LOG_INFO(Kernel, "sleep stats: timer callback, taking the lock{}", TimerLock().Take());
    LOG_INFO(Kernel, "sleep stats: timer callback, timer tasks{}", TimerTasks().Take());
    LOG_INFO(Kernel, "sleep stats: timer callback, leaving the lock{}", TimerUnlock().Take());
}

} // namespace Kernel::Nx2SleepStats
