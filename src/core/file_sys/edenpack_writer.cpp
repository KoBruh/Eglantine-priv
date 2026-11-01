// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/edenpack_writer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/fs/file.h"

namespace FileSys::EdenPack {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::array<char, 8> Magic{'E', 'D', 'E', 'N', 'P', 'K', '0', '1'};
constexpr u32 Version = 1;
constexpr u64 HeaderSize = 40;
constexpr u64 Alignment = 4096;
constexpr u64 ProgressStep = 64ULL * 1024 * 1024;
constexpr size_t MaxChunkFiles = 4096;
constexpr size_t CopyBufferSize = 1024 * 1024;

struct Item {
    std::string path;
    fs::path source;
    bool directory{};
};

struct Packed {
    std::string path;
    u64 offset{};
    u64 size{};
    std::string sha256;
};

class PackError : public std::runtime_error {
public:
    PackError(PackResult result_, const std::string& message)
        : std::runtime_error(message), result{result_} {}
    PackResult result;
};

u64 Align(u64 value) {
    return (value + Alignment - 1) / Alignment * Alignment;
}

std::string ToUtf8(const fs::path& path) {
    const auto u8 = path.generic_u8string();
    return {u8.begin(), u8.end()};
}

std::string Lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

fs::path WithSuffix(fs::path path, std::string_view suffix) {
    path += suffix;
    return path;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// Dumps use both exefs and ExeFS spellings, so children are matched case-insensitively.
fs::path FindDirectory(const fs::path& parent, std::string_view name) {
    std::error_code ec;
    if (parent.empty() || !fs::is_directory(parent, ec)) {
        return {};
    }
    const auto wanted = Lower(std::string{name});
    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (entry.is_directory(ec) && Lower(ToUtf8(entry.path().filename())) == wanted) {
            return entry.path();
        }
    }
    return {};
}

std::optional<u64> ParseVersion(const std::string& name) {
    u64 value{};
    const auto* end = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(name.data(), end, value);
    if (name.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

// A resumed low-storage run may already have deleted exefs/main, so it only needs the folders.
std::optional<DumpSource> ResolveSource(const fs::path& root, bool require_main) {
    const auto has_main = [&](const fs::path& exefs) {
        return !require_main || IsFile(exefs / "main");
    };

    fs::path exefs;
    std::optional<u64> best_version;
    if (const auto updates = FindDirectory(root, "updates"); !updates.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(updates, ec)) {
            const auto version = ParseVersion(ToUtf8(entry.path().filename()));
            const auto candidate = FindDirectory(entry.path(), "exefs");
            if (!version || candidate.empty() || !has_main(candidate)) {
                continue;
            }
            if (!best_version || *version > *best_version) {
                best_version = version;
                exefs = candidate;
            }
        }
    }

    const auto base = FindDirectory(root, "base");
    if (exefs.empty()) {
        exefs = FindDirectory(base, "exefs");
    }
    if (exefs.empty()) {
        exefs = FindDirectory(root, "exefs");
    }
    if (exefs.empty() || !has_main(exefs)) {
        return std::nullopt;
    }
    if (require_main && !IsFile(exefs / "main.npdm")) {
        return std::nullopt;
    }

    auto romfs = FindDirectory(exefs.parent_path(), "romfs");
    if (romfs.empty()) {
        romfs = FindDirectory(base, "romfs");
    }
    if (romfs.empty()) {
        return std::nullopt;
    }
    return DumpSource{exefs, romfs};
}

void CollectTree(const fs::path& root, const std::string& prefix, std::vector<Item>& items) {
    items.push_back({prefix, root, true});
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it) {
        const auto& entry = *it;
        if (entry.is_symlink()) {
            throw PackError(PackResult::InvalidSource,
                            "symlinks are not supported: " + ToUtf8(entry.path()));
        }
        const bool directory = entry.is_directory();
        if (!directory && !entry.is_regular_file()) {
            throw PackError(PackResult::InvalidSource,
                            "unsupported file type: " + ToUtf8(entry.path()));
        }
        items.push_back(
            {prefix + "/" + ToUtf8(fs::relative(entry.path(), root)), entry.path(), directory});
    }
}

// Same order as the Python packer: by the UTF-8 bytes of the path.
void SortItems(std::vector<Item>& items) {
    std::ranges::sort(items, [](const Item& a, const Item& b) {
        return std::lexicographical_compare(a.path.begin(), a.path.end(), b.path.begin(),
                                            b.path.end(),
                                            [](unsigned char x, unsigned char y) { return x < y; });
    });
}

class Sha256 {
public:
    Sha256() : ctx{EVP_MD_CTX_new()} {
        if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
            throw PackError(PackResult::IoError, "cannot initialise SHA-256");
        }
    }
    ~Sha256() {
        EVP_MD_CTX_free(ctx);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void Update(const u8* data, size_t size) {
        EVP_DigestUpdate(ctx, data, size);
    }

    std::string HexDigest() {
        std::array<u8, 32> digest{};
        unsigned int length{};
        EVP_DigestFinal_ex(ctx, digest.data(), &length);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(digest.size() * 2);
        for (const u8 byte : digest) {
            out += hex[byte >> 4];
            out += hex[byte & 0xF];
        }
        return out;
    }

private:
    EVP_MD_CTX* ctx;
};

// Stops at the first line that does not parse. Only the tail can be torn by a crash, since the
// journal is synced before the files it lists are deleted.
std::vector<Packed> LoadJournal(const fs::path& journal) {
    std::vector<Packed> packed;
    const auto contents = Common::FS::ReadStringFromFile(journal, Common::FS::FileType::BinaryFile);
    size_t start = 0;
    while (start < contents.size()) {
        const auto end = contents.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        const auto line = std::string_view{contents}.substr(start, end - start);
        start = end + 1;
        try {
            const auto json = Json::parse(line);
            packed.push_back({json.at("path").get<std::string>(), json.at("offset").get<u64>(),
                              json.at("size").get<u64>(), json.at("sha256").get<std::string>()});
        } catch (const Json::exception&) {
            break;
        }
    }
    return packed;
}

void RemoveFile(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

bool IsEmptyDirectory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && fs::is_empty(path, ec) && !ec;
}

// Removes empty directories bottom-up; anything still holding files is left alone.
void RemoveEmptyTree(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> directories;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            directories.push_back(it->path());
        }
    }
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        if (IsEmptyDirectory(*it)) {
            fs::remove(*it, ec);
        }
    }
    if (IsEmptyDirectory(root)) {
        fs::remove(root, ec);
    }
}

// Removes now-empty parents such as updates/<v>, updates and base, stopping at the dump root.
void RemoveEmptyParents(fs::path path, const fs::path& dump_root) {
    while (!path.empty() && path.has_relative_path()) {
        std::error_code ec;
        if (fs::equivalent(path, dump_root, ec) || ec || !IsEmptyDirectory(path)) {
            break;
        }
        fs::remove(path, ec);
        path = path.parent_path();
    }
}

class Packer {
public:
    Packer(const fs::path& dump_root_, const fs::path& archive_, const PackOptions& options_,
           const std::function<void(const PackProgress&)>& progress_,
           const std::atomic_bool* cancel_)
        : dump_root{dump_root_}, archive{archive_}, part{WithSuffix(archive_, ".part")},
          journal{WithSuffix(archive_, ".journal")}, options{options_}, progress_cb{progress_},
          cancel{cancel_} {}

    PackResult Run() {
        std::error_code ec;
        if (fs::exists(archive, ec)) {
            throw PackError(PackResult::IoError, "archive already exists: " + ToUtf8(archive));
        }
        const bool resume = options.delete_sources && HasResumableJournal(archive);
        const auto source = ResolveSource(dump_root, !resume);
        if (!source) {
            throw PackError(PackResult::InvalidSource,
                            "expected exefs/main, exefs/main.npdm and a romfs folder");
        }

        std::vector<Item> items;
        CollectTree(source->exefs, "ExeFS", items);
        CollectTree(source->romfs, "RomFS", items);
        if (resume) {
            OpenForResume(items);
        } else {
            OpenNew();
        }
        SortItems(items);
        CountTotals(items);
        Report();

        for (const auto& item : items) {
            if (item.directory || packed.contains(item.path)) {
                continue;
            }
            PackFile(item);
            if (options.delete_sources &&
                (chunk_bytes >= options.chunk_bytes || chunk.size() >= MaxChunkFiles)) {
                Checkpoint();
            }
        }
        if (options.delete_sources) {
            Checkpoint();
        }

        WriteIndex(items);
        out.Close();
        fs::rename(part, archive, ec);
        if (ec) {
            throw PackError(PackResult::IoError, "cannot rename archive: " + ec.message());
        }

        if (options.delete_sources) {
            RemoveFile(journal);
            for (const auto& tree : {source->exefs, source->romfs}) {
                RemoveEmptyTree(tree);
                RemoveEmptyParents(tree.parent_path(), dump_root);
            }
        }
        return PackResult::Success;
    }

    void Abort() {
        out.Close();
        if (!options.delete_sources) {
            RemoveFile(part);
        }
    }

private:
    void OpenNew() {
        RemoveFile(journal);
        out.Open(part, Common::FS::FileAccessMode::Write);
        if (!out.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot create " + ToUtf8(part));
        }
        WriteZeros(HeaderSize);
    }

    void OpenForResume(std::vector<Item>& items) {
        const auto entries = LoadJournal(journal);
        out.Open(part, Common::FS::FileAccessMode::ReadWrite);
        if (!out.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot open " + ToUtf8(part));
        }

        const u64 end = entries.empty() ? HeaderSize : entries.back().offset + entries.back().size;
        if (out.GetSize() < end) {
            throw PackError(PackResult::IoError, "partial archive is shorter than its journal; "
                                                 "delete " +
                                                     ToUtf8(part) + " and " + ToUtf8(journal) +
                                                     " to start over");
        }
        if (!out.SetSize(end) || !out.Seek(static_cast<s64>(end))) {
            throw PackError(PackResult::IoError, "cannot truncate " + ToUtf8(part));
        }

        std::unordered_map<std::string, fs::path> present;
        for (const auto& item : items) {
            if (!item.directory) {
                present.emplace(item.path, item.source);
            }
        }
        for (const auto& entry : entries) {
            if (const auto it = present.find(entry.path); it != present.end()) {
                // Journaled before an interruption, but the delete never happened.
                RemoveFile(it->second);
            } else {
                items.push_back({entry.path, {}, false});
            }
            packed.emplace(entry.path, entry);
        }
    }

    void CountTotals(const std::vector<Item>& items) {
        for (const auto& item : items) {
            if (item.directory) {
                continue;
            }
            ++progress.files_total;
            if (const auto it = packed.find(item.path); it != packed.end()) {
                progress.bytes_total += it->second.size;
                progress.bytes_done += it->second.size;
                ++progress.files_done;
            } else {
                std::error_code ec;
                progress.bytes_total += fs::file_size(item.source, ec);
            }
        }
    }

    void WriteZeros(u64 count) {
        static constexpr std::array<u8, Alignment> zeros{};
        while (count > 0) {
            const auto n = static_cast<size_t>(std::min<u64>(count, zeros.size()));
            if (out.WriteSpan(std::span{zeros.data(), n}) != n) {
                throw PackError(PackResult::IoError, "write failed");
            }
            count -= n;
        }
    }

    bool Cancelled() const {
        return cancel && cancel->load();
    }

    void PackFile(const Item& item) {
        const auto position = static_cast<u64>(out.Tell());
        const auto offset = Align(position);
        WriteZeros(offset - position);

        Common::FS::IOFile in{item.source, Common::FS::FileAccessMode::Read};
        if (!in.IsOpen()) {
            throw PackError(PackResult::IoError, "cannot read " + ToUtf8(item.source));
        }
        buffer.resize(CopyBufferSize);
        Sha256 hash;
        u64 size = 0;
        while (true) {
            if (Cancelled()) {
                throw PackError(PackResult::Cancelled, "cancelled");
            }
            const auto read = in.ReadSpan(std::span{buffer});
            if (read == 0) {
                break;
            }
            if (out.WriteSpan(std::span<const u8>{buffer.data(), read}) != read) {
                throw PackError(PackResult::IoError, "write failed, the disk may be full");
            }
            hash.Update(buffer.data(), read);
            size += read;
            progress.bytes_done += read;
            if (progress.bytes_done - last_report >= ProgressStep) {
                Report();
            }
        }

        Packed entry{item.path, offset, size, hash.HexDigest()};
        packed.emplace(entry.path, entry);
        chunk.emplace_back(std::move(entry), item.source);
        chunk_bytes += size;
        ++progress.files_done;
        Report();
    }

    // Archive data first, then the journal, then the deletes, so an interruption loses nothing.
    void Checkpoint() {
        if (chunk.empty()) {
            return;
        }
        if (!out.Commit()) {
            throw PackError(PackResult::IoError, "cannot sync " + ToUtf8(part));
        }
        std::string lines;
        for (const auto& [entry, source] : chunk) {
            lines += Json{{"offset", entry.offset},
                          {"path", entry.path},
                          {"sha256", entry.sha256},
                          {"size", entry.size}}
                         .dump();
            lines += '\n';
        }
        {
            Common::FS::IOFile log{journal, Common::FS::FileAccessMode::Append};
            if (!log.IsOpen() || log.WriteString(lines) != lines.size() || !log.Commit()) {
                throw PackError(PackResult::IoError, "cannot write " + ToUtf8(journal));
            }
        }
        for (const auto& [entry, source] : chunk) {
            RemoveFile(source);
        }
        chunk.clear();
        chunk_bytes = 0;
    }

    void WriteIndex(const std::vector<Item>& items) {
        Json entries = Json::array();
        for (const auto& item : items) {
            if (item.directory) {
                entries.push_back(Json{{"path", item.path}, {"type", "directory"}});
                continue;
            }
            const auto& entry = packed.at(item.path);
            entries.push_back(Json{{"offset", entry.offset},
                                   {"path", entry.path},
                                   {"sha256", entry.sha256},
                                   {"size", entry.size},
                                   {"type", "file"}});
        }
        const Json index{{"alignment", Alignment},
                         {"entries", entries},
                         {"format", "EdenPack"},
                         {"version", Version}};
        const auto payload = index.dump(-1, ' ', false, Json::error_handler_t::strict);

        const auto position = static_cast<u64>(out.Tell());
        const auto index_offset = Align(position);
        WriteZeros(index_offset - position);
        if (out.WriteString(payload) != payload.size()) {
            throw PackError(PackResult::IoError, "cannot write index");
        }

        std::array<u8, HeaderSize> header{};
        std::memcpy(header.data(), Magic.data(), Magic.size());
        const auto put = [&header](size_t at, u64 value, size_t bytes) {
            for (size_t i = 0; i < bytes; ++i) {
                header[at + i] = static_cast<u8>(value >> (8 * i));
            }
        };
        put(8, Version, 4);
        put(12, 0, 4);
        put(16, index_offset, 8);
        put(24, payload.size(), 8);
        put(32, entries.size(), 8);
        if (!out.Seek(0) || out.WriteSpan(std::span<const u8>{header}) != header.size() ||
            !out.Commit()) {
            throw PackError(PackResult::IoError, "cannot finish " + ToUtf8(part));
        }
    }

    void Report() {
        last_report = progress.bytes_done;
        if (progress_cb) {
            progress_cb(progress);
        }
    }

    fs::path dump_root;
    fs::path archive;
    fs::path part;
    fs::path journal;
    PackOptions options;
    std::function<void(const PackProgress&)> progress_cb;
    const std::atomic_bool* cancel;

    Common::FS::IOFile out;
    std::unordered_map<std::string, Packed> packed;
    std::vector<std::pair<Packed, fs::path>> chunk;
    u64 chunk_bytes = 0;
    std::vector<u8> buffer;
    PackProgress progress{};
    u64 last_report = 0;
};

} // namespace

std::optional<DumpSource> ResolveDumpSource(const fs::path& dump_root) {
    return ResolveSource(dump_root, true);
}

bool IsLooseDump(const fs::path& dir) {
    return ResolveDumpSource(dir).has_value();
}

std::optional<fs::path> DumpRootFromMain(const fs::path& main) {
    const auto exefs = main.parent_path();
    if (Lower(ToUtf8(exefs.filename())) != "exefs") {
        return std::nullopt;
    }
    auto root = exefs.parent_path();
    const auto name = Lower(ToUtf8(root.filename()));
    if (name == "base") {
        root = root.parent_path();
    } else if (ParseVersion(name) && Lower(ToUtf8(root.parent_path().filename())) == "updates") {
        root = root.parent_path().parent_path();
    }
    if (!IsLooseDump(root)) {
        return std::nullopt;
    }
    return root;
}

fs::path DefaultArchivePath(const fs::path& dump_root) {
    auto root = dump_root;
    if (!root.has_filename()) {
        root = root.parent_path();
    }
    return root.parent_path() / (root.filename().u8string() + u8".edenpack");
}

bool HasResumableJournal(const fs::path& archive) {
    std::error_code ec;
    return fs::exists(WithSuffix(archive, ".part"), ec) &&
           fs::exists(WithSuffix(archive, ".journal"), ec);
}

PackResult PackDump(const fs::path& dump_root, const fs::path& archive, const PackOptions& options,
                    const std::function<void(const PackProgress&)>& progress,
                    const std::atomic_bool* cancel, std::string* error_out) {
    Packer packer{dump_root, archive, options, progress, cancel};
    try {
        return packer.Run();
    } catch (const PackError& e) {
        packer.Abort();
        if (error_out) {
            *error_out = e.what();
        }
        return e.result;
    } catch (const std::exception& e) {
        packer.Abort();
        if (error_out) {
            *error_out = e.what();
        }
        return PackResult::IoError;
    }
}

} // namespace FileSys::EdenPack
