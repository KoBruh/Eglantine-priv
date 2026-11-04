// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/loader/nup.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::NupContainer;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},   {"en-gb", 1},  {"ja", 2},     {"fr", 3},     {"de", 4},
    {"es-419", 5},  {"es", 6},     {"it", 7},     {"nl", 8},     {"fr-ca", 9},
    {"pt", 10},     {"ru", 11},    {"ko", 12},    {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},  {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

// The entries that name the game, else the first: a new name goes where any language looks.
std::vector<std::size_t> NamedEntries(const std::vector<FileSys::LanguageEntry>& entries) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].GetApplicationName().empty()) {
            out.push_back(i);
        }
    }
    if (out.empty() && !entries.empty()) {
        out.push_back(0);
    }
    return out;
}

// The control data the dump was packed with when it had one (save and cache storage sizes,
// account flags...), else a minimal one. Only what the TLVs set is written over it: a name or
// publisher the packer copied out of this NACP is one of its own and keeps the per-language
// names; any other was set by hand (pack option or edit) and renames every language.
std::unique_ptr<FileSys::NACP> BuildNacp(const NupContainer::PackageInfo& meta, u64 title_id,
                                         const std::string& version) {
    std::unique_ptr<FileSys::NACP> nacp;
    if (meta.nacp.size() == sizeof(FileSys::RawNACP)) {
        nacp = std::make_unique<FileSys::NACP>(
            std::make_shared<FileSys::VectorVfsFile>(meta.nacp, "control.nacp"));
    } else {
        if (!meta.nacp.empty()) {
            LOG_WARNING(Loader, "NUP control data is {:#x} bytes instead of {:#x}; ignoring it",
                        meta.nacp.size(), sizeof(FileSys::RawNACP));
        }
        if (meta.title_name.empty() && meta.source_dump.empty() &&
            meta.localized_titles.empty() && meta.publisher.empty() && version.empty()) {
            return nullptr;
        }
        nacp = std::make_unique<FileSys::NACP>();
        nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);
    }
    auto& entries = nacp->language_entries;
    if (entries.empty()) {
        // No stored NACP, or a compressed title block that did not inflate.
        entries.resize(16);
    }

    std::string title = meta.title_name;
    const auto names = NamedEntries(entries);
    if (title.empty() && entries[names.front()].GetApplicationName().empty()) {
        title = meta.source_dump;
    }
    const auto has_name = [&](const std::string& name) {
        return std::ranges::any_of(entries, [&](const FileSys::LanguageEntry& entry) {
            return entry.GetApplicationName() == name;
        });
    };
    if (!title.empty() && !has_name(title)) {
        for (const std::size_t i : names) {
            CopyNameToEntry(entries[i], title);
        }
    }
    for (const auto& [lang, name] : meta.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            if (slot.tag == lang && slot.index < entries.size()) {
                CopyNameToEntry(entries[slot.index], name);
            }
        }
    }
    const auto has_developer = [&](const std::string& developer) {
        return std::ranges::any_of(entries, [&](const FileSys::LanguageEntry& entry) {
            return entry.GetDeveloperName() == developer;
        });
    };
    if (!meta.publisher.empty() && !has_developer(meta.publisher)) {
        for (const std::size_t i : NamedEntries(entries)) {
            CopyDeveloperToEntry(entries[i], meta.publisher);
        }
    }
    // The game reads the raw block. The compressed title format keeps its own copy of the
    // names there, which stays as packed; the uncompressed one holds the first 16 entries.
    if (nacp->raw.titles_data_format == FileSys::TitleDataFormat::Uncompressed) {
        std::memcpy(nacp->raw.language_entries.language_entries.data(), entries.data(),
                    std::min<std::size_t>(entries.size(), 16) * sizeof(FileSys::LanguageEntry));
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    if (nacp->raw.save_data_owner_id == 0) {
        nacp->raw.save_data_owner_id = title_id;
    }
    return nacp;
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<NupContainer>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();

    // The program comes from the base package; a standalone update container
    // (SPEC §4.1.2) carries a complete patched tree and boots on its own.
    std::optional<std::size_t> program_index;
    std::optional<std::size_t> exefs_index;
    if (base_index && nup->GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup->GetExeFSDir(*update_index) != nullptr
                          ? update_index
                          : base_index;
    } else if (update_index && nup->GetExeFSDir(*update_index) != nullptr) {
        LOG_INFO(Loader,
                 "NUP container is a standalone update; booting the update program directly");
        program_index = update_index;
        exefs_index = update_index;
    }
    if (!program_index || !exefs_index) {
        LOG_ERROR(Loader, "NUP container has no package with a usable ExeFS");
        return;
    }

    const auto& program = nup->GetPackages()[*program_index];
    base_title_id = program.title_id;
    has_program = true;

    const auto exefs = nup->GetExeFSDir(*exefs_index);
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP package has no usable ExeFS");
        return;
    }
    // The program loader is made at Load, over the ExeFS with its mods applied.
    exefs_dir = exefs;

    if (update_index) {
        const auto& update = nup->GetPackages()[*update_index];
        display_version = update.display_version.empty() ? DecodeVersion(update.version)
                                                         : update.display_version;
    } else {
        display_version = program.display_version.empty() ? DecodeVersion(program.version)
                                                          : program.display_version;
    }
    // Where the packer and the editor put the name, icon and control data; not always the
    // package the program boots from (a base without an ExeFS, for one).
    const auto& meta = nup->GetPackages()[nup->FindMetadataPackage()];
    nacp_file = BuildNacp(meta, base_title_id, display_version);
    if (!meta.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(meta.icon, "icon.dat");
    } else {
        for (const auto& [lang, icon] : meta.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
            }
        }
        if (icon_file == nullptr && !meta.localized_icons.empty()) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                meta.localized_icons.front().second, "icon.dat");
        }
    }

    const auto addon_count = std::count_if(nup->GetPackages().begin(), nup->GetPackages().end(),
                                           [](const auto& package) {
                                               return package.kind ==
                                                      NupContainer::PackageKind::Addon;
                                           });
    if (addon_count > 0) {
        LOG_WARNING(Loader,
                    "NUP container has {} add-on content package(s); AoC mounting is not "
                    "supported yet",
                    addon_count);
    }
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::NupContainer::IsNupFile(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || exefs_dir == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    // Mods only, no update: one installed in NAND, on the SD card or in the external content
    // folders is whatever version it is, while this container carries its own update, whose
    // RomFS the program runs against. A loose dump's loader is not asked to apply one either.
    const FileSys::PatchManager patch_manager{base_title_id, system.GetFileSystemController(),
                                              system.GetContentProvider()};
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(
        patch_manager.PatchExeFS(exefs_dir, false), false);
    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    // LayeredFS mods in load/<title id>/: the RomFS factory leaves this RomFS alone (it is not
    // updatable, the update is merged in already), so apply them here. Without an NCA,
    // PatchRomFS applies mods only, as for a loose dump.
    if (FileSys::VirtualFile base_romfs; ReadRomFS(base_romfs) == ResultStatus::Success) {
        if (auto patched = patch_manager.PatchRomFS(nullptr, base_romfs); patched != nullptr) {
            if (patched != base_romfs) {
                LOG_INFO(Loader, "Applied RomFS patches (LayeredFS) for NUP title {:016X}",
                         base_title_id);
            }
            romfs = std::move(patched);
        }
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    is_loaded = true;
    return result;
}

ResultStatus AppLoader_NUP::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    // Built once: after Load it is the image with the mods applied, which the RomFS factory and
    // later readers get.
    if (romfs == nullptr) {
        const auto base_index = nup->FindBasePackage();
        const auto base_romfs =
            base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
        const auto update_index = nup->FindBestUpdatePackage();
        const auto update_romfs =
            update_index ? nup->GetRomFSDir(*update_index) : FileSys::VirtualDir{};
        // A standalone update carries the complete merged tree; a base+update pair
        // overlays the update onto the base (SPEC §4.1.4).
        const auto merged = FileSys::NupContainer::MergeShadowDirs(base_romfs, update_romfs);
        if (merged == nullptr) {
            return ResultStatus::ErrorNoRomFS;
        }
        romfs = FileSys::CreateRomFS(merged);
    }
    if (romfs == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = romfs;
    return ResultStatus::Success;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader
