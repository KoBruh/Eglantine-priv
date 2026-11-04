// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"

TEST_CASE("Host socket errors reach the guest as errors", "[core][sockets]") {
    using GuestErrno = Service::Sockets::Errno;
    REQUIRE(Service::Sockets::Translate(Network::Errno::SUCCESS) == GuestErrno::SUCCESS);
    // OTHER is the last host error: every value before it, and itself, is a failure.
    for (int value = static_cast<int>(Network::Errno::SUCCESS) + 1;
         value <= static_cast<int>(Network::Errno::OTHER); ++value) {
        INFO("Network::Errno " << value);
        REQUIRE(Service::Sockets::Translate(static_cast<Network::Errno>(value)) !=
                GuestErrno::SUCCESS);
    }
}

TEST_CASE("Host socket errors keep their meaning for the guest", "[core][sockets]") {
    using GuestErrno = Service::Sockets::Errno;
    using Service::Sockets::Translate;
    REQUIRE(Translate(Network::Errno::ADDRINUSE) == GuestErrno::ADDRINUSE);
    REQUIRE(Translate(Network::Errno::HOSTUNREACH) == GuestErrno::HOSTUNREACH);
    REQUIRE(Translate(Network::Errno::NETDOWN) == GuestErrno::NETDOWN);
    REQUIRE(Translate(Network::Errno::NETUNREACH) == GuestErrno::NETUNREACH);
    REQUIRE(Translate(Network::Errno::MSGSIZE) == GuestErrno::MSGSIZE);
    REQUIRE(Translate(Network::Errno::NOPROTOOPT) == GuestErrno::NOPROTOOPT);
    REQUIRE(Translate(Network::Errno::ALREADY) == GuestErrno::ALREADY);
    REQUIRE(Translate(Network::Errno::AGAIN) == GuestErrno::AGAIN);
}
