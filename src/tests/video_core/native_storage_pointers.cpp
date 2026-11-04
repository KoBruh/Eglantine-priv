// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/glsl/emit_glsl.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/shader_info.h"

namespace {
using namespace Shader;

using Slots = std::map<std::pair<u32, u32>, bool>;

/// A native compute program's environment: which constant buffer arguments are mapped pointers.
class PointerEnvironment final : public Environment {
public:
    explicit PointerEnvironment(Slots slots_) : slots{std::move(slots_)} {
        stage = Stage::Compute;
    }

    bool IsPointer(u32 index, u32 offset) const {
        const auto it = slots.find({index, offset});
        return it != slots.end() && it->second;
    }

    u64 ReadInstruction(u32) override {
        return 0;
    }
    bool IsNativeProgram() const override {
        return true;
    }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        return IsPointer(index, offset);
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    TextureType ReadTextureType(u32) override {
        return TextureType::Color2D;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 2;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    Slots slots;
};

/// One block of global memory accesses, addressed the way lowered native programs do it: the
/// pointer argument a predicate selects (a predicated move), plus an index.
struct NativeProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;

    NativeProgram() {
        program.stage = Stage::Compute;
        program.workgroup_size = {32, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
    }

    /// select(cbuf0 flags, cbuf1[offsets[0]], cbuf1[offsets[1]], ...) + cbuf0[0x100] * 4
    IR::U64 Address(std::span<const u32> offsets) {
        IR::IREmitter ir{block};
        IR::U32 low{ir.GetCbuf(ir.Imm32(1), ir.Imm32(offsets.back()))};
        IR::U32 high{ir.GetCbuf(ir.Imm32(1), ir.Imm32(offsets.back() + 4))};
        for (size_t index = offsets.size() - 1; index-- > 0;) {
            const IR::U32 flag{ir.GetCbuf(ir.Imm32(0), ir.Imm32(static_cast<u32>(index * 4)))};
            const IR::U1 pick{ir.INotEqual(flag, ir.Imm32(0))};
            low = IR::U32{ir.Select(pick, ir.GetCbuf(ir.Imm32(1), ir.Imm32(offsets[index])), low)};
            high = IR::U32{
                ir.Select(pick, ir.GetCbuf(ir.Imm32(1), ir.Imm32(offsets[index] + 4)), high)};
        }
        const IR::U32 element{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x100))};
        low = IR::U32{ir.IAdd(low, IR::U32{ir.ShiftLeftLogical(element, ir.Imm32(2))})};
        return ir.PackUint2x32(ir.CompositeConstruct(low, high));
    }

    void Load(std::span<const u32> offsets) {
        const IR::U64 address{Address(offsets)};
        IR::IREmitter ir{block};
        static_cast<void>(ir.LoadGlobal32(address));
    }

    void Store(std::span<const u32> offsets) {
        const IR::U64 address{Address(offsets)};
        IR::IREmitter ir{block};
        ir.WriteGlobal32(address, ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x104)));
    }

    size_t Count(IR::Opcode opcode) const {
        return static_cast<size_t>(
            std::count_if(block.begin(), block.end(),
                          [opcode](const IR::Inst& inst) { return inst.GetOpcode() == opcode; }));
    }
};

bool Fits(const Info& info, const PointerEnvironment& env) {
    return NativePointerSlotsMatch(info.native_pointer_slots, [&env](u32 index, u32 offset) {
        return env.IsPointer(index, offset);
    });
}

size_t CountInstructions(const std::vector<u32>& code, u32 opcode) {
    size_t count{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        if ((code[offset] & 0xffffU) == opcode) {
            ++count;
        }
        offset += size;
    }
    return count;
}

constexpr std::array<u32, 2> SelectedPointers{0x10, 0x18};

/// A store through either of two mapped pointers, with the passes the backends need run.
void BuildSelectedStore(NativeProgram& fixture) {
    fixture.Store(SelectedPointers);
    PointerEnvironment env{Slots{{{1, 0x10}, true}, {{1, 0x18}, true}}};
    Optimization::GlobalMemoryToStorageBufferPass(env, fixture.program, HostTranslateInfo{});
    Optimization::CollectShaderInfoPass(env, fixture.program);
}

Profile BackendProfile() {
    Profile profile;
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.min_ssbo_alignment = 16;
    return profile;
}

} // Anonymous namespace

TEST_CASE("Native storage pointers: a program only fits the pointer answers it was built for",
          "[video_core][native_storage_pointers]") {
    // The first dispatch: the second argument is still null.
    PointerEnvironment second_null{Slots{{{1, 0x10}, true}, {{1, 0x18}, false}}};
    NativeProgram first;
    first.Load(SelectedPointers);
    Optimization::GlobalMemoryToStorageBufferPass(second_null, first.program, HostTranslateInfo{});
    const Info& first_info{first.program.info};
    // Only the mapped argument is a candidate, so the access is rebased on it.
    REQUIRE(first.Count(IR::Opcode::LoadStorage32) == 1);
    REQUIRE(first_info.storage_buffers_descriptors.size() == 1);
    REQUIRE(first_info.storage_buffers_descriptors[0].cbuf_offset == 0x10);
    const std::vector<NativePointerSlot> asked{{1, 0x10, true}, {1, 0x18, false}};
    REQUIRE(first_info.native_pointer_slots == asked);
    REQUIRE(Fits(first_info, second_null));

    // A later dispatch selects the second argument, mapped now. The rebased access would read
    // the first buffer at the distance between the two: the program must not be used for it.
    PointerEnvironment both_mapped{Slots{{{1, 0x10}, true}, {{1, 0x18}, true}}};
    REQUIRE_FALSE(Fits(first_info, both_mapped));

    // The variant built for those answers binds both and picks one at run time.
    NativeProgram second;
    second.Load(SelectedPointers);
    Optimization::GlobalMemoryToStorageBufferPass(both_mapped, second.program,
                                                  HostTranslateInfo{});
    const Info& second_info{second.program.info};
    REQUIRE(second.Count(IR::Opcode::LoadGlobal32) == 1);
    REQUIRE(second_info.native_global_bounds);
    REQUIRE(second_info.storage_buffers_descriptors.size() == 2);
    REQUIRE(Fits(second_info, both_mapped));
    REQUIRE_FALSE(Fits(second_info, second_null));
}

TEST_CASE("Native storage pointers: a global store may write any buffer it can select",
          "[video_core][native_storage_pointers]") {
    NativeProgram fixture;
    // A read through one pointer (rebased, read only by itself), a store through either of two.
    constexpr std::array<u32, 1> read_pointer{0x20};
    fixture.Load(read_pointer);
    fixture.Store(SelectedPointers);
    PointerEnvironment env{Slots{{{1, 0x10}, true}, {{1, 0x18}, true}, {{1, 0x20}, true}}};
    Optimization::GlobalMemoryToStorageBufferPass(env, fixture.program, HostTranslateInfo{});
    const Info& info{fixture.program.info};
    REQUIRE(fixture.Count(IR::Opcode::LoadStorage32) == 1);
    REQUIRE(fixture.Count(IR::Opcode::WriteGlobal32) == 1);
    REQUIRE(info.native_global_bounds);
    REQUIRE(info.storage_buffers_descriptors.size() == 3);
    // The store picks its buffer among all three at run time; each must be bound as written.
    const auto written{[](const StorageBufferDescriptor& desc) { return desc.is_written; }};
    REQUIRE(std::ranges::all_of(info.storage_buffers_descriptors, written));
}

TEST_CASE("Native storage pointers: more candidates than bindings fail the translation",
          "[video_core][native_storage_pointers]") {
    std::vector<u32> offsets;
    Slots mapped;
    for (u32 index = 0; index < Info::MAX_SSBOS + 8; ++index) {
        offsets.push_back(0x10 + index * 8);
        mapped.emplace(std::pair{1U, 0x10 + index * 8}, true);
    }
    NativeProgram fixture;
    fixture.Load(offsets);
    PointerEnvironment env{std::move(mapped)};
    // A shader exception fails the pipeline; anything else ends the GPU thread.
    REQUIRE_THROWS_AS(
        Optimization::GlobalMemoryToStorageBufferPass(env, fixture.program, HostTranslateInfo{}),
        Shader::NotImplementedException);
}

TEST_CASE("Native storage pointers: the SPIR-V selection stays inside the binding",
          "[video_core][native_storage_pointers]") {
    NativeProgram fixture;
    BuildSelectedStore(fixture);
    const size_t num_buffers{fixture.program.info.storage_buffers_descriptors.size()};
    REQUIRE(fixture.program.info.native_global_bounds);
    REQUIRE(num_buffers == 2);
    const std::vector<u32> code{Backend::SPIRV::EmitSPIRV(BackendProfile(), fixture.program)};
    // OpArrayLength: each buffer's length bounds it in the 32, 64 and 128-bit load and store
    // functions.
    REQUIRE(CountInstructions(code, 68) == 6 * num_buffers);
}

TEST_CASE("Native storage pointers: the GLSL selection stays inside the binding",
          "[video_core][native_storage_pointers]") {
    NativeProgram fixture;
    BuildSelectedStore(fixture);
    const std::string glsl{Backend::GLSL::EmitGLSL(BackendProfile(), fixture.program)};
    for (size_t index = 0; index < 2; ++index) {
        REQUIRE(glsl.find("_ssbo" + std::to_string(index) + ".length()") != std::string::npos);
    }
}
