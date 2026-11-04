// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

namespace {

/// How long a test waits for a call that has to end, so that one that hangs fails the test rather
/// than hanging it.
constexpr std::chrono::seconds watchdog_timeout{10};
constexpr s32 watchdog_timeout_ms = 10'000;

/// Has listener listen on a free loopback TCP port, and returns that port's address.
Network::SockAddrIn ListenOnLoopback(Network::Socket& listener) {
    REQUIRE(listener.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(listener.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) == Network::Errno::SUCCESS);
    REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
    const auto [address, address_error] = listener.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);
    return address;
}

/// Accepts the connection coming to listener, without waiting in the accept itself, as the bsd
/// service does: it waits for the socket to poll readable, which a deferred accept waits for.
std::unique_ptr<Network::SocketBase> AcceptConnection(Network::Socket& listener) {
    std::vector<Network::PollFD> pollfds{Network::PollFD{
        .socket = &listener,
        .events = Network::PollEvents::RdNorm,
        .revents = Network::PollEvents{},
    }};
    const auto [polled, poll_error] = Network::Poll(pollfds, watchdog_timeout_ms);
    REQUIRE(polled > 0);
    REQUIRE(True(pollfds[0].revents & Network::PollEvents::RdNorm));
    auto [accepted, accept_error] = listener.Accept(Network::FLAG_MSG_DONTWAIT);
    REQUIRE(accept_error == Network::Errno::SUCCESS);
    REQUIRE(accepted.socket != nullptr);
    return std::move(accepted.socket);
}

} // namespace

TEST_CASE("Network::Errors", "[core]") {
    Network::NetworkInstance network_instance; // initialize network

    Network::Socket socks[2];
    for (Network::Socket& sock : socks) {
        REQUIRE(sock.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    }

    Network::SockAddrIn addr{
        Network::Domain::INET,
        {127, 0, 0, 1},
        1, // hopefully nobody running this test has something listening on port 1
    };
    REQUIRE(socks[0].Connect(addr, 0) == Network::Errno::CONNREFUSED);

    std::vector<u8> message{1, 2, 3, 4};
    REQUIRE(socks[1].Recv(0, message).second == Network::Errno::NOTCONN);
}

TEST_CASE("Network::PerCallNonblockingReceive", "[core][sockets]") {
    using namespace std::chrono_literals;
    Network::NetworkInstance network_instance;
    Network::Socket receiver;
    Network::Socket sender;
    REQUIRE(receiver.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                                Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(sender.Initialize(Network::Domain::INET, Network::Type::DGRAM,
                              Network::Protocol::UDP) == Network::Errno::SUCCESS);
    REQUIRE(receiver.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(receiver.SetRcvTimeo(2000) == Network::Errno::SUCCESS);
    const auto [address, address_error] = receiver.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);

    auto blocking = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        const auto [size, error] = receiver.RecvFrom(0, buffer, nullptr);
        return std::pair{std::pair{size, error}, buffer[0]};
    });
    const bool remained_blocked = blocking.wait_for(100ms) == std::future_status::timeout;
    auto no_wait = std::async(std::launch::async, [&] {
        std::array<u8, 1> buffer{};
        return receiver.RecvFrom(Network::FLAG_MSG_DONTWAIT, buffer, nullptr);
    });
    const bool returned_promptly = no_wait.wait_for(1s) == std::future_status::ready;
    const std::array<u8, 1> data{'x'};
    const auto first_send = sender.SendTo(0, data, &address);
    const auto second_send = sender.SendTo(0, data, &address);
    const auto no_wait_result = no_wait.get();
    const auto [blocking_result, received] = blocking.get();

    CHECK(remained_blocked);
    CHECK(returned_promptly);
    CHECK(no_wait_result.first == -1);
    CHECK(no_wait_result.second == Network::Errno::AGAIN);
    CHECK(first_send.second == Network::Errno::SUCCESS);
    CHECK(second_send.second == Network::Errno::SUCCESS);
    CHECK(blocking_result.first == 1);
    CHECK(blocking_result.second == Network::Errno::SUCCESS);
    CHECK(received == 'x');
}

TEST_CASE("Network::CloseWakesBlockedReceive", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    Network::Socket listener;
    const Network::SockAddrIn address = ListenOnLoopback(listener);
    Network::Socket client;
    REQUIRE(client.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(client.Connect(address, 0) == Network::Errno::SUCCESS);
    const std::unique_ptr<Network::SocketBase> server = AcceptConnection(listener);

    // A blocking receive with no timeout, which nothing but the close below can end: the peer
    // sends nothing.
    std::promise<void> receiving;
    std::promise<std::pair<s32, Network::Errno>> received;
    std::future<std::pair<s32, Network::Errno>> receive_done = received.get_future();
    std::thread receiver([&server, &receiving, &received] {
        std::array<u8, 1> buffer{};
        receiving.set_value();
        received.set_value(server->Recv(0, buffer));
    });
    receiving.get_future().wait();
    const Network::Errno close_error = server->Close();
    const bool woke = receive_done.wait_for(watchdog_timeout) == std::future_status::ready;
    if (!woke) {
        // End the receive as the peer would, so that the thread can be joined.
        client.Close();
    }
    receiver.join();

    CHECK(close_error == Network::Errno::SUCCESS);
    REQUIRE(woke);
    // The end of the stream, or a failure of the receive on a closed socket.
    REQUIRE(receive_done.get().first <= 0);
}

TEST_CASE("Network::ConnectWithoutWaitingReportsRefusal", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    // A port bound without listening refuses connections, and stays taken meanwhile.
    Network::Socket refuser;
    REQUIRE(refuser.Initialize(Network::Domain::INET, Network::Type::STREAM,
                               Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(refuser.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) == Network::Errno::SUCCESS);
    const auto [address, address_error] = refuser.GetSockName();
    REQUIRE(address_error == Network::Errno::SUCCESS);

    // On a blocking socket, the connect returns at once, as the bsd service's deferred connect
    // needs, and reports the connection in progress as the console does.
    Network::Socket client;
    REQUIRE(client.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);
    const Network::Errno started = client.Connect(address, Network::FLAG_MSG_DONTWAIT);
    REQUIRE((started == Network::Errno::INPROGRESS || started == Network::Errno::CONNREFUSED));
    if (started == Network::Errno::CONNREFUSED) {
        return;
    }

    // The deferred connect waits for the socket to poll writable or failed, then SO_ERROR tells
    // the outcome.
    std::vector<Network::PollFD> pollfds{Network::PollFD{
        .socket = &client,
        .events = Network::PollEvents::Out,
        .revents = Network::PollEvents{},
    }};
    const auto [polled, poll_error] = Network::Poll(pollfds, watchdog_timeout_ms);
    REQUIRE(polled > 0);
    REQUIRE(True(pollfds[0].revents & (Network::PollEvents::Out | Network::PollEvents::Err |
                                       Network::PollEvents::Hup)));
    const auto [pending_error, getsockopt_error] = client.GetPendingError();
    REQUIRE(getsockopt_error == Network::Errno::SUCCESS);
    REQUIRE(pending_error == Network::Errno::CONNREFUSED);
}

TEST_CASE("Network::AcceptWithoutWaiting", "[core][sockets]") {
    Network::NetworkInstance network_instance;
    Network::Socket listener;
    const Network::SockAddrIn address = ListenOnLoopback(listener);
    Network::Socket client;
    REQUIRE(client.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);

    // With no connection to accept, a blocking socket's accept fails at once instead of waiting.
    std::future<Network::Errno> empty_accept = std::async(std::launch::async, [&listener] {
        return listener.Accept(Network::FLAG_MSG_DONTWAIT).second;
    });
    const bool returned = empty_accept.wait_for(watchdog_timeout) == std::future_status::ready;
    if (!returned) {
        // Give the accept a connection to end with, so that the thread can be joined.
        client.Connect(address, 0);
    }
    const Network::Errno empty_error = empty_accept.get();
    REQUIRE(returned);
    REQUIRE(empty_error == Network::Errno::AGAIN);

    REQUIRE(client.Connect(address, 0) == Network::Errno::SUCCESS);
    const std::unique_ptr<Network::SocketBase> server = AcceptConnection(listener);

    // The accepted socket blocks, as its Socket starts out, though accepting without waiting
    // made the listening host socket nonblocking, which Winsock hands on to it: its receive waits
    // for the byte sent below rather than failing with EAGAIN.
    std::promise<void> receiving;
    std::future<std::pair<s32, Network::Errno>> receive_done =
        std::async(std::launch::async, [&server, &receiving] {
            std::array<u8, 1> buffer{};
            receiving.set_value();
            return server->Recv(0, buffer);
        });
    receiving.get_future().wait();
    const std::array<u8, 1> byte{'x'};
    const auto sent = client.Send(byte, 0);
    const bool woke = receive_done.wait_for(watchdog_timeout) == std::future_status::ready;
    if (!woke) {
        client.Close();
    }
    const auto received = receive_done.get();

    CHECK(sent.first == 1);
    REQUIRE(woke);
    REQUIRE(received == std::pair<s32, Network::Errno>{1, Network::Errno::SUCCESS});
}
