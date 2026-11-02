// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/compilation_budget.h"
#include "shader_recompiler/object_pool.h"

namespace {

struct OwnedValue {
    explicit OwnedValue(int& live_) : live{live_}, value{std::make_unique<int>(42)} {
        ++live;
    }

    ~OwnedValue() {
        --live;
    }

    int& live;
    std::unique_ptr<int> value;
};

constexpr std::uint64_t GiB = 1ULL << 30;

} // namespace

TEST_CASE("Shader pool releases owned objects when an exact chunk is reused", "[shader]") {
    int live{};
    Shader::ObjectPool<OwnedValue> pool{4};
    for (int pass = 0; pass < 3; ++pass) {
        for (int index = 0; index < 4; ++index) {
            const auto* value = pool.Create(live);
            REQUIRE(*value->value == 42);
        }
        REQUIRE(live == 4);
        pool.ReleaseContents();
        REQUIRE(live == 0);
    }
}

TEST_CASE("Shader pool releases several chunks and remaining objects", "[shader]") {
    int live{};
    {
        Shader::ObjectPool<OwnedValue> pool{2};
        for (int index = 0; index < 7; ++index) {
            static_cast<void>(pool.Create(live));
        }
        REQUIRE(live == 7);
        pool.ReleaseContents();
        REQUIRE(live == 0);
        static_cast<void>(pool.Create(live));
        REQUIRE(live == 1);
    }
    REQUIRE(live == 0);
}

TEST_CASE("Shader compiler reserves CPU capacity and respects physical memory", "[shader]") {
    REQUIRE(Shader::CompilationWorkerCount(12, 16 * GiB) == 6);
    REQUIRE(Shader::CompilationWorkerCount(64, 16 * GiB) == 8);
    REQUIRE(Shader::CompilationWorkerCount(64, 8 * GiB) == 1);
    REQUIRE(Shader::CompilationWorkerCount(64, 64 * GiB) == 32);
    REQUIRE(Shader::CompilationWorkerCount(2, 64 * GiB) == 1);
}

TEST_CASE("Shader compiler handles missing or very small host resource reports", "[shader]") {
    REQUIRE(Shader::CompilationWorkerCount(0, 0) == 1);
    REQUIRE(Shader::CompilationWorkerCount(12, 0) == 6);
    REQUIRE(Shader::CompilationWorkerCount(0, 16 * GiB) == 1);
    REQUIRE(Shader::CompilationWorkerCount(16, GiB / 2) == 1);
}
