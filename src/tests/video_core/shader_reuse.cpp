// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_shader_reuse.h"

namespace {

// An environment that only has the fixed state translation reads besides its requests.
class FixedEnvironment final : public Shader::Environment {
public:
    FixedEnvironment(Shader::Stage stage_, u32 texture_bound_, u32 surface_bound_,
                     std::array<u32, 3> mesh_registers_, u8 header_byte)
        : texture_bound{texture_bound_}, surface_bound{surface_bound_} {
        stage = stage_;
        mesh_registers = mesh_registers_;
        reinterpret_cast<u8*>(&mesh_primitive_header)[4] = header_byte;
    }

    u64 ReadInstruction(u32) override {
        return 0;
    }
    bool IsNativeProgram() const override {
        return native;
    }
    bool IsCbufStorageBufferPointer(u32, u32) override {
        return storage_pointers;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 0;
    }
    u32 TextureBoundBuffer() const override {
        return texture_bound;
    }
    u32 SurfaceBoundBuffer() const override {
        return surface_bound;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

    bool native = false;
    bool storage_pointers = true;

private:
    u32 texture_bound;
    u32 surface_bound;
};

constexpr std::array<u32, 3> MeshRegisters{0x11, 0x22, 0x33};

} // Anonymous namespace

TEST_CASE("Shader reuse recording forwards the mesh state and the surface buffer",
          "[video_core][shader_reuse]") {
    FixedEnvironment inner{Shader::Stage::Mesh, 2, 5, MeshRegisters, 0x5a};
    Vulkan::RecordingEnvironment recorder{inner};

    REQUIRE(recorder.ShaderStage() == Shader::Stage::Mesh);
    REQUIRE(recorder.TextureBoundBuffer() == 2);
    REQUIRE(recorder.SurfaceBoundBuffer() == 5);
    REQUIRE(recorder.MeshRegisters() == MeshRegisters);
    REQUIRE(std::memcmp(&recorder.MeshPrimitiveHeader(), &inner.MeshPrimitiveHeader(),
                        sizeof(Shader::ProgramHeader)) == 0);
}

TEST_CASE("Shader reuse needs the same surface buffer and mesh state",
          "[video_core][shader_reuse]") {
    FixedEnvironment inner{Shader::Stage::Mesh, 2, 5, MeshRegisters, 0x5a};
    Vulkan::RecordingEnvironment recorder{inner};
    const Vulkan::TranslationReads reads{recorder.TakeReads()};

    FixedEnvironment same{Shader::Stage::Mesh, 2, 5, MeshRegisters, 0x5a};
    REQUIRE(reads.Matches(same));

    FixedEnvironment other_surface{Shader::Stage::Mesh, 2, 6, MeshRegisters, 0x5a};
    REQUIRE_FALSE(reads.Matches(other_surface));

    FixedEnvironment other_registers{Shader::Stage::Mesh, 2, 5, {0x11, 0x22, 0x34}, 0x5a};
    REQUIRE_FALSE(reads.Matches(other_registers));

    FixedEnvironment other_header{Shader::Stage::Mesh, 2, 5, MeshRegisters, 0x5b};
    REQUIRE_FALSE(reads.Matches(other_header));
}

TEST_CASE("Shader reuse records the storage pointer answers of native programs",
          "[video_core][shader_reuse]") {
    FixedEnvironment inner{Shader::Stage::Fragment, 2, 2, {}, 0};
    inner.native = true;
    inner.storage_pointers = false;
    Vulkan::RecordingEnvironment recorder{inner};
    REQUIRE(recorder.IsNativeProgram());
    REQUIRE_FALSE(recorder.IsCbufStorageBufferPointer(1, 0x10));
    const Vulkan::TranslationReads reads{recorder.TakeReads()};

    FixedEnvironment same{Shader::Stage::Fragment, 2, 2, {}, 0};
    same.native = true;
    same.storage_pointers = false;
    REQUIRE(reads.Matches(same));

    // The argument now points at mapped memory: the translation would bind it as a candidate.
    FixedEnvironment mapped{Shader::Stage::Fragment, 2, 2, {}, 0};
    mapped.native = true;
    REQUIRE_FALSE(reads.Matches(mapped));

    FixedEnvironment not_native{Shader::Stage::Fragment, 2, 2, {}, 0};
    not_native.storage_pointers = false;
    REQUIRE_FALSE(reads.Matches(not_native));
}
