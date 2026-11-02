// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/thread.h"

TEST_CASE("An event reports a timeout without a signal", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    const auto start = std::chrono::steady_clock::now();
    const bool signalled = event.WaitFor(100ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK_FALSE(signalled);
    CHECK(elapsed >= 80ms);
}

TEST_CASE("An event waits for a later signal", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    std::thread signal_thread{[&] {
        std::this_thread::sleep_for(1300ms);
        event.Set();
    }};
    const bool signalled = event.WaitFor(2500ms);
    signal_thread.join();
    CHECK(signalled);
    CHECK_FALSE(event.IsSet());
}

TEST_CASE("An event consumes an existing signal", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    event.Set();
    CHECK(event.WaitFor(0ns));
    CHECK_FALSE(event.WaitFor(0ns));
}
