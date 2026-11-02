// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/fsmitm_romfsbuild.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

struct RomFSBuildContextTestAccess {
    static std::weak_ptr<RomFSBuildDirectoryContext> Root(const RomFSBuildContext& context) {
        return context.root;
    }

    static std::weak_ptr<RomFSBuildFileContext> FirstFile(const RomFSBuildContext& context) {
        return context.files.front();
    }
};

} // namespace FileSys

TEST_CASE("RomFS construction releases its temporary tree", "[core][romfs][memory]") {
    using namespace FileSys;

    auto input = std::make_shared<VectorVfsDirectory>();
    auto subdir = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, "nested");
    input->AddFile(std::make_shared<VectorVfsFile>(std::vector<u8>{1, 2, 3}, "root.bin"));
    subdir->AddFile(std::make_shared<VectorVfsFile>(std::vector<u8>{4, 5, 6}, "child.bin"));
    input->AddDirectory(subdir);

    std::weak_ptr<RomFSBuildDirectoryContext> root;
    std::weak_ptr<RomFSBuildFileContext> file;
    VirtualFile output;
    {
        RomFSBuildContext context{input};
        root = RomFSBuildContextTestAccess::Root(context);
        file = RomFSBuildContextTestAccess::FirstFile(context);
        output = ConcatenatedVfsFile::MakeConcatenatedFile(0, "test", context.Build());
        REQUIRE(output);
        REQUIRE_FALSE(root.expired());
        REQUIRE_FALSE(file.expired());
    }

    REQUIRE(root.expired());
    REQUIRE(file.expired());
    const auto extracted = ExtractRomFS(output);
    REQUIRE(extracted);
    const auto root_file = extracted->GetFile("root.bin");
    const auto extracted_subdir = extracted->GetSubdirectory("nested");
    REQUIRE(root_file);
    REQUIRE(extracted_subdir);
    const auto child_file = extracted_subdir->GetFile("child.bin");
    REQUIRE(child_file);
    const std::vector<u8> root_data{1, 2, 3};
    const std::vector<u8> child_data{4, 5, 6};
    REQUIRE(root_file->ReadAllBytes() == root_data);
    REQUIRE(child_file->ReadAllBytes() == child_data);
}

TEST_CASE("RomFS keeps a large sibling list readable", "[core][romfs][memory]") {
    using namespace FileSys;

    constexpr size_t count = 8192;
    auto input = std::make_shared<VectorVfsDirectory>();
    for (size_t i = 0; i < count; ++i) {
        input->AddFile(std::make_shared<VectorVfsFile>(
            std::vector<u8>{static_cast<u8>(i & 0xff)}, "file-" + std::to_string(i)));
    }
    auto output = CreateRomFS(input);
    REQUIRE(output);
    auto extracted = ExtractRomFS(output);
    REQUIRE(extracted);
    REQUIRE(extracted->GetFiles().size() == count);
    for (const size_t index : {size_t{0}, count / 2, count - 1}) {
        const auto file = extracted->GetFile("file-" + std::to_string(index));
        REQUIRE(file);
        REQUIRE(file->ReadAllBytes() == std::vector<u8>{static_cast<u8>(index & 0xff)});
    }
}
