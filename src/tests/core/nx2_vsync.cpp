// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_features.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hle/service/nx2_gsv.h"

namespace {

// Single-core CoreTiming counts emulated CPU ticks, so time only passes here, in 1 ms steps.
void AdvanceMs(Core::Timing::CoreTiming& timing, u64 ms) {
    for (u64 i = 0; i < ms; ++i) {
        timing.AddTicks(Common::WallClock::CPUTickFreq / 1000);
        timing.Advance();
    }
}

} // Anonymous namespace

TEST_CASE("nx2:vi2 vsync events follow a refresh rate changed while running", "[nx2-vsync]") {
    if (std::getenv("EDEN_NX2_VSYNC_HZ") != nullptr) {
        SKIP("EDEN_NX2_VSYNC_HZ overrides the refresh rate");
    }
    const u32 saved_rate = Settings::values.nx2_refresh_rate.GetValue();
    Settings::values.nx2_refresh_rate.SetValue(60U);

    Core::Timing::CoreTiming timing;
    timing.SetMulticore(false);
    timing.Initialize([] {});

    u32 vblanks = 0;
    const auto vsync = Service::Nx2CreateVsyncTimer([&vblanks] { ++vblanks; });
    const auto interval = Service::Nx2VsyncInterval();
    timing.ScheduleLoopingEvent(interval, interval, vsync);

    AdvanceMs(timing, 1000);
    const u32 at_60_hz = vblanks;

    // The "FPS Limit (FPS mods)" setting changed during play: the next vblank is still a 60 Hz
    // one, every one after it comes at 120 Hz.
    Settings::values.nx2_refresh_rate.SetValue(120U);
    vblanks = 0;
    AdvanceMs(timing, 1000);
    const u32 at_120_hz = vblanks;

    timing.UnscheduleEvent(vsync);
    Settings::values.nx2_refresh_rate.SetValue(saved_rate);

    REQUIRE(at_60_hz >= 59);
    REQUIRE(at_60_hz <= 61);
    REQUIRE(at_120_hz >= 118);
    REQUIRE(at_120_hz <= 121);
}
