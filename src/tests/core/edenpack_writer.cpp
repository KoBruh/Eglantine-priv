// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/file_sys/edenpack_writer.h"
#include "core/file_sys/vfs/vfs_edenpack.h"
#include "core/file_sys/vfs/vfs_real.h"

namespace {
struct TempDir {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("edenpack-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
void Put(const std::filesystem::path& p, std::string_view value) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}
std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
// About 2000 small files in nested romfs folders, plus content that must survive conversion.
void MakeLargeDump(const std::filesystem::path& root) {
    Put(root / "base/exefs/main", "base");
    Put(root / "base/exefs/main.npdm", "npdm");
    Put(root / "updates/65536/exefs/main", "update");
    Put(root / "updates/65536/exefs/main.npdm", "npdm-update");
    for (int dir = 0; dir < 40; ++dir) {
        for (int file = 0; file < 50; ++file) {
            const auto name = "d" + std::to_string(dir) + "/sub/f" + std::to_string(file);
            Put(root / "updates/65536/romfs" / name,
                std::string(100 + dir * 50 + file, 'a' + dir % 26));
        }
    }
    Put(root / "dlc/x.bin", "dlc");
}
std::string Read(FileSys::VirtualFile f) {
    std::string out(f->GetSize(), '\0');
    f->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
    return out;
}
} // namespace

TEST_CASE("EdenPack selects highest update and mounts its files", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    Put(root / "base/exefs/main", "base");
    Put(root / "base/exefs/main.npdm", "npdm");
    Put(root / "base/romfs/a", "base-rom");
    Put(root / "updates/2/exefs/main", "update2");
    Put(root / "updates/2/exefs/main.npdm", "npdm2");
    Put(root / "updates/10/exefs/main", "update10");
    Put(root / "updates/10/exefs/main.npdm", "npdm10");
    Put(root / "updates/10/romfs/nested/file", "rom");
    auto archive = t.path / "Game.edenpack";
    REQUIRE(FileSys::EdenPack::PackDump(root, archive) == FileSys::EdenPack::PackResult::Success);
    auto staged = t.path / "staged";
    std::filesystem::create_directories(staged);
    std::filesystem::copy(root / "updates/10/exefs", staged / "ExeFS",
                          std::filesystem::copy_options::recursive);
    std::filesystem::copy(root / "updates/10/romfs", staged / "RomFS",
                          std::filesystem::copy_options::recursive);
    const auto verify = "python3 tools/edenpack/edenpack.py verify '" + archive.string() +
                        "' --source '" + staged.string() + "'";
    REQUIRE(std::system(verify.c_str()) == 0);
    FileSys::RealVfsFilesystem real;
    auto mounted = FileSys::EdenPackVfsFilesystem::Open(real.OpenFile(archive.string()));
    REQUIRE(mounted);
    auto exefs = mounted->GetSubdirectory("ExeFS");
    auto romfs = mounted->GetSubdirectory("RomFS");
    REQUIRE(Read(exefs->GetFile("main")) == "update10");
    REQUIRE(Read(romfs->GetSubdirectory("nested")->GetFile("file")) == "rom");
}

TEST_CASE("EdenPack low storage resumes and preserves archive data", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "exefs/main.npdm", "npdm");
    Put(root / "romfs/a", "a");
    Put(root / "romfs/sub/b", "second");
    auto archive = t.path / "Game.edenpack";
    std::atomic_bool cancel = false;
    bool cancelled = false;
    FileSys::EdenPack::PackOptions options{.delete_sources = true, .chunk_bytes = 1};
    auto result = FileSys::EdenPack::PackDump(
        root, archive, options,
        [&](const auto& p) {
            if (p.files_done >= 2 && !cancelled) {
                cancel = true;
                cancelled = true;
            }
        },
        &cancel);
    REQUIRE(result == FileSys::EdenPack::PackResult::Cancelled);
    REQUIRE(FileSys::EdenPack::HasResumableJournal(archive));
    REQUIRE_FALSE(std::filesystem::exists(root / "exefs/main"));
    REQUIRE_FALSE(std::filesystem::exists(root / "exefs/main.npdm"));
    cancel = false;
    REQUIRE(FileSys::EdenPack::PackDump(root, archive, options, {}, &cancel) ==
            FileSys::EdenPack::PackResult::Success);
    REQUIRE_FALSE(std::filesystem::exists(archive.string() + ".journal"));
    REQUIRE(std::filesystem::exists(archive));
}

TEST_CASE("EdenPack rejects overwrite and missing NPDM", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "romfs/a", "rom");
    auto archive = t.path / "out.edenpack";
    Put(archive, "existing");
    REQUIRE(FileSys::EdenPack::PackDump(root, archive) == FileSys::EdenPack::PackResult::IoError);
    std::filesystem::remove(archive);
    REQUIRE(FileSys::EdenPack::PackDump(root, archive) ==
            FileSys::EdenPack::PackResult::InvalidSource);
}

TEST_CASE("EdenPack low storage removes packed trees and keeps the rest", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    MakeLargeDump(root);
    auto copy = t.path / "copy";
    std::filesystem::copy(root, copy, std::filesystem::copy_options::recursive);
    auto archive = t.path / "Game.edenpack";
    FileSys::EdenPack::PackOptions options{.delete_sources = true, .chunk_bytes = 64 * 1024};
    REQUIRE(FileSys::EdenPack::PackDump(root, archive, options) ==
            FileSys::EdenPack::PackResult::Success);

    REQUIRE_FALSE(std::filesystem::exists(root / "updates"));
    REQUIRE(ReadAll(root / "base/exefs/main") == "base");
    REQUIRE(ReadAll(root / "dlc/x.bin") == "dlc");

    FileSys::RealVfsFilesystem real;
    auto mounted = FileSys::EdenPackVfsFilesystem::Open(real.OpenFile(archive.string()));
    REQUIRE(mounted);
    auto romfs = mounted->GetSubdirectory("RomFS");
    for (int dir = 0; dir < 40; dir += 13) {
        for (int file = 0; file < 50; file += 7) {
            const auto name = "f" + std::to_string(file);
            const auto sub =
                romfs->GetSubdirectory("d" + std::to_string(dir))->GetSubdirectory("sub");
            REQUIRE(
                Read(sub->GetFile(name)) ==
                ReadAll(copy / "updates/65536/romfs" / ("d" + std::to_string(dir)) / "sub" / name));
        }
    }
}

TEST_CASE("EdenPack resumed low storage run matches a normal pack", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    MakeLargeDump(root);
    auto copy = t.path / "copy";
    std::filesystem::copy(root, copy, std::filesystem::copy_options::recursive);

    auto reference = t.path / "reference.edenpack";
    REQUIRE(FileSys::EdenPack::PackDump(copy, reference) == FileSys::EdenPack::PackResult::Success);

    auto archive = t.path / "Game.edenpack";
    FileSys::EdenPack::PackOptions options{.delete_sources = true, .chunk_bytes = 64 * 1024};
    std::atomic_bool cancel = false;
    REQUIRE(FileSys::EdenPack::PackDump(
                root, archive, options,
                [&](const auto& p) {
                    if (p.files_done >= 900) {
                        cancel = true;
                    }
                },
                &cancel) == FileSys::EdenPack::PackResult::Cancelled);
    REQUIRE(FileSys::EdenPack::HasResumableJournal(archive));
    REQUIRE_FALSE(std::filesystem::exists(root / "updates/65536/romfs/d0/sub/f0"));

    // Tear the last journal line, as a crash during the append would.
    auto journal = archive;
    journal += ".journal";
    std::ofstream(journal, std::ios::binary | std::ios::app) << "{\"offset\":12";

    cancel = false;
    std::string error;
    REQUIRE(FileSys::EdenPack::PackDump(root, archive, options, {}, &cancel, &error) ==
            FileSys::EdenPack::PackResult::Success);
    INFO(error);
    REQUIRE(ReadAll(archive) == ReadAll(reference));
    REQUIRE_FALSE(std::filesystem::exists(journal));
    REQUIRE_FALSE(std::filesystem::exists(root / "updates"));
    REQUIRE(ReadAll(root / "dlc/x.bin") == "dlc");
}

TEST_CASE("EdenPack finds the dump root from a main executable", "[edenpack]") {
    TempDir t;
    auto root = t.path / "Game";
    MakeLargeDump(root);
    using FileSys::EdenPack::DumpRootFromMain;
    REQUIRE(DumpRootFromMain(root / "base/exefs/main") == root);
    REQUIRE(DumpRootFromMain(root / "updates/65536/exefs/main") == root);
    REQUIRE_FALSE(DumpRootFromMain(root / "dlc/x.bin"));
    REQUIRE(FileSys::EdenPack::DefaultArchivePath(root) == t.path / "Game.edenpack");
}
