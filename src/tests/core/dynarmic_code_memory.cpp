// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && defined(ARCHITECTURE_x86_64)

#include <windows.h>
#include <psapi.h>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/interface/A64/a64.h"

namespace {

class CodeMemoryCallbacks final : public Dynarmic::A64::UserCallbacks {
public:
    std::uint64_t MemoryRead(Dynarmic::A64::VAddr, std::size_t) override {
        return 0x8B020020; // ADD X0, X1, X2
    }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr) override { return {}; }
    void MemoryWrite(Dynarmic::A64::VAddr, std::uint64_t, std::size_t) override {}
    void MemoryWrite128(Dynarmic::A64::VAddr, Dynarmic::A64::Vector) override {}
    void CallSVC(std::uint32_t) override {}
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1; }
    std::uint64_t GetCNTPCT() override { return 0; }
};

SIZE_T PrivateCommit() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    REQUIRE(GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)));
    return counters.PrivateUsage;
}

} // namespace

TEST_CASE("Dynarmic reserves its full code cache without committing the unused tail",
          "[core][dynarmic][memory]") {
    CodeMemoryCallbacks callbacks;
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &callbacks;
    config.code_cache_size = 512 * 1024 * 1024;
    const SIZE_T before = PrivateCommit();
    Dynarmic::A64::Jit jit{config};
    REQUIRE(PrivateCommit() < before + 128 * 1024 * 1024);
    for (int round = 0; round < 2; ++round) {
        jit.SetRegister(1, 19);
        jit.SetRegister(2, 23);
        jit.SetPC(0);
        jit.Step();
        REQUIRE(jit.GetRegister(0) == 42);
        REQUIRE(jit.GetPC() == 4);
        jit.ClearCache();
    }
}

#endif
