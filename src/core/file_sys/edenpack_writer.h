// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "common/common_types.h"

namespace FileSys::EdenPack {

struct DumpSource {
    std::filesystem::path exefs;
    std::filesystem::path romfs;
};

// Picks the ExeFS/RomFS a loose dump boots with: the newest updates/<version>, else base, else
// exefs/romfs at the root. Folder names are matched case-insensitively.
std::optional<DumpSource> ResolveDumpSource(const std::filesystem::path& dump_root);
bool IsLooseDump(const std::filesystem::path& dir);
// The dump folder a game list entry's exefs/main belongs to, if it is a loose dump.
std::optional<std::filesystem::path> DumpRootFromMain(const std::filesystem::path& main);
std::filesystem::path DefaultArchivePath(const std::filesystem::path& dump_root);

struct PackOptions {
    // Low-storage mode: delete each source file once its chunk is synced and journaled.
    bool delete_sources = false;
    u64 chunk_bytes = 256ULL * 1024 * 1024;
};

struct PackProgress {
    u64 bytes_done{};
    u64 bytes_total{};
    u64 files_done{};
    u64 files_total{};
};

enum class PackResult { Success, Cancelled, InvalidSource, IoError };

// Writes <archive>.part and renames it when done; an existing archive is never overwritten. A
// cancelled or failed low-storage run keeps its .part and .journal, and calling this again with
// the same arguments resumes it.
PackResult PackDump(const std::filesystem::path& dump_root, const std::filesystem::path& archive,
                    const PackOptions& options = {},
                    const std::function<void(const PackProgress&)>& progress = {},
                    const std::atomic_bool* cancel = nullptr, std::string* error_out = nullptr);

// True when an interrupted low-storage conversion left <archive>.part and <archive>.journal.
bool HasResumableJournal(const std::filesystem::path& archive);

} // namespace FileSys::EdenPack
