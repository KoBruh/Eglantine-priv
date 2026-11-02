// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "core/file_sys/vfs/vfs_real.h"

namespace FileSys {

struct RealVfsCacheTestAccess {
    static VirtualFile OpenKnownSize(RealVfsFilesystem& fs, const std::string& path) {
        return fs.OpenFileFromEntry(path, u64{0}, std::string{"vfs-cache-test"}, OpenMode::Read);
    }

    static size_t CacheSize(RealVfsFilesystem& fs) {
        std::scoped_lock lock{fs.list_lock};
        return fs.cache.size();
    }

    static void Invalidate(RealVfsFilesystem& fs) {
        std::scoped_lock lock{fs.list_lock};
        fs.cache.clear();
    }
};

} // namespace FileSys

TEST_CASE("Real VFS releases expired cached file references", "[core][vfs][memory]") {
    FileSys::RealVfsFilesystem fs;
    for (int i = 0; i < 8192; ++i) {
        const auto path = "vfs-cache-test/file-" + std::to_string(i);
        auto file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(fs, path);
        REQUIRE(file);
        REQUIRE(FileSys::RealVfsCacheTestAccess::CacheSize(fs) == 1);
        file.reset();
        REQUIRE(FileSys::RealVfsCacheTestAccess::CacheSize(fs) == 0);
    }
}

TEST_CASE("An old VFS file cannot evict a newer reference for the same path", "[core][vfs][memory]") {
    FileSys::RealVfsFilesystem fs;
    const std::string path{"vfs-cache-test/shared-file"};
    auto old_file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(fs, path);
    REQUIRE(old_file);
    FileSys::RealVfsCacheTestAccess::Invalidate(fs);
    auto new_file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(fs, path);
    REQUIRE(new_file);
    REQUIRE(new_file != old_file);
    old_file.reset();
    REQUIRE(FileSys::RealVfsCacheTestAccess::CacheSize(fs) == 1);
    auto reused_file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(fs, path);
    REQUIRE(reused_file == new_file);
    reused_file.reset();
    new_file.reset();
    REQUIRE(FileSys::RealVfsCacheTestAccess::CacheSize(fs) == 0);
}

TEST_CASE("Concurrent VFS opens and releases leave no expired references", "[core][vfs][memory]") {
    FileSys::RealVfsFilesystem fs;
    constexpr size_t workers = 8;
    std::barrier start{workers};
    std::atomic<bool> failed{};
    std::array<std::thread, workers> threads;
    for (auto& thread : threads) {
        thread = std::thread([&] {
            start.arrive_and_wait();
            for (size_t i = 0; i < 1024; ++i) {
                auto file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(
                    fs, "vfs-cache-test/concurrent-file");
                if (!file) {
                    failed.store(true, std::memory_order_relaxed);
                }
                if (i % 16 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(FileSys::RealVfsCacheTestAccess::CacheSize(fs) == 0);
}

TEST_CASE("Real VFS file names match the last nonempty path component", "[core][vfs]") {
    FileSys::RealVfsFilesystem fs;
    for (const std::string path : {"file.bin", "folder/file.bin", "folder\\file.bin",
                                   "folder//child///file.bin", "folder/file.bin/",
                                   "/", "C:\\", "folder/with spaces/\xc3\xa1.bin"}) {
        const auto sanitized = Common::FS::SanitizePath(
            path, Common::FS::DirectorySeparator::PlatformDefault);
        const auto components = Common::FS::SplitPathComponentsCopy(sanitized);
        const auto file = FileSys::RealVfsCacheTestAccess::OpenKnownSize(fs, path);
        REQUIRE(file);
        REQUIRE(file->GetName() == (components.empty() ? "" : components.back()));
    }
}

#ifdef _WIN32
TEST_CASE("Real VFS enumerates Windows directory entries", "[core][vfs]") {
    namespace fs = std::filesystem;
    struct Fixture {
        fs::path dir = fs::temp_directory_path() /
                       ("eglantine-vfs-enumeration-" + std::to_string(GetCurrentProcessId()));
        ~Fixture() {
            std::error_code ec;
            fs::remove(dir / L"file-link", ec);
            fs::remove(dir / L"dir-link", ec);
            fs::remove(dir / L"zero.bin", ec);
            fs::remove(dir / L"\u00e1.bin", ec);
            fs::remove(dir / L"nested", ec);
            fs::remove(dir, ec);
        }
    } fixture;
    REQUIRE(fs::create_directory(fixture.dir));
    REQUIRE(fs::create_directory(fixture.dir / L"nested"));
    std::ofstream{fixture.dir / L"zero.bin", std::ios::binary};
    {
        std::ofstream file{fixture.dir / L"\u00e1.bin", std::ios::binary};
        file << "abc";
        REQUIRE(file.good());
    }

    std::error_code ec;
    fs::create_symlink(fixture.dir / L"\u00e1.bin", fixture.dir / L"file-link", ec);
    const bool file_link_created = !ec;
    ec.clear();
    fs::create_directory_symlink(fixture.dir / L"nested", fixture.dir / L"dir-link", ec);
    const bool dir_link_created = !ec;

    FileSys::RealVfsFilesystem real_fs;
    auto dir = real_fs.OpenDirectory(Common::FS::PathToUTF8String(fixture.dir));
    REQUIRE(dir);
    const auto files = dir->GetFiles();
    const auto directories = dir->GetSubdirectories();
    const std::vector<u8> expected{'a', 'b', 'c'};
    REQUIRE(files.size() == 2 + static_cast<size_t>(file_link_created));
    REQUIRE(directories.size() == 1 + static_cast<size_t>(dir_link_created));
    for (const auto& file : files) {
        REQUIRE(file);
        if (file->GetName() == "zero.bin") {
            REQUIRE(file->GetSize() == 0);
        } else {
            REQUIRE((file->GetName() == "\xc3\xa1.bin" || file->GetName() == "file-link"));
            REQUIRE(file->GetSize() == 3);
            REQUIRE(file->ReadAllBytes() == expected);
        }
    }
    for (const auto& child : directories) {
        REQUIRE((child->GetName() == "nested" || child->GetName() == "dir-link"));
    }
}

TEST_CASE("Benchmark Real VFS metadata footprint", "[!benchmark][vfs][memory]") {
    const auto private_bytes = [] {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                  sizeof(counters))) {
            return size_t{};
        }
        return static_cast<size_t>(counters.PrivateUsage);
    };

    FileSys::RealVfsFilesystem fs;
    constexpr size_t count = 200000;
    std::vector<FileSys::VirtualFile> files;
    files.reserve(count);
    const size_t before = private_bytes();
    for (size_t i = 0; i < count; ++i) {
        files.push_back(FileSys::RealVfsCacheTestAccess::OpenKnownSize(
            fs, "C:/virtual_romfs/content/levels/course/common/objects/textures/texture-" +
                    std::to_string(i) + ".bin"));
    }
    const size_t after = private_bytes();
    REQUIRE(before != 0);
    REQUIRE(after >= before);
    REQUIRE(files.size() == count);
    std::printf("VFS metadata: %zu files, %zu bytes private increase\n", count, after - before);
}
#endif
