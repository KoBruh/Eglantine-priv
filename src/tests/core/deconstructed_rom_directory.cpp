// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace {
class CountingRomFSDirectory final : public FileSys::VectorVfsDirectory {
public:
    CountingRomFSDirectory() : VectorVfsDirectory({}, {}, "romfs") {}

    std::vector<FileSys::VirtualFile> GetFiles() const override {
        ++file_enumerations;
        return VectorVfsDirectory::GetFiles();
    }

    mutable size_t file_enumerations{};
};
} // namespace

TEST_CASE("Loose RomFS is constructed only when needed", "[core][loader][romfs]") {
    using namespace FileSys;

    auto root = std::make_shared<VectorVfsDirectory>();
    auto romfs = std::make_shared<CountingRomFSDirectory>();
    romfs->AddFile(std::make_shared<VectorVfsFile>(std::vector<u8>{1, 2, 3}, "proof.bin"));
    root->AddDirectory(romfs);
    auto exefs = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, "exefs", root);

    SECTION("file-based loader") {
        auto main = std::make_shared<VectorVfsFile>(std::vector<u8>{}, "main", exefs);
        Loader::AppLoader_DeconstructedRomDirectory loader(main);
        u64 program_id{};
        REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
        REQUIRE(romfs->file_enumerations == 0);

        VirtualFile first;
        REQUIRE(loader.ReadRomFS(first) == Loader::ResultStatus::Success);
        REQUIRE(first);
        REQUIRE(romfs->file_enumerations == 1);
        auto extracted = ExtractRomFS(first);
        REQUIRE(extracted);
        const auto extracted_file = extracted->GetFile("proof.bin");
        REQUIRE(extracted_file);
        const std::vector<u8> expected{1, 2, 3};
        REQUIRE(extracted_file->ReadAllBytes() == expected);

        VirtualFile second;
        REQUIRE(loader.ReadRomFS(second) == Loader::ResultStatus::Success);
        REQUIRE(first == second);
        REQUIRE(romfs->file_enumerations == 1);
    }

    SECTION("directory-based loader") {
        exefs->AddFile(std::make_shared<VectorVfsFile>(std::vector<u8>{}, "main"));
        Loader::AppLoader_DeconstructedRomDirectory loader(exefs);
        REQUIRE(romfs->file_enumerations == 0);
        VirtualFile result;
        REQUIRE(loader.ReadRomFS(result) == Loader::ResultStatus::Success);
        REQUIRE(result);
        REQUIRE(romfs->file_enumerations == 1);
    }
}
