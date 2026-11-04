// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/shader_environment.h"

namespace {

namespace fs = std::filesystem;

constexpr u32 CacheVersion = 0x7e57;
constexpr std::array<char, 8> Magic{'e', 'g', 'l', 'n', 'c', 'a', 'c', 'h'};

// An empty vertex program as FileEnvironment::Deserialize reads it: its counts and fields, all
// zero, then its header.
constexpr std::size_t EmptyProgramSize = 7 * sizeof(u64) + 7 * sizeof(u32) +
                                         sizeof(Shader::Stage) + sizeof(Shader::ProgramHeader);

fs::path NewCachePath() {
    return fs::temp_directory_path() /
           ("pipeline-cache-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(std::random_device{}()) + ".bin");
}

void WriteHeader(std::ofstream& file) {
    file.write(Magic.data(), Magic.size());
    file.write(reinterpret_cast<const char*>(&CacheVersion), sizeof(CacheVersion));
}

// A graphics pipeline of one empty program, keyed by `id`.
void WritePipeline(std::ofstream& file, u64 id) {
    constexpr u32 num_envs = 1;
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    const std::vector<char> program(EmptyProgramSize);
    file.write(program.data(), static_cast<std::streamsize>(program.size()));
    file.write(reinterpret_cast<const char*>(&id), sizeof(id));
}

// A pipeline cache file with a valid header and one damaged entry: its program count, then the
// first program's code size and `zeroes` zero bytes, which read as empty programs.
fs::path WriteDamagedCache(u32 num_envs, u64 code_size, std::size_t zeroes = 256) {
    const fs::path path = NewCachePath();
    std::ofstream file(path, std::ios::binary);
    WriteHeader(file);
    file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size));
    const std::vector<char> fields(zeroes);
    file.write(fields.data(), static_cast<std::streamsize>(fields.size()));
    return path;
}

// Loads the file; true if any entry reached the callbacks.
bool Load(const fs::path& path) {
    bool loaded = false;
    std::stop_source stop;
    VideoCommon::LoadPipelines(
        stop.get_token(), path, CacheVersion,
        [&loaded](std::ifstream&, VideoCommon::FileEnvironment) { loaded = true; },
        [&loaded](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) { loaded = true; });
    return loaded;
}

// The graphics pipelines of a file written with WritePipeline: their ids, and where each ends.
struct LoadedPipelines {
    std::vector<u64> ids;
    std::vector<u64> ends;
};

LoadedPipelines LoadIds(const fs::path& path) {
    LoadedPipelines loaded;
    std::stop_source stop;
    VideoCommon::LoadPipelines(
        stop.get_token(), path, CacheVersion, [](std::ifstream&, VideoCommon::FileEnvironment) {},
        [&loaded](std::ifstream& file, std::vector<VideoCommon::FileEnvironment>) {
            u64 id{};
            file.read(reinterpret_cast<char*>(&id), sizeof(id));
            loaded.ids.push_back(id);
            loaded.ends.push_back(static_cast<u64>(static_cast<std::streamoff>(file.tellg())));
        });
    return loaded;
}

fs::path WriteCache(std::initializer_list<u64> ids) {
    const fs::path path = NewCachePath();
    std::ofstream file(path, std::ios::binary);
    WriteHeader(file);
    for (const u64 id : ids) {
        WritePipeline(file, id);
    }
    return path;
}

} // Anonymous namespace

TEST_CASE("Pipeline cache entries with an impossible program count are discarded",
          "[video_core][pipeline_cache]") {
    // Enough zeroes for seven empty programs: without the count check they all load and reach
    // load_graphics, and a count of zero reads the first of no programs.
    for (const u32 num_envs : {0U, 7U}) {
        const fs::path path = WriteDamagedCache(num_envs, 0, 64 * 1024);
        REQUIRE_FALSE(Load(path));
        REQUIRE_FALSE(fs::exists(path));
    }
}

TEST_CASE("Pipeline cache entries with an impossible code size are discarded",
          "[video_core][pipeline_cache]") {
    const fs::path path = WriteDamagedCache(1, ~u64{0} - 15);
    REQUIRE_FALSE(Load(path));
    REQUIRE_FALSE(fs::exists(path));
}

TEST_CASE("A pipeline cut short at the end of the cache is dropped, the ones before it stay",
          "[video_core][pipeline_cache]") {
    const fs::path path = WriteCache({1, 2});
    {
        // The emulator stopped while appending a third.
        std::ofstream file(path, std::ios::binary | std::ios::app);
        constexpr u32 num_envs = 1;
        file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
        const std::vector<char> partial(EmptyProgramSize / 2);
        file.write(partial.data(), static_cast<std::streamsize>(partial.size()));
    }
    const std::vector<u64> expected{1, 2};
    const LoadedPipelines loaded = LoadIds(path);
    REQUIRE(loaded.ids == expected);
    // Deleted whole before: every pipeline was built again.
    REQUIRE(fs::exists(path));
    REQUIRE(fs::file_size(path) == loaded.ends.back());
    REQUIRE(LoadIds(path).ids == expected);
    fs::remove(path);
}

TEST_CASE("Pipeline cache compaction keeps the marked pipelines and those stored since the load",
          "[video_core][pipeline_cache]") {
    const fs::path path = WriteCache({1, 2, 3});
    const LoadedPipelines loaded = LoadIds(path);
    REQUIRE(loaded.ids.size() == 3);
    {
        // A draw stores a pipeline before the compaction runs.
        std::ofstream file(path, std::ios::binary | std::ios::app);
        WritePipeline(file, 4);
    }
    const std::array entries{
        VideoCommon::PipelineCacheEntry{.end = loaded.ends[0], .keep = true},
        VideoCommon::PipelineCacheEntry{.end = loaded.ends[1], .keep = false},
        VideoCommon::PipelineCacheEntry{.end = loaded.ends[2], .keep = true},
    };
    REQUIRE(VideoCommon::CompactPipelineCache(path, entries));
    const std::vector<u64> expected{1, 3, 4};
    REQUIRE(LoadIds(path).ids == expected);
    fs::path temporary = path;
    temporary += ".tmp";
    REQUIRE_FALSE(fs::exists(temporary));
    fs::remove(path);
}

TEST_CASE("Pipeline cache compaction with a list that does not match the file leaves it alone",
          "[video_core][pipeline_cache]") {
    const fs::path path = WriteCache({1, 2});
    const LoadedPipelines loaded = LoadIds(path);
    REQUIRE(loaded.ends.size() == 2);
    const std::array entries{
        VideoCommon::PipelineCacheEntry{.end = loaded.ends[1] + 1, .keep = false},
    };
    REQUIRE_FALSE(VideoCommon::CompactPipelineCache(path, entries));
    const std::vector<u64> expected{1, 2};
    REQUIRE(LoadIds(path).ids == expected);
    fs::remove(path);
}
