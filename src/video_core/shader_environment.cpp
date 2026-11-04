// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <cstring>
#include <string>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "common/assert.h"
#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include <ranges>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/pipeline_stats.h"
#include "video_core/shader_environment.h"
#include "video_core/texture_cache/format_lookup_table.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

// Not yuzu's and Eden's "yuzucach": their files use another format, and a shared cache or one
// copied in from them must not parse as ours when the version numbers happen to match.
constexpr std::array<char, 8> MAGIC_NUMBER{'e', 'g', 'l', 'n', 'c', 'a', 'c', 'h'};
/// Magic number and cache version; the first pipeline starts after it.
constexpr std::streamoff PIPELINE_CACHE_HEADER_SIZE = MAGIC_NUMBER.size() + sizeof(u32);

constexpr size_t INST_SIZE = sizeof(u64);

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

static u64 MakeCbufKey(u32 index, u32 offset) {
    return (static_cast<u64>(index) << 32) | offset;
}

/// A hash of IsCbufStorageBufferPointer answers, in key order (the map has none), 0 for none.
static u64 HashStoragePointerTypes(const ::Common::unordered_map<u64, u8>& types) {
    if (types.empty()) {
        return 0;
    }
    std::vector<std::array<u64, 2>> entries;
    entries.reserve(types.size());
    for (const auto& [key, type] : types) {
        entries.push_back({key, type});
    }
    std::ranges::sort(entries);
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(entries.data()),
                                        entries.size() * sizeof(entries[0]));
    return hash != 0 ? hash : 1;
}

bool IsStorageBufferPointer(const Tegra::MemoryManager& gpu_memory, u64 value) {
    return value != 0 && gpu_memory.IsWithinGPUAddressRange(value) &&
           gpu_memory.IsFullyMappedRange(value, 1);
}

bool IsCbufArgumentStorageBufferPointer(const Tegra::MemoryManager& gpu_memory,
                                        GPUVAddr cbuf_address, u32 cbuf_size, u32 offset) {
    // GraphicsEnvironment::ReadCbufValue and ComputeEnvironment::ReadCbufValue, word by word.
    const auto word = [&](u32 word_offset) -> u64 {
        return word_offset < cbuf_size ? gpu_memory.Read<u32>(cbuf_address + word_offset) : 0;
    };
    return IsStorageBufferPointer(gpu_memory, word(offset) | (word(offset + 4) << 32));
}

static Shader::TextureType ConvertTextureType(const Tegra::Texture::TICEntry& entry) {
    switch (entry.texture_type) {
    case Tegra::Texture::TextureType::Texture1D:
        return Shader::TextureType::Color1D;
    case Tegra::Texture::TextureType::Texture2D:
    case Tegra::Texture::TextureType::Texture2DNoMipmap:
        return entry.normalized_coords ? Shader::TextureType::Color2D
                                       : Shader::TextureType::Color2DRect;
    case Tegra::Texture::TextureType::Texture3D:
        return Shader::TextureType::Color3D;
    case Tegra::Texture::TextureType::TextureCubemap:
        return Shader::TextureType::ColorCube;
    case Tegra::Texture::TextureType::Texture1DArray:
        return Shader::TextureType::ColorArray1D;
    case Tegra::Texture::TextureType::Texture2DArray:
        return Shader::TextureType::ColorArray2D;
    case Tegra::Texture::TextureType::Texture1DBuffer:
        return Shader::TextureType::Buffer;
    case Tegra::Texture::TextureType::TextureCubeArray:
        return Shader::TextureType::ColorArrayCube;
    default:
        LOG_ERROR(Shader, "Invalid texture_type={}. Falling back to texture_type={}", static_cast<int>(entry.texture_type.Value()), Shader::TextureType::Color2D);
        return Shader::TextureType::Color2D;
    }
}

// Shader::TexturePixelFormat mirrors VideoCore::Surface::PixelFormat value for value.
static_assert(static_cast<u32>(Shader::TexturePixelFormat::E5B9G9R9_FLOAT) ==
              static_cast<u32>(VideoCore::Surface::PixelFormat::E5B9G9R9_FLOAT));
static_assert(static_cast<u32>(Shader::TexturePixelFormat::D32_FLOAT) ==
              static_cast<u32>(VideoCore::Surface::PixelFormat::D32_FLOAT));
static_assert(static_cast<size_t>(Shader::TexturePixelFormat::D32_FLOAT_S8_UINT) + 1 ==
              VideoCore::Surface::MaxPixelFormat);

static Shader::TexturePixelFormat ConvertTexturePixelFormat(const Tegra::Texture::TICEntry& entry) {
    return static_cast<Shader::TexturePixelFormat>(
        PixelFormatFromTextureInfo(entry.format, entry.r_type, entry.g_type, entry.b_type,
                                   entry.a_type, entry.srgb_conversion));
}

static u32 PackSamplerState(const Tegra::Texture::TSCEntry& entry) {
    return Shader::TextureSamplerState{
        .known = true,
        .compare_enabled = entry.depth_compare_enabled != 0,
        .compare_func = static_cast<u32>(entry.depth_compare_func.Value()),
        .linear_filter = entry.mag_filter == Tegra::Texture::TextureFilter::Linear,
    }
        .Pack();
}

static std::string_view StageToPrefix(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB:
        return "vs";
    case Shader::Stage::TessellationControl:
        return "tc";
    case Shader::Stage::TessellationEval:
        return "te";
    case Shader::Stage::Geometry:
        return "gs";
    case Shader::Stage::Fragment:
        return "fs";
    case Shader::Stage::Compute:
        return "cs";
    case Shader::Stage::VertexA:
        return "va";
    default:
        return "uk";
    }
}

static void DumpImpl(u64 /*pipeline_hash*/, u64 shader_hash, std::span<const u64> code,
                     [[maybe_unused]] u32 read_highest, [[maybe_unused]] u32 read_lowest,
                     u32 initial_offset, Shader::Stage stage) {
    const auto dump_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir)};
    if (!Common::FS::CreateDir(dump_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create dump directory");
        return;
    }
    const auto prefix = StageToPrefix(stage);
    const auto name{dump_dir /
                    fmt::format("{:016x}_{:016x}_{}.ash",
                                Settings::GetCurrentProgramID(), shader_hash, prefix)};
    std::fstream shader_file(name, std::ios::out | std::ios::binary);
    ASSERT(initial_offset % sizeof(u64) == 0);
    const size_t jump_index = initial_offset / sizeof(u64);
    const size_t code_size = code.size_bytes() - initial_offset;
    shader_file.write(reinterpret_cast<const char*>(&code[jump_index]), code_size);

    // + 1 instruction, due to the fact that we skip the final self branch instruction in the code,
    // but we need to consider it for padding, otherwise nvdisasm rages.
    const size_t padding_needed = (32 - ((code_size + INST_SIZE) % 32)) % 32;
    for (size_t i = 0; i < INST_SIZE + padding_needed; i++) {
        shader_file.put(0);
    }
}

GenericEnvironment::GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_, bool support_cooperative_matrix_)
    : gpu_memory{&gpu_memory_}, program_base{program_base_},
      support_cooperative_matrix{support_cooperative_matrix_} {
    start_address = start_address_;
}

GenericEnvironment::~GenericEnvironment() = default;

u32 GenericEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 GenericEnvironment::SurfaceBoundBuffer() const {
    return surface_bound != ~0U ? surface_bound : texture_bound;
}

u32 GenericEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 GenericEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> GenericEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

u64 GenericEnvironment::ReadInstruction(u32 address) {
    read_lowest = (std::min)(read_lowest, address);
    read_highest = (std::max)(read_highest, address);

    if (address >= cached_lowest && address < cached_highest) {
        return code[(address - cached_lowest) / INST_SIZE];
    }
    if (native_source_size) {
        throw Shader::RuntimeError("Lowered native instruction address out of range: {:#x}", address);
    }
    has_unbound_instructions = true;
    return gpu_memory->Read<u64>(program_base + address);
}

u32 GenericEnvironment::TranslateProgramAddress(u32 address) const {
    if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) return *translated;
    if (native_source_size || Shader::Ampere::HasNativeAddressMap(code))
        throw Shader::RuntimeError("Invalid native indirect branch address {:#x}", address);
    return address;
}

bool GenericEnvironment::IsNativeProgram() const {
    return native_source_size != 0 || Shader::Ampere::HasNativeAddressMap(code);
}

bool GenericEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const u64 key = MakeCbufKey(index, offset);
    if (const auto it = storage_pointer_types.find(key); it != storage_pointer_types.end()) {
        return it->second != 0;
    }
    const u64 address = u64{ReadCbufValue(index, offset)} |
                        (u64{ReadCbufValue(index, offset + 4)} << 32);
    const bool mapped = IsStorageBufferPointer(*gpu_memory, address);
    storage_pointer_types.emplace(key, static_cast<u8>(mapped));
    return mapped;
}

u64 GenericEnvironment::StoragePointerTypesHash() const {
    return HashStoragePointerTypes(storage_pointer_types);
}

std::optional<u64> GenericEnvironment::Analyze() {
    if (native_source_size) {
        // Include the converted header and a version tag to isolate this experimental ABI.
        return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                                  code.size() * sizeof(u64)) ^ 0x4f554e4345000002ULL;
    }
    const std::optional<u64> size{TryFindSize()};
    if (!size) {
        return std::nullopt;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(*size);
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), *size);
}

bool GenericEnvironment::RequiresRasterizerClip() const {
    return Shader::Ampere::HasNativeAddressMap(code) && sph.common0.reserved1 == 3;
}

void GenericEnvironment::SetCachedSize(size_t size_bytes) {
    if (native_source_size) {
        if (size_bytes != CachedSizeBytes()) {
            throw Shader::RuntimeError("Native shader changed while constructing pipeline");
        }
        return;
    }
    cached_lowest = start_address;
    cached_highest = start_address + static_cast<u32>(size_bytes);
    code.resize(CachedSizeWords());
    gpu_memory->ReadBlock(program_base + cached_lowest, code.data(), code.size() * sizeof(u64));
}

size_t GenericEnvironment::CachedSizeWords() const noexcept {
    return CachedSizeBytes() / INST_SIZE;
}

size_t GenericEnvironment::CachedSizeBytes() const noexcept {
    return static_cast<size_t>(cached_highest) - cached_lowest + INST_SIZE;
}

size_t GenericEnvironment::ReadSizeBytes() const noexcept {
    return read_highest - read_lowest + INST_SIZE;
}

bool GenericEnvironment::CanBeSerialized() const noexcept {
    return !has_unbound_instructions;
}

u64 GenericEnvironment::CalculateHash() const {
    const size_t size{ReadSizeBytes()};
    const auto data{std::make_unique<char[]>(size)};
    gpu_memory->ReadBlock(program_base + read_lowest, data.get(), size);
    return Common::CityHash64(data.get(), size);
}

void GenericEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

void GenericEnvironment::Serialize(std::ofstream& file) const {
    const u64 code_size{static_cast<u64>(CachedSizeBytes())};
    const u64 num_texture_types{static_cast<u64>(texture_types.size())};
    const u64 num_texture_pixel_formats{static_cast<u64>(texture_pixel_formats.size())};
    const u64 num_texture_sampler_states{static_cast<u64>(texture_sampler_states.size())};
    const u64 num_cbuf_values{static_cast<u64>(cbuf_values.size())};
    const u64 num_storage_pointer_types{static_cast<u64>(storage_pointer_types.size())};
    const u64 num_cbuf_replacement_values{static_cast<u64>(cbuf_replacements.size())};

    file.write(reinterpret_cast<const char*>(&code_size), sizeof(code_size))
        .write(reinterpret_cast<const char*>(&num_texture_types), sizeof(num_texture_types))
        .write(reinterpret_cast<const char*>(&num_texture_pixel_formats),
               sizeof(num_texture_pixel_formats))
        .write(reinterpret_cast<const char*>(&num_texture_sampler_states),
               sizeof(num_texture_sampler_states))
        .write(reinterpret_cast<const char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .write(reinterpret_cast<const char*>(&num_storage_pointer_types),
               sizeof(num_storage_pointer_types))
        .write(reinterpret_cast<const char*>(&num_cbuf_replacement_values),
               sizeof(num_cbuf_replacement_values))
        .write(reinterpret_cast<const char*>(&local_memory_size), sizeof(local_memory_size))
        .write(reinterpret_cast<const char*>(&texture_bound), sizeof(texture_bound))
        .write(reinterpret_cast<const char*>(&surface_bound), sizeof(surface_bound))
        .write(reinterpret_cast<const char*>(&start_address), sizeof(start_address))
        .write(reinterpret_cast<const char*>(&cached_lowest), sizeof(cached_lowest))
        .write(reinterpret_cast<const char*>(&cached_highest), sizeof(cached_highest))
        .write(reinterpret_cast<const char*>(&viewport_transform_state),
               sizeof(viewport_transform_state))
        .write(reinterpret_cast<const char*>(&stage), sizeof(stage))
        .write(reinterpret_cast<const char*>(code.data()), code_size);
    for (const auto& [key, type] : texture_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, format] : texture_pixel_formats) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&format), sizeof(format));
    }
    for (const auto& [key, state] : texture_sampler_states) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&state), sizeof(state));
    }
    for (const auto& [key, type] : cbuf_values) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : storage_pointer_types) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    for (const auto& [key, type] : cbuf_replacements) {
        file.write(reinterpret_cast<const char*>(&key), sizeof(key))
            .write(reinterpret_cast<const char*>(&type), sizeof(type));
    }
    if (stage == Shader::Stage::Compute) {
        file.write(reinterpret_cast<const char*>(&workgroup_size), sizeof(workgroup_size))
            .write(reinterpret_cast<const char*>(&shared_memory_size), sizeof(shared_memory_size));
    } else {
        file.write(reinterpret_cast<const char*>(&sph), sizeof(sph));
        if (stage == Shader::Stage::Geometry) {
            file.write(reinterpret_cast<const char*>(&gp_passthrough_mask),
                       sizeof(gp_passthrough_mask));
        }
    }
}

std::optional<u64> GenericEnvironment::TryFindSize() {
    static constexpr size_t BLOCK_SIZE = 0x1000;
    static constexpr size_t MAXIMUM_SIZE = 0x100000;

    static constexpr u64 SELF_BRANCH_A = 0xE2400FFFFF87000FULL;
    static constexpr u64 SELF_BRANCH_B = 0xE2400FFFFF07000FULL;

    static constexpr u64 EXIT_VALUE = 0xE30000000007000FULL;

    code.resize(MAXIMUM_SIZE / INST_SIZE);

    GPUVAddr guest_addr{program_base + start_address};
    size_t offset{0};
    size_t size{BLOCK_SIZE};
    while (size <= MAXIMUM_SIZE) {
        u64* const data = code.data() + offset / INST_SIZE;
        gpu_memory->ReadBlock(guest_addr, data, BLOCK_SIZE);
        for (size_t index = 0; index < BLOCK_SIZE; index += INST_SIZE) {
            const u64 inst = data[index / INST_SIZE];
            if (inst == SELF_BRANCH_A || inst == SELF_BRANCH_B) {
                return offset + index;
            }
            if (!is_proprietary_driver && inst == EXIT_VALUE) {
                return offset + index + INST_SIZE;
            }
        }
        guest_addr += BLOCK_SIZE;
        size += BLOCK_SIZE;
        offset += BLOCK_SIZE;
    }
    return std::nullopt;
}

Tegra::Texture::TICEntry GenericEnvironment::ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                                             bool via_header_index, u32 raw) {
    const auto handle{Tegra::Texture::TexturePair(raw, via_header_index)};
    const u32 index{Tegra::Texture::TextureHeaderIndex(handle.first, tic_limit,
                                                       gpu_memory->UsesNativeTiling())};
    ASSERT(index <= tic_limit);
    const GPUVAddr descriptor_addr{tic_addr + index * sizeof(Tegra::Texture::TICEntry)};
    Tegra::Texture::TICEntry entry;
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

std::optional<Tegra::Texture::TSCEntry> GenericEnvironment::ReadSamplerInfo(GPUVAddr tsc_addr,
                                                                             u32 tsc_limit,
                                                                             bool via_header_index,
                                                                             u32 raw) {
    const u32 index{Tegra::Texture::TexturePair(raw, via_header_index).second};
    if (index > tsc_limit) {
        return std::nullopt;
    }
    const GPUVAddr descriptor_addr{tsc_addr + u64{index} * sizeof(Tegra::Texture::TSCEntry)};
    if (!gpu_memory->IsFullyMappedRange(descriptor_addr, sizeof(Tegra::Texture::TSCEntry))) {
        return std::nullopt;
    }
    Tegra::Texture::TSCEntry entry;
    gpu_memory->ReadBlock(descriptor_addr, &entry, sizeof(entry));
    return entry;
}

namespace {

// Reads a native program a 4 KiB page at a time instead of one 16-byte instruction at a time.
// Each page is read with ReadBlock, so GPU-written code is flushed first, once per page instead
// of once per instruction. An instruction that is not inside a fully mapped page is left to the
// caller.
class NativeInstructionReader {
public:
    using Instruction = Shader::Ampere::Instruction;
    static constexpr GPUVAddr PageSize = 0x1000;

    explicit NativeInstructionReader(Tegra::MemoryManager& gpu_memory_)
        : gpu_memory{gpu_memory_} {}

    /// Copies the instruction at `address` into `out`, loading its page first if needed.
    /// Returns false when the instruction is not inside a fully mapped page.
    bool TryRead(GPUVAddr address, Instruction& out) {
        if (!Holds(address)) {
            LoadFrom(address);
        }
        if (!Holds(address)) {
            return false;
        }
        out = page[(address - page_begin) / sizeof(Instruction)];
        return true;
    }

private:
    bool Holds(GPUVAddr address) const {
        return address >= page_begin && address + sizeof(Instruction) <= page_end;
    }

    // Loads from `address` up to the next page boundary; loads nothing if that range is not
    // fully mapped.
    void LoadFrom(GPUVAddr address) {
        const GPUVAddr next_page = (address | (PageSize - 1)) + 1;
        const GPUVAddr length = next_page - address;
        page_begin = address;
        page_end = address;
        if (length >= sizeof(Instruction) && gpu_memory.IsFullyMappedRange(address, length)) {
            gpu_memory.ReadBlock(address, page.data(), length);
            page_end = next_page;
        }
    }

    Tegra::MemoryManager& gpu_memory;
    std::array<Instruction, PageSize / sizeof(Instruction)> page;
    GPUVAddr page_begin{};
    GPUVAddr page_end{};
};

} // Anonymous namespace

// A native program is captured and lowered once, when the shader cache first meets it. The
// environments built later for its pipelines receive that result instead of lowering again.
void GenericEnvironment::InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                                              u32 source_header_size, bool is_compute) {
    if (!lowered) {
        const Vulkan::PipelineStats::ScopedTimer timer{
            is_compute ? Vulkan::PipelineStats::Kind::Compute
                       : Vulkan::PipelineStats::Kind::Graphics,
            Vulkan::PipelineStats::Stage::Lower};
        lowered = LowerNativeCode(source_header_size);
    }
    if (lowered->texture_bound != ~0U) {
        // Native TEX carries its cbuf index explicitly; TEXS obtains it from the environment.
        texture_bound = lowered->texture_bound;
    }
    if (lowered->surface_bound != ~0U) {
        surface_bound = lowered->surface_bound;
    }
    native_source_size = lowered->source_size;
    code = lowered->code;
    // The lowered header carries the rasterizer clip flag (RequiresRasterizerClip).
    std::memcpy(&sph, code.data(), sizeof(sph));
    cached_lowest = 0;
    cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    lowered_native = std::move(lowered);
}

std::shared_ptr<const LoweredNativeCode> GenericEnvironment::LowerNativeCode(u32 source_header_size) {
    using Shader::Ampere::Instruction;
    const u32 native_header_size = source_header_size;
    constexpr u32 maximum_instructions = Shader::Ampere::ProgramCaptureState::MaximumInstructions;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    NativeInstructionReader reader{*gpu_memory};
    for (u32 index = 0; index < maximum_instructions; ++index) {
        const GPUVAddr address = program_base + native_header_size + index * sizeof(Instruction);
        Instruction instruction;
        if (!reader.TryRead(address, instruction)) {
            // Not inside a fully mapped page: check and read this one instruction on its own.
            if (!gpu_memory->IsFullyMappedRange(address, sizeof(Instruction))) {
                // The program may end before a branch target past its last EXIT (the capture
                // can't tell); stop at the mapping's end and let the lowering judge what was
                // captured.
                if (!native.empty()) break;
                throw Shader::RuntimeError("Unmapped native shader instruction at {:#x}", address);
            }
            gpu_memory->ReadBlock(address, &instruction, sizeof(instruction));
        }
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, index);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            if (const char* dir = std::getenv("EDEN_NX2_SHADER_DUMP")) {
                const auto path = fmt::format("{}/rejected-{:x}.bin", dir, program_base + native_header_size);
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(native.data(), sizeof(Instruction), std::min<size_t>(native.size(), 4096), file);
                    std::fclose(file);
                }
            }
            throw Shader::RuntimeError("Native shader branch outside capture bounds at {:#x} +{:#x} [{:08x} {:08x} {:08x} {:08x}]",
                                       program_base + native_header_size, index * 16,
                                       instruction.words[0], instruction.words[1],
                                       instruction.words[2], instruction.words[3]);
        }
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    auto lowered = Shader::Ampere::LowerToMaxwell(native, {
        .code_address = program_base + native_header_size,
        .single_invocation_geometry = source_header_size && sph.common0.shader_type == 4 &&
                                       sph.common2.threads_per_input_primitive == 1,
        .allow_rasterizer_clip = source_header_size && sph.common0.shader_type == 1,
        .geometry_stage = stage == Shader::Stage::Geometry,
        .support_cooperative_matrix = support_cooperative_matrix && stage == Shader::Stage::Compute});
    // EDEN_NX2_REJECT=<hex address>,...: treat these programs as rejected (to find which newly
    // lowered program changes a game's behaviour without rebuilding).
    if (static const std::string reject_list = [] {
            const char* text = std::getenv("EDEN_NX2_REJECT");
            return text ? "," + std::string{text} + "," : std::string{};
        }();
        !reject_list.empty() &&
        reject_list.find(fmt::format(",{:x},", program_base)) != std::string::npos) {
        throw Shader::RuntimeError("Native shader at {:#x} rejected by EDEN_NX2_REJECT", program_base);
    }
    // EDEN_NX2_SHADER_DUMP=<dir>: keep every captured native program (named by its key) so the
    // lowering can be tested offline against all of a game's shaders.
    const auto dump_mapped = [&](const std::filesystem::path& path) {
        std::ofstream mapped(path, std::ios::binary);
        if (!mapped) return 0U;
        std::array<u8, 0x1000> block{};
        u32 offset = 0;
        while (offset < 0x10000) {
            const size_t size = std::min<size_t>(
                block.size() - ((program_base + offset) % block.size()), 0x10000 - offset);
            if (!gpu_memory->IsWithinGPUAddressRange(program_base + offset) ||
                !gpu_memory->IsWithinGPUAddressRange(program_base + offset + size - 1) ||
                !gpu_memory->IsContinuousRange(program_base + offset, size)) {
                break;
            }
            gpu_memory->ReadBlock(program_base + offset, block.data(), size);
            mapped.write(reinterpret_cast<const char*>(block.data()), size);
            if (!mapped) break;
            offset += static_cast<u32>(size);
        }
        return offset;
    };
    if (static const char* const dump_directory = std::getenv("EDEN_NX2_SHADER_DUMP");
        dump_directory != nullptr) {
        const auto folder = std::filesystem::path{dump_directory};
        const auto path = folder / fmt::format("{:016x}-{}-{:x}.bin", Shader::Ampere::NativeProgramKey(native),
                                               source_header_size == 0 ? "cs" : "gfx", program_base);
        if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(folder);
            std::ofstream dump(path, std::ios::binary);
            dump.write(reinterpret_cast<const char*>(native.data()),
                       native.size() * sizeof(Shader::Ampere::Instruction));
        }
        if (!lowered && std::getenv("EDEN_NX2_SHADER_DUMP_MAPPED")) {
            auto mapped_path = path;
            mapped_path.replace_extension(".mapped.bin");
            const u32 size = dump_mapped(mapped_path);
            LOG_INFO(Shader, "Mapped native shader dump at {:#x}: {} bytes in {}", program_base,
                     size, mapped_path.string());
        }
    }
    if (!lowered) {
        if (const char* directory = std::getenv("EDEN_NATIVE_SHADER_OVERRIDE_DIR")) {
            const auto key = fmt::format("{:016x}", Shader::Ampere::NativeProgramKey(native));
            const auto folder = std::filesystem::path{directory};
            std::filesystem::create_directories(folder);
            const auto source_path = folder / (key + ".source.bin");
            const auto override_path = folder / (key + ".override.bin");
            {
                std::ofstream dump(source_path, std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native.data()), native.size() * sizeof(Shader::Ampere::Instruction));
            }
            {
                // Retain mapped bytes beyond the first EXIT for investigating
                // native CALL targets in compiler library subroutines.
                dump_mapped(folder / (key + ".mapped.bin"));
            }
            LOG_WARNING(Shader, "Native shader paused: key={}, offset={:#x}, reason={}, source={}",
                        key, lowered.error_offset, lowered.error, source_path.string());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes{30};
            const bool wait_for_override = std::getenv("EDEN_NATIVE_SHADER_OVERRIDE_NO_WAIT") == nullptr;
            while (!gpu_memory->IsShuttingDown() && std::chrono::steady_clock::now() < deadline &&
                   (wait_for_override || std::filesystem::exists(override_path))) {
                if (std::filesystem::exists(override_path)) {
                    std::ifstream file(override_path, std::ios::binary | std::ios::ate);
                    const auto length = file.tellg();
                    if (length <= 0 || length > 16 * 1024 * 1024) {
                        throw Shader::RuntimeError("Invalid native shader override size");
                    }
                    std::vector<u8> bytes(static_cast<size_t>(length));
                    file.seekg(0);
                    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                    if (!file) throw Shader::RuntimeError("Native shader override read failed");
                    const u32 source_bytes = Shader::Ampere::ProgramOverrideSourceSize(bytes);
                    if (source_bytes > native.size() * sizeof(Shader::Ampere::Instruction)) {
                        if (!gpu_memory->IsFullyMappedRange(program_base + native_header_size, source_bytes))
                            throw Shader::RuntimeError("Unmapped extended native shader source");
                        native.resize(source_bytes / sizeof(Shader::Ampere::Instruction));
                        gpu_memory->ReadBlock(program_base + native_header_size, native.data(), source_bytes);
                    }
                    lowered = Shader::Ampere::DecodeProgramOverride(bytes, native);
                    if (!lowered) throw Shader::RuntimeError("Native shader override rejected: {}", lowered.error);
                    LOG_WARNING(Shader, "Native shader resumed: key={}, instructions={}", key, lowered.instructions.size());
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    }
    if (!lowered) {
        const size_t failed = lowered.error_offset / sizeof(Shader::Ampere::Instruction);
        const auto words = failed < native.size() ? native[failed].words : std::array<u32, 4>{};
        throw Shader::RuntimeError(
            "Native shader at {:#x}, instruction {:#x}: {} [op {:03x} {:08x} {:08x} {:08x} {:08x}]",
            program_base, lowered.error_offset, lowered.error, words[0] & 0xfff, words[0],
            words[1], words[2], words[3]);
    }
    auto result = std::make_shared<LoweredNativeCode>();
    result->texture_bound = lowered.required_texture_buffer;
    result->surface_bound = lowered.required_surface_buffer;
    result->source_size = native_header_size + lowered.source_size;
    auto& lowered_code = result->code;
    lowered_code.resize(sizeof(sph) / sizeof(u64));
    // Private converted-header metadata, retained by existing disk-cache serialization.
    sph.common0.reserved1.Assign(lowered.requires_rasterizer_clip ? 3 : 0);
    std::memcpy(lowered_code.data(), &sph, sizeof(sph));
    for (const u64 instruction : lowered.instructions) {
        if ((lowered_code.size() * sizeof(u64)) % 32 == 0) {
            lowered_code.push_back(0); // Maxwell scheduling word, absent from native instructions.
        }
        lowered_code.push_back(instruction);
    }
    // Keep the original padding ABI, then retain the native address map as
    // unreachable data. Existing disk-cache serialization carries it with code.
    lowered_code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(lowered_code, lowered.source_instruction_offsets);
    LOG_DEBUG(Shader, "Lowered native shader at {:#x}: {} source bytes, {} instructions",
              program_base, result->source_size, lowered.instructions.size());
    return result;
}

GraphicsEnvironment::GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                         Tegra::MemoryManager& gpu_memory_,
                                         Maxwell::ShaderType program, GPUVAddr program_base_,
                                         u32 start_address_,
                                         std::shared_ptr<const LoweredNativeCode> lowered_native_,
                                         bool support_cooperative_matrix_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_, support_cooperative_matrix_},
      maxwell3d{&maxwell3d_} {
    gpu_memory->ReadBlock(program_base + start_address, &sph, sizeof(sph));
    initial_offset = sizeof(sph);
    texture_bound = maxwell3d->regs.bindless_texture_const_buffer_slot;
    const auto& regs{maxwell3d->regs};
    const bool native{maxwell3d->GetNativeMacroEngine().native_turing};
    // Task and mesh programs: the task in the vertex slot, its mesh program in the tessellation
    // slot (without a task, the mesh program is in the vertex slot).
    const bool mesh_slot{native && regs.IsMeshEnabled() &&
                         (program == Maxwell::ShaderType::VertexB ||
                          program == Maxwell::ShaderType::Tessellation)};
    gp_passthrough_mask = maxwell3d->regs.post_vtg_shader_attrib_skip_mask;
    switch (program) {
    case Maxwell::ShaderType::VertexA:
        stage = Shader::Stage::VertexA;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::VertexB:
        stage = Shader::Stage::VertexB;
        stage_index = 0;
        break;
    case Maxwell::ShaderType::TessellationInit:
        stage = Shader::Stage::TessellationControl;
        stage_index = 1;
        break;
    case Maxwell::ShaderType::Tessellation:
        stage = Shader::Stage::TessellationEval;
        stage_index = 2;
        break;
    case Maxwell::ShaderType::Geometry:
        stage = Shader::Stage::Geometry;
        stage_index = 3;
        break;
    case Maxwell::ShaderType::Pixel:
        stage = Shader::Stage::Fragment;
        stage_index = 4;
        break;
    default:
        ASSERT_MSG(false, "Invalid program={}", program);
        break;
    }
    if (mesh_slot) {
        const bool has_task{regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation)};
        stage = program == Maxwell::ShaderType::VertexB && has_task ? Shader::Stage::Task
                                                                    : Shader::Stage::Mesh;
        mesh_registers = {regs.reg_array[Maxwell::MeshShaderA], regs.reg_array[Maxwell::MeshShaderB],
                          regs.reg_array[Maxwell::MeshInitShader]};
        if (stage == Shader::Stage::Mesh && regs.IsShaderConfigEnabled(Maxwell::ShaderType::Geometry)) {
            const auto& geometry{regs.pipelines[static_cast<size_t>(Maxwell::ShaderType::Geometry)]};
            const GPUVAddr address{(static_cast<u64>(geometry.reserved[0] & 0xff) << 32) |
                                   geometry.reserved[1]};
            gpu_memory->ReadBlock(address, &mesh_primitive_header, sizeof(mesh_primitive_header));
        }
    }
    if (native) {
        if (program != Maxwell::ShaderType::VertexB && program != Maxwell::ShaderType::Geometry &&
            program != Maxwell::ShaderType::Pixel && !mesh_slot) {
            if (std::getenv("EDEN_NX2_SHADER_DUMP")) {
                InitializeNativeCode(nullptr);
            }
            throw Shader::RuntimeError("Unsupported native graphics shader stage {}", static_cast<u32>(program));
        }
        if (std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS")) {
            const u32 native_stage = program == Maxwell::ShaderType::Pixel ? 4 :
                                     program == Maxwell::ShaderType::Geometry ? 3 : 0;
            const auto& table = maxwell3d->state.shader_stages[native_stage].const_buffers[1];
            if (table.enabled && table.size && table.size <= Tegra::Engines::Maxwell3D::Regs::MaxConstBufferSize) {
                std::vector<u8> bytes(table.size);
                gpu_memory->ReadBlock(table.address, bytes.data(), bytes.size());
                std::ofstream dump(fmt::format("native-graphics-cbuf-{}-1.bin", static_cast<u32>(program)), std::ios::binary);
                dump.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            }
        }
        if (program == Maxwell::ShaderType::Pixel && std::getenv("EDEN_NX2_SHADER_DUMP")) {
            static u32 gather_probes{};
            const auto mapped = [&](GPUVAddr address, u64 size) {
                return size != 0 && size - 1 <= std::numeric_limits<GPUVAddr>::max() - address &&
                       gpu_memory->IsWithinGPUAddressRange(address) &&
                       gpu_memory->IsWithinGPUAddressRange(address + size - 1) &&
                       gpu_memory->IsFullyMappedRange(address, size);
            };
            if (gather_probes < 8 && mapped(program_base, 0xc0)) {
                Shader::Ampere::Instruction gather;
                gpu_memory->ReadBlock(program_base + 0xb0, &gather, sizeof(gather));
                if (gather.Bits(0, 12) == 0xb63 && gather.Bits(79, 2) == 1) {
                    ++gather_probes;
                    const u32 bank = gather.Bits(54, 5);
                    const auto& buffers = maxwell3d->state.shader_stages[4].const_buffers;
                    LOG_WARNING(Shader, "Native gather provenance: program={:#x} stage=4 "
                                "bound_bank={} encoded_bank={} words={:08x} {:08x} {:08x} {:08x}",
                                program_base, texture_bound, bank, gather.words[0], gather.words[1],
                                gather.words[2], gather.words[3]);
                    if (bank >= buffers.size()) {
                        LOG_WARNING(Shader, "Native gather provenance: invalid bank {}", bank);
                    } else {
                        const auto& buffer = buffers[bank];
                        for (const u32 offset : {gather.Bits(40, 14) * 4, 0x170U}) {
                            if (!buffer.enabled || offset > buffer.size || buffer.size - offset < 4 ||
                                !mapped(buffer.address, u64{offset} + 4)) {
                                LOG_WARNING(Shader, "Native gather provenance: program={:#x} "
                                            "bank={} offset={:#x} unavailable enabled={} base={:#x} size={:#x}",
                                            program_base, bank, offset, buffer.enabled,
                                            buffer.address, buffer.size);
                                continue;
                            }
                            const u32 raw = gpu_memory->Read<u32>(buffer.address + offset);
                            const auto pair = Tegra::Texture::TexturePair(
                                raw, regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding);
                            const GPUVAddr pool = regs.tex_header.Address();
                            const u64 descriptor_offset = u64{pair.first} * sizeof(Tegra::Texture::TICEntry);
                            if (pair.first > regs.tex_header.limit ||
                                descriptor_offset > std::numeric_limits<GPUVAddr>::max() - pool ||
                                !mapped(pool + descriptor_offset, sizeof(Tegra::Texture::TICEntry))) {
                                LOG_WARNING(Shader, "Native gather provenance: program={:#x} "
                                            "bank={} offset={:#x} handle={:#x} tic={} tsc={} "
                                            "unavailable pool={:#x} limit={}", program_base, bank,
                                            offset, raw, pair.first, pair.second, pool, regs.tex_header.limit);
                                continue;
                            }
                            Tegra::Texture::TICEntry entry;
                            gpu_memory->ReadBlock(pool + descriptor_offset, &entry, sizeof(entry));
                            LOG_WARNING(Shader, "Native gather provenance: program={:#x} bank={} "
                                        "base={:#x} size={:#x} offset={:#x} handle={:#x} tic={} tsc={} "
                                        "pool={:#x} limit={} raw={:016x} {:016x} {:016x} {:016x} "
                                        "format={} types={},{},{},{} swizzles={},{},{},{} srgb={} "
                                        "type={} header={} dimensions={}x{} texture={:#x}",
                                        program_base, bank, buffer.address, buffer.size, offset, raw,
                                        pair.first, pair.second, pool, regs.tex_header.limit,
                                        entry.raw[0], entry.raw[1], entry.raw[2], entry.raw[3],
                                        static_cast<u32>(entry.format.Value()), static_cast<u32>(entry.r_type.Value()),
                                        static_cast<u32>(entry.g_type.Value()), static_cast<u32>(entry.b_type.Value()),
                                        static_cast<u32>(entry.a_type.Value()), static_cast<u32>(entry.x_source.Value()),
                                        static_cast<u32>(entry.y_source.Value()), static_cast<u32>(entry.z_source.Value()),
                                        static_cast<u32>(entry.w_source.Value()), entry.srgb_conversion.Value(),
                                        static_cast<u32>(entry.texture_type.Value()), static_cast<u32>(entry.header_version.Value()),
                                        entry.Width(), entry.Height(), entry.Address());
                        }
                    }
                }
            }
        }
        InitializeNativeCode(std::move(lowered_native_));
    }
    const u64 local_size{sph.LocalMemorySize()};
    ASSERT(local_size <= (std::numeric_limits<u32>::max)());
    local_memory_size = static_cast<u32>(local_size) + sph.common3.shader_local_memory_crs_size;
    is_proprietary_driver = maxwell3d->GetNativeMacroEngine().native_turing || texture_bound == 2;
    has_hle_engine_state =
        maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro;
}

u32 GraphicsEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    u32 value{};
    if (snapshot) {
        const auto& cbuf{snapshot->const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            std::memcpy(&value, cbuf.bytes.data() + cbuf_offset, sizeof(value));
        }
    } else {
        const auto& cbuf{maxwell3d->state.shader_stages[stage_index].const_buffers[cbuf_index]};
        ASSERT(cbuf.enabled);
        if (cbuf_offset < cbuf.size) {
            value = gpu_memory->Read<u32>(cbuf.address + cbuf_offset);
        }
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

std::optional<Shader::ReplaceConstant> GraphicsEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    if (!has_hle_engine_state) {
        return std::nullopt;
    }
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    const auto& replace_table{snapshot ? snapshot->replace_table : maxwell3d->replace_table};
    auto it = replace_table.find(key);
    if (it == replace_table.end()) {
        return std::nullopt;
    }
    const auto converted_value = [](Tegra::Engines::Maxwell3D::HLEReplacementAttributeType name) {
        switch (name) {
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseVertex:
            return Shader::ReplaceConstant::BaseVertex;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::BaseInstance:
            return Shader::ReplaceConstant::BaseInstance;
        case Tegra::Engines::Maxwell3D::HLEReplacementAttributeType::DrawID:
            return Shader::ReplaceConstant::DrawID;
        default:
            UNREACHABLE();
        }
    }(it->second);
    cbuf_replacements.emplace(key, converted_value);
    return converted_value;
}

Shader::TextureType GraphicsEnvironment::ReadTextureType(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat GraphicsEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto entry{ReadTextureDescriptor(handle)};
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool GraphicsEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 GraphicsEnvironment::ReadTextureSamplerState(u32 handle) {
    u32 result{};
    if (!snapshot) {
        const auto& regs{maxwell3d->regs};
        const bool via_header_index{regs.sampler_binding ==
                                    Maxwell::SamplerBinding::ViaHeaderBinding};
        const u32 limit{via_header_index ? regs.tex_header.limit : regs.tex_sampler.limit};
        if (const auto entry{
                ReadSamplerInfo(regs.tex_sampler.Address(), limit, via_header_index, handle)}) {
            result = PackSamplerState(*entry);
        }
    } else {
        const u32 index{Tegra::Texture::TexturePair(handle, snapshot->via_header_index).second};
        if (index < snapshot->sampler_states.size()) {
            result = snapshot->sampler_states[index];
        }
    }
    texture_sampler_states.emplace(handle, result);
    return result;
}

u32 GraphicsEnvironment::ReadViewportTransformState() {
    viewport_transform_state = snapshot ? snapshot->viewport_scale_offset_enabled
                                        : maxwell3d->regs.viewport_scale_offset_enabled;
    return viewport_transform_state;
}

Tegra::Texture::TICEntry GraphicsEnvironment::ReadTextureDescriptor(u32 handle) {
    if (!snapshot) {
        const auto& regs{maxwell3d->regs};
        const bool via_header_index{regs.sampler_binding ==
                                    Maxwell::SamplerBinding::ViaHeaderBinding};
        return ReadTextureInfo(regs.tex_header.Address(), regs.tex_header.limit,
                               via_header_index, handle);
    }
    const u32 index{Tegra::Texture::TexturePair(handle, snapshot->via_header_index).first};
    ASSERT(index < snapshot->texture_pool.size());
    if (index >= snapshot->texture_pool.size()) {
        return {};
    }
    return snapshot->texture_pool[index];
}

// Runs on the GPU thread, before the environment is handed to a translation worker.
// - Constant buffers are copied whole: translation folds some of their values into the shader
//   (driver constants in bank 1, texture handles, indirect branch tables), and games reuse
//   uniform memory once the GPU is past the draw, so a later read could see another draw's data.
// - The texture pool is copied up to its limit, read like the attached path reads entries.
// - The viewport transform flag and the HLE macro replacements are copied.
bool GraphicsEnvironment::Detach() {
    constexpr u32 MaxTexturePoolEntries = 1U << 15;
    const auto& regs{maxwell3d->regs};
    if (regs.tex_header.limit >= MaxTexturePoolEntries) {
        return false;
    }
    const GPUVAddr pool_address{regs.tex_header.Address()};
    const size_t pool_entries{size_t{regs.tex_header.limit} + 1};
    const size_t pool_size{pool_entries * sizeof(Tegra::Texture::TICEntry)};
    if (!gpu_memory->IsFullyMappedRange(pool_address, pool_size)) {
        return false;
    }
    EngineSnapshot copy;
    copy.texture_pool.resize(pool_entries);
    gpu_memory->ReadBlock(pool_address, copy.texture_pool.data(), pool_size);
    const auto& live_buffers{maxwell3d->state.shader_stages[stage_index].const_buffers};
    for (size_t index = 0; index < live_buffers.size(); ++index) {
        const auto& live{live_buffers[index]};
        auto& copied{copy.const_buffers[index]};
        copied.enabled = live.enabled;
        if (!live.enabled || live.size == 0) {
            continue;
        }
        // The size register can exceed what a constant buffer holds; never copy more than that.
        copied.size = (std::min)(live.size, static_cast<u32>(Maxwell::MaxConstBufferSize));
        copied.bytes.resize(copied.size + sizeof(u32));
        gpu_memory->ReadBlockUnsafe(live.address, copied.bytes.data(), copied.bytes.size());
    }
    copy.via_header_index = regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding;
    // The sampler pool, packed to what translation reads of each entry (a byte).
    const u32 sampler_limit{copy.via_header_index ? regs.tex_header.limit
                                                  : regs.tex_sampler.limit};
    const GPUVAddr sampler_address{regs.tex_sampler.Address()};
    const size_t sampler_entries{size_t{sampler_limit} + 1};
    if (sampler_limit < MaxTexturePoolEntries &&
        gpu_memory->IsFullyMappedRange(sampler_address,
                                       sampler_entries * sizeof(Tegra::Texture::TSCEntry))) {
        copy.sampler_states.resize(sampler_entries);
        std::array<Tegra::Texture::TSCEntry, 256> chunk;
        for (size_t first = 0; first < sampler_entries; first += chunk.size()) {
            const size_t count{(std::min)(chunk.size(), sampler_entries - first)};
            gpu_memory->ReadBlock(sampler_address + first * sizeof(Tegra::Texture::TSCEntry),
                                  chunk.data(), count * sizeof(Tegra::Texture::TSCEntry));
            for (size_t i = 0; i < count; ++i) {
                copy.sampler_states[first + i] = static_cast<u8>(PackSamplerState(chunk[i]));
            }
        }
    }
    copy.viewport_scale_offset_enabled = regs.viewport_scale_offset_enabled;
    if (has_hle_engine_state) {
        copy.replace_table = maxwell3d->replace_table;
    }
    snapshot = std::move(copy);
    maxwell3d = nullptr;
    return true;
}

size_t GraphicsEnvironment::SnapshotSizeBytes() const noexcept {
    if (!snapshot) {
        return 0;
    }
    size_t total{};
    for (const auto& cbuf : snapshot->const_buffers) {
        total += cbuf.bytes.size();
    }
    return total + snapshot->texture_pool.size() * sizeof(Tegra::Texture::TICEntry) +
           snapshot->sampler_states.size();
}

ComputeEnvironment::ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                       Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                       u32 start_address_,
                                       std::shared_ptr<const LoweredNativeCode> lowered_native_,
                                       bool support_cooperative_matrix_)
    : GenericEnvironment{gpu_memory_, program_base_, start_address_, support_cooperative_matrix_},
      kepler_compute{
                                                                      &kepler_compute_} {
    const auto& qmd{kepler_compute->launch_description};
    stage = Shader::Stage::Compute;
    local_memory_size = qmd.local_pos_alloc + qmd.local_crs_alloc;
    texture_bound = kepler_compute->regs.tex_cb_index;
    is_proprietary_driver = texture_bound == 2;
    shared_memory_size = qmd.shared_alloc;
    workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z};
    if (kepler_compute->IsNativeMode()) {
        initial_offset = sizeof(sph);
        is_proprietary_driver = true;
        InitializeNativeCode(std::move(lowered_native_), 0, true);
        start_address = sizeof(sph);
    }
}

u32 ComputeEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto& qmd{kepler_compute->launch_description};
    ASSERT(((qmd.const_buffer_enable_mask.Value() >> cbuf_index) & 1) != 0);
    const auto& cbuf{qmd.const_buffer_config[cbuf_index]};
    u32 value{};
    if (cbuf_offset < cbuf.size) {
        value = gpu_memory->Read<u32>(cbuf.Address() + cbuf_offset);
    }
    cbuf_values.emplace(MakeCbufKey(cbuf_index, cbuf_offset), value);
    return value;
}

Shader::TextureType ComputeEnvironment::ReadTextureType(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TextureType result{ConvertTextureType(entry)};
    texture_types.emplace(handle, result);
    return result;
}

Shader::TexturePixelFormat ComputeEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const auto& qmd{kepler_compute->launch_description};
    auto entry = ReadTextureInfo(regs.tic.Address(), regs.tic.limit, qmd.linked_tsc != 0, handle);
    const Shader::TexturePixelFormat result(ConvertTexturePixelFormat(entry));
    texture_pixel_formats.emplace(handle, result);
    return result;
}

bool ComputeEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 ComputeEnvironment::ReadTextureSamplerState(u32 handle) {
    const auto& regs{kepler_compute->regs};
    const bool linked_tsc{kepler_compute->launch_description.linked_tsc != 0};
    const u32 limit{linked_tsc ? regs.tic.limit : regs.tsc.limit};
    u32 result{};
    if (const auto entry{ReadSamplerInfo(regs.tsc.Address(), limit, linked_tsc, handle)}) {
        result = PackSamplerState(*entry);
    }
    texture_sampler_states.emplace(handle, result);
    return result;
}

u32 ComputeEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

void FileEnvironment::Deserialize(std::ifstream& file) {
    u64 code_size{};
    u64 num_texture_types{};
    u64 num_texture_pixel_formats{};
    u64 num_texture_sampler_states{};
    u64 num_cbuf_values{};
    u64 num_storage_pointer_types{};
    u64 num_cbuf_replacement_values{};
    file.read(reinterpret_cast<char*>(&code_size), sizeof(code_size))
        .read(reinterpret_cast<char*>(&num_texture_types), sizeof(num_texture_types))
        .read(reinterpret_cast<char*>(&num_texture_pixel_formats),
              sizeof(num_texture_pixel_formats))
        .read(reinterpret_cast<char*>(&num_texture_sampler_states),
              sizeof(num_texture_sampler_states))
        .read(reinterpret_cast<char*>(&num_cbuf_values), sizeof(num_cbuf_values))
        .read(reinterpret_cast<char*>(&num_storage_pointer_types),
              sizeof(num_storage_pointer_types))
        .read(reinterpret_cast<char*>(&num_cbuf_replacement_values),
              sizeof(num_cbuf_replacement_values))
        .read(reinterpret_cast<char*>(&local_memory_size), sizeof(local_memory_size))
        .read(reinterpret_cast<char*>(&texture_bound), sizeof(texture_bound))
        .read(reinterpret_cast<char*>(&surface_bound), sizeof(surface_bound))
        .read(reinterpret_cast<char*>(&start_address), sizeof(start_address))
        .read(reinterpret_cast<char*>(&read_lowest), sizeof(read_lowest))
        .read(reinterpret_cast<char*>(&read_highest), sizeof(read_highest))
        .read(reinterpret_cast<char*>(&viewport_transform_state), sizeof(viewport_transform_state))
        .read(reinterpret_cast<char*>(&stage), sizeof(stage));
    code.resize(Common::DivCeil(code_size, sizeof(u64)));
    file.read(reinterpret_cast<char*>(code.data()), code_size);
    for (size_t i = 0; i < num_texture_types; ++i) {
        u32 key;
        Shader::TextureType type;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&type), sizeof(type));
        texture_types.emplace(key, type);
    }
    for (size_t i = 0; i < num_texture_pixel_formats; ++i) {
        u32 key;
        Shader::TexturePixelFormat format;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&format), sizeof(format));
        texture_pixel_formats.emplace(key, format);
    }
    for (size_t i = 0; i < num_texture_sampler_states; ++i) {
        u32 key;
        u32 state;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&state), sizeof(state));
        texture_sampler_states.emplace(key, state);
    }
    for (size_t i = 0; i < num_cbuf_values; ++i) {
        u64 key;
        u32 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_values.emplace(key, value);
    }
    for (size_t i = 0; i < num_storage_pointer_types; ++i) {
        u64 key;
        u8 value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        storage_pointer_types.emplace(key, value);
    }
    for (size_t i = 0; i < num_cbuf_replacement_values; ++i) {
        u64 key;
        Shader::ReplaceConstant value;
        file.read(reinterpret_cast<char*>(&key), sizeof(key))
            .read(reinterpret_cast<char*>(&value), sizeof(value));
        cbuf_replacements.emplace(key, value);
    }
    if (stage == Shader::Stage::Compute) {
        file.read(reinterpret_cast<char*>(&workgroup_size), sizeof(workgroup_size))
            .read(reinterpret_cast<char*>(&shared_memory_size), sizeof(shared_memory_size));
        initial_offset = Shader::Ampere::HasNativeAddressMap(code) ? sizeof(sph) : 0;
    } else {
        file.read(reinterpret_cast<char*>(&sph), sizeof(sph));
        initial_offset = sizeof(sph);
        if (stage == Shader::Stage::Geometry) {
            file.read(reinterpret_cast<char*>(&gp_passthrough_mask), sizeof(gp_passthrough_mask));
        }
    }
    // Native programs fold bank 1 reads as they did when they were built.
    is_proprietary_driver = texture_bound == 2 || Shader::Ampere::HasNativeAddressMap(code);
}

void FileEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    DumpImpl(pipeline_hash, shader_hash, code, read_highest, read_lowest, initial_offset, stage);
}

u32 FileEnvironment::TranslateProgramAddress(u32 address) const {
    if (const auto translated = Shader::Ampere::NativeProgramAddress(code, address)) return *translated;
    if (Shader::Ampere::HasNativeAddressMap(code))
        throw Shader::RuntimeError("Invalid cached native indirect branch address {:#x}", address);
    return address;
}

bool FileEnvironment::IsNativeProgram() const {
    return Shader::Ampere::HasNativeAddressMap(code);
}

bool FileEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const auto it = storage_pointer_types.find(MakeCbufKey(index, offset));
    if (it == storage_pointer_types.end()) {
        throw Shader::LogicError("Uncached storage pointer classification");
    }
    return it->second != 0;
}

u64 FileEnvironment::StoragePointerTypesHash() const {
    return HashStoragePointerTypes(storage_pointer_types);
}

u64 FileEnvironment::ReadInstruction(u32 address) {
    if (address < read_lowest || address > read_highest) {
        throw Shader::LogicError("Out of bounds address {}", address);
    }
    return code[(address - read_lowest) / sizeof(u64)];
}

u32 FileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    const auto it{cbuf_values.find(MakeCbufKey(cbuf_index, cbuf_offset))};
    if (it == cbuf_values.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TextureType FileEnvironment::ReadTextureType(u32 handle) {
    const auto it{texture_types.find(handle)};
    if (it == texture_types.end()) {
        throw Shader::LogicError("Uncached read texture type");
    }
    return it->second;
}

Shader::TexturePixelFormat FileEnvironment::ReadTexturePixelFormat(u32 handle) {
    const auto it{texture_pixel_formats.find(handle)};
    if (it == texture_pixel_formats.end()) {
        throw Shader::LogicError("Uncached read texture pixel format");
    }
    return it->second;
}

bool FileEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return VideoCore::Surface::IsPixelFormatInteger(
        static_cast<VideoCore::Surface::PixelFormat>(ReadTexturePixelFormat(handle)));
}

u32 FileEnvironment::ReadTextureSamplerState(u32 handle) {
    const auto it{texture_sampler_states.find(handle)};
    if (it == texture_sampler_states.end()) {
        throw Shader::LogicError("Uncached read texture sampler state");
    }
    return it->second;
}

u32 FileEnvironment::ReadViewportTransformState() {
    return viewport_transform_state;
}

u32 FileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 FileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

u32 FileEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 FileEnvironment::SurfaceBoundBuffer() const {
    return surface_bound != ~0U ? surface_bound : texture_bound;
}

std::array<u32, 3> FileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

std::optional<Shader::ReplaceConstant> FileEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                              u32 offset) {
    const u64 key = (static_cast<u64>(bank) << 32) | static_cast<u64>(offset);
    auto it = cbuf_replacements.find(key);
    if (it == cbuf_replacements.end()) {
        return std::nullopt;
    }
    return it->second;
}

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version) {
    if (!std::ranges::all_of(envs, &GenericEnvironment::CanBeSerialized)) {
        return;
    }
    std::ofstream file(filename, std::ios::binary | std::ios::ate | std::ios::app);
    // Checked before the exception mask is set: set first, it threw for a file that could not
    // be opened (held by another process for a moment), and the handler deleted the cache.
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    // Where this pipeline starts. A failed write is cut off there: left in the file, the next
    // boot's load would stop at it and drop every pipeline appended after it.
    const std::streamoff begin = file.tellp();
    if (begin < 0) {
        LOG_ERROR(Common_Filesystem, "Failed to seek in pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    try {
        file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        if (begin == 0) {
            // Write header
            file.write(MAGIC_NUMBER.data(), MAGIC_NUMBER.size())
                .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
        }
        const u32 num_envs{static_cast<u32>(envs.size())};
        file.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
        for (const GenericEnvironment* const env : envs) {
            env->Serialize(file);
        }
        file.write(key.data(), key.size_bytes());
        file.close();
        return;
    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "Failed to write pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), e.what());
    }
    // Closed before the cut, or the buffer's flush would write the partial pipeline back.
    file.exceptions(std::ios::goodbit);
    file.close();
    std::error_code error;
    if (begin > 0) {
        std::filesystem::resize_file(filename, static_cast<std::uintmax_t>(begin), error);
        if (!error) {
            return;
        }
    }
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

void LoadPipelines(
    std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    Common::UniqueFunction<void, std::ifstream&, FileEnvironment> load_compute,
    Common::UniqueFunction<void, std::ifstream&, std::vector<FileEnvironment>> load_graphics) {
    // End of the last pipeline read whole. A damaged one after it (the emulator stopped while
    // appending it) is cut off there; the pipelines before it stay.
    std::streamoff good_end = 0;
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return;
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem,
                              "Pipeline cache file is not Eglantine's (another emulator's, or "
                              "damaged), deleted it");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return;
        }
        good_end = PIPELINE_CACHE_HEADER_SIZE;
        while (file.tellg() != end) {
            if (stop_loading.stop_requested()) {
                return;
            }
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            // A pipeline has one to MaxShaderProgram programs; any other count is a damaged entry.
            if (num_envs == 0 || num_envs > Maxwell::MaxShaderProgram) {
                throw std::runtime_error(fmt::format("invalid program count {}", num_envs));
            }
            std::vector<FileEnvironment> envs(num_envs);
            for (FileEnvironment& env : envs) {
                env.Deserialize(file);
            }
            if (envs.front().ShaderStage() == Shader::Stage::Compute) {
                load_compute(file, std::move(envs.front()));
            } else {
                load_graphics(file, std::move(envs));
            }
            good_end = file.tellg();
        }
        return;
    } catch (const std::exception& e) {
        // A damaged or truncated file: read errors, and garbage sizes that fail to allocate. Any
        // of them would otherwise end the process on every boot of the game.
        LOG_ERROR(Common_Filesystem, "Pipeline cache {} is damaged after {} bytes: {}",
                  Common::FS::PathToUTF8String(filename), good_end, e.what());
    }
    if (good_end > PIPELINE_CACHE_HEADER_SIZE) {
        std::error_code error;
        std::filesystem::resize_file(filename, static_cast<std::uintmax_t>(good_end), error);
        if (!error) {
            LOG_WARNING(Common_Filesystem, "Kept the pipelines before the damage");
            return;
        }
    }
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

bool CompactPipelineCache(const std::filesystem::path& filename,
                          std::span<const PipelineCacheEntry> entries) {
    // Written beside the cache and renamed over it: stopped mid-way, the old file stays whole.
    auto temporary = filename;
    temporary += ".tmp";
    std::error_code error;
    try {
        std::ifstream in(filename, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            return false;
        }
        in.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const std::streamoff size = in.tellg();
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR(Common_Filesystem, "Failed to create {}",
                      Common::FS::PathToUTF8String(temporary));
            return false;
        }
        out.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        std::vector<char> buffer(1 << 20);
        const auto copy = [&](std::streamoff begin, std::streamoff end) {
            in.seekg(begin);
            while (begin < end) {
                const auto chunk =
                    static_cast<std::streamsize>(std::min<std::streamoff>(end - begin, 1 << 20));
                in.read(buffer.data(), chunk);
                out.write(buffer.data(), chunk);
                begin += chunk;
            }
        };
        // The header, then each run of kept pipelines in one copy.
        std::streamoff run_begin = 0;
        std::streamoff entry_begin = PIPELINE_CACHE_HEADER_SIZE;
        for (const PipelineCacheEntry& entry : entries) {
            const auto entry_end = static_cast<std::streamoff>(entry.end);
            if (entry_end <= entry_begin || entry_end > size) {
                throw std::runtime_error("the pipeline list does not match the file");
            }
            if (!entry.keep) {
                copy(run_begin, entry_begin);
                run_begin = entry_end;
            }
            entry_begin = entry_end;
        }
        // Then what was written since LoadPipelines read the file.
        copy(run_begin, size);
        out.close();
    } catch (const std::exception& e) {
        LOG_ERROR(Common_Filesystem, "Failed to compact pipeline cache {}: {}",
                  Common::FS::PathToUTF8String(filename), e.what());
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace pipeline cache {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace VideoCommon
