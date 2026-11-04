// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <boost/algorithm/string/replace.hpp>

#include <fmt/format.h>
#include "common/scm_rev.h"
#include "net.h"

#include "common/logging.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace Common::Net {

constexpr const char github_api[] = "https://api.github.com";

namespace {

// GitHub sends null for the fields a release leaves unset (no name, no notes), and json.value()
// throws on a null, so a field is only read when it is a string.
std::string StringField(const nlohmann::json& json, const char* key, std::string fallback = {}) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::move(fallback);
}

// GitHub lists the checksum of each release file as "sha256:<64 hex digits>".
std::string Sha256FromDigest(std::string_view digest) {
    constexpr std::string_view prefix = "sha256:";
    if (!digest.starts_with(prefix))
        return {};

    std::string hex{digest.substr(prefix.size())};
    const bool is_hex =
        std::ranges::all_of(hex, [](unsigned char c) { return std::isxdigit(c) != 0; });
    if (hex.size() != 64 || !is_hex)
        return {};

    std::ranges::transform(hex, hex.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hex;
}

} // namespace

std::optional<BuildTag> ParseBuildTag(std::string_view tag) {
    std::string_view channel;
    std::string_view digits;
    constexpr std::string_view channel_marker = "-build-";
    constexpr std::string_view plain_marker = "build-";
    if (const auto pos = tag.rfind(channel_marker); pos != std::string_view::npos && pos != 0) {
        channel = tag.substr(0, pos);
        digits = tag.substr(pos + channel_marker.size());
    } else if (tag.starts_with(plain_marker)) {
        digits = tag.substr(plain_marker.size());
    } else {
        return std::nullopt;
    }

    u64 number{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
        return std::nullopt;

    return BuildTag{channel, number};
}

std::optional<Release> FindNewerRelease(const std::vector<Release>& releases,
                                        const BuildTag& current) {
    const Release* latest = nullptr;
    u64 latest_number = current.number;
    for (const Release& release : releases) {
        const auto tag = ParseBuildTag(release.tag);
        if (tag && tag->channel == current.channel && tag->number > latest_number) {
            latest = &release;
            latest_number = tag->number;
        }
    }

    if (latest == nullptr)
        return std::nullopt;
    return *latest;
}

std::optional<Asset> FindPlatformAsset(const std::vector<Asset>& assets, std::string_view platform,
                                       std::span<const std::string_view> extensions) {
    if (platform.empty())
        return std::nullopt;

    for (const std::string_view extension : extensions) {
        const auto suffix = fmt::format("-{}{}", platform, extension);
        const auto it = std::ranges::find_if(
            assets, [&suffix](const Asset& asset) { return asset.name.ends_with(suffix); });
        if (it != assets.end())
            return *it;
    }

    return std::nullopt;
}

std::optional<Asset> Release::GetPlatformAsset() const {
    // Release files are named eglantine-<platform> plus the package's extension; each build
    // updates to the package it came in.
#if defined(_WIN32)
    static constexpr std::array<std::string_view, 1> extensions{".zip"};
#elif defined(__ANDROID__)
    static constexpr std::array<std::string_view, 1> extensions{".apk"};
#elif defined(__linux__)
    // The AppImage runtime sets APPIMAGE. The tar.gz build runs on the system's Qt instead, so
    // its users get the tar.gz again.
    static constexpr std::array<std::string_view, 2> appimage{".AppImage", ".tar.gz"};
    static constexpr std::array<std::string_view, 2> tarball{".tar.gz", ".AppImage"};
    const auto extensions =
        std::getenv("APPIMAGE") != nullptr ? std::span{appimage} : std::span{tarball};
#else
    static constexpr std::array<std::string_view, 2> extensions{".zip", ".tar.gz"};
#endif
    return FindPlatformAsset(assets, Common::g_build_release_platform, extensions);
}

static inline u64 ParseIsoTimestamp(const std::string& iso) {
    if (iso.empty())
        return 0;

    std::string buf = iso;
    if (buf.back() == 'Z')
        buf.pop_back();

    std::tm tm{};
    std::istringstream ss(buf);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail())
        return 0;

#ifdef _WIN32
    return static_cast<u64>(_mkgmtime(&tm));
#else
    return static_cast<u64>(timegm(&tm));
#endif
}

std::optional<Release> Release::FromJson(const nlohmann::json& json) {
    Release rel;
    if (!json.is_object())
        return std::nullopt;

    rel.tag = StringField(json, "tag_name");
    if (rel.tag.empty())
        return std::nullopt;

    rel.title = StringField(json, "name");
    if (rel.title.empty())
        rel.title = rel.tag;

    const auto id_field = json.find("id");
    rel.id = id_field != json.end() && id_field->is_number_integer()
                 ? id_field->get<u64>()
                 : static_cast<u64>(std::hash<std::string>{}(rel.title));
    rel.commit = StringField(json, "target_commitish");

    rel.published = ParseIsoTimestamp(StringField(json, "published_at"));
    const auto prerelease_field = json.find("prerelease");
    rel.prerelease = prerelease_field != json.end() && prerelease_field->is_boolean() &&
                     prerelease_field->get<bool>();

    auto body = StringField(json, "body", rel.title);
    boost::replace_all(body, "\r", "");
    rel.body = std::move(body);

    rel.html_url = StringField(json, "html_url",
                               fmt::format("https://github.com/{}/releases/tag/{}",
                                           Common::g_build_auto_update_repo, rel.tag));

    if (const auto asset_list = json.find("assets");
        asset_list != json.end() && asset_list->is_array()) {
        for (const auto& obj : *asset_list) {
            if (!obj.is_object())
                continue;
            rel.assets.push_back({
                .name = StringField(obj, "name"),
                .url = StringField(obj, "browser_download_url"),
                .sha256 = Sha256FromDigest(StringField(obj, "digest")),
            });
        }
    }

    return rel;
}

std::optional<std::vector<Release>> Release::ListFromJson(const nlohmann::json& json) {
    if (!json.is_array())
        return std::nullopt;

    std::vector<Release> releases;
    for (const auto& obj : json) {
        auto rel = Release::FromJson(obj);
        if (rel)
            releases.emplace_back(std::move(*rel));
    }
    return releases;
}

std::optional<std::vector<Release>> Release::ListFromJson(const std::string_view& json) {
    try {
        return ListFromJson(nlohmann::json::parse(json));
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return std::nullopt;
}

std::optional<std::string> MakeRequest(const std::string& url, const std::string& path) {
    try {
        constexpr std::size_t timeout_seconds = 15;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(url);
        // A URL httplib cannot parse (a bad port) leaves the client without a connection, and
        // configuring it would dereference that.
        if (!client->is_valid()) {
            LOG_ERROR(Common, "Invalid URL {}{}", url, path);
            return {};
        }

        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

        httplib::Request request{
            .method = "GET",
            .path = path,
        };

        client->set_follow_location(true);
        client->set_default_headers({{"Accept", "application/vnd.github+json"},
                                     {"User-Agent", "Eglantine"}});
        httplib::Result result = client->send(request);

        if (!result) {
            LOG_ERROR(Common, "GET to {}{} returned null", url, path);
            return {};
        }

        const auto& response = result.value();
        if (response.status >= 400) {
            LOG_ERROR(Common, "GET to {}{} returned error status code: {}", url, path,
                      response.status);
            return {};
        }
        if (!response.has_header("content-type")) {
            LOG_ERROR(Common, "GET to {}{} returned no content", url, path);
            return {};
        }

        return response.body;
    } catch (std::exception& e) {
        LOG_ERROR(Common, "GET to {}{} failed during update check: {}", url, path, e.what());
        return std::nullopt;
    }
}

std::optional<std::string> GetReleasesBody() {
    return MakeRequest(github_api,
                       fmt::format("/repos/{}/releases?per_page=50", Common::g_build_auto_update_repo));
}

std::optional<std::vector<Release>> GetReleases() {
    const auto body = GetReleasesBody();
    if (!body) {
        LOG_WARNING(Common, "Failed to get releases");
        return std::nullopt;
    }

    auto releases = Release::ListFromJson(std::string_view{body.value()});
    if (!releases)
        LOG_WARNING(Common, "GitHub did not answer with a list of releases");
    return releases;
}

std::optional<std::vector<Commit>> GetCommits(const std::string& head, u32 limit) {
    const auto body = MakeRequest(github_api, fmt::format("/repos/{}/commits?sha={}&per_page={}",
                                                         Common::g_build_auto_update_repo, head,
                                                         limit));
    if (!body) {
        LOG_WARNING(Common, "Failed to get commits of {}", head);
        return std::nullopt;
    }

    try {
        const auto json = nlohmann::json::parse(body.value());
        std::vector<Commit> commits;
        for (const auto& obj : json) {
            const auto& commit = obj.at("commit");
            const auto message = StringField(commit, "message");
            commits.push_back({
                .sha = StringField(obj, "sha"),
                .summary = message.substr(0, message.find('\n')),
                .author = StringField(commit.at("author"), "name"),
            });
        }
        return commits;
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse commits of {}: {}", head, e.what());
    }

    return std::nullopt;
}

std::string GetCompareUrl(const std::string& base, const std::string& head) {
    return fmt::format("https://github.com/{}/compare/{}...{}", Common::g_build_auto_update_repo,
                       base, head);
}

} // namespace Common::Net
