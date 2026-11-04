// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/loader.h"

namespace {

constexpr std::size_t NpdmSize = 0x400;
constexpr u32 AcidOffset = 0x80;
constexpr u32 AciOffset = 0x300;
constexpr u32 KacOffset = 0x60; // Relative to the ACI0 block, so at 0x360 in the file.

void Put32(std::vector<u8>& data, std::size_t offset, u32 value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

// A Switch 1 layout NPDM: header, ACID with its file access control, ACI0 with its file access
// header, and kac_size bytes of kernel capabilities claimed at ACI0 + KacOffset.
FileSys::VirtualFile MakeNpdm(u32 kac_size) {
    std::vector<u8> data(NpdmSize);
    std::memcpy(data.data(), "META", 4);
    Put32(data, 0x70, AciOffset);
    Put32(data, 0x74, 0x100);
    Put32(data, 0x78, AcidOffset);
    Put32(data, 0x7C, 0x280);
    std::memcpy(data.data() + AcidOffset + 0x200, "ACID", 4);
    Put32(data, AcidOffset + 0x220, 0x240); // FAC, right after the ACID header
    std::memcpy(data.data() + AciOffset, "ACI0", 4);
    Put32(data, AciOffset + 0x20, 0x40); // FAH, right after the ACI0 header
    Put32(data, AciOffset + 0x30, KacOffset);
    Put32(data, AciOffset + 0x34, kac_size);
    return std::make_shared<FileSys::VectorVfsFile>(std::move(data), "main.npdm");
}

} // Anonymous namespace

TEST_CASE("NPDM kernel capabilities load whole words inside the file", "[core][npdm]") {
    FileSys::ProgramMetadata metadata;
    REQUIRE(metadata.Load(MakeNpdm(8)) == Loader::ResultStatus::Success);
    REQUIRE(metadata.GetKernelCapabilities().size() == 2);

    // Exactly up to the end of the file.
    REQUIRE(metadata.Load(MakeNpdm(NpdmSize - AciOffset - KacOffset)) ==
            Loader::ResultStatus::Success);
}

TEST_CASE("NPDM kernel capabilities of a bad size are rejected", "[core][npdm]") {
    FileSys::ProgramMetadata metadata;
    // Not whole words: the read used to write past the buffer sized in words.
    REQUIRE(metadata.Load(MakeNpdm(0x101)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
    REQUIRE(metadata.Load(MakeNpdm(3)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
    // Past the end of the file, by a word or by gigabytes (no buffer that size is allocated).
    REQUIRE(metadata.Load(MakeNpdm(NpdmSize - AciOffset - KacOffset + 4)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
    REQUIRE(metadata.Load(MakeNpdm(0xFFFFFFF0U)) ==
            Loader::ResultStatus::ErrorBadKernelCapabilityDescriptors);
}
