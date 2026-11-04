// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Common::Net {

struct Asset {
    std::string name;
    std::string url;
    // Lowercase hex SHA-256 GitHub lists for the file, or empty when it lists none.
    std::string sha256;
};

struct Release {
    std::string title;
    std::string body;
    std::string tag;
    std::string html_url;
    // Commit the release was built from.
    std::string commit;

    std::vector<Asset> assets;

    u64 id;
    u64 published;
    bool prerelease;

    // The release file built for the same platform and package as this build, if it has one.
    std::optional<Asset> GetPlatformAsset() const;

    static std::optional<Release> FromJson(const nlohmann::json& json);
    // Nullopt when the response is not a list of releases (an error page, a truncated body).
    static std::optional<std::vector<Release>> ListFromJson(const nlohmann::json& json);
    static std::optional<std::vector<Release>> ListFromJson(const std::string_view& json);
};

struct Commit {
    std::string sha;
    std::string summary;
    std::string author;
};

// A release tag CI publishes builds under: build-<number>, or <channel>-build-<number>.
struct BuildTag {
    // Empty for build-<number>. Points into the parsed tag.
    std::string_view channel;
    u64 number;
};

std::optional<BuildTag> ParseBuildTag(std::string_view tag);

// The highest numbered of `releases` on `current`'s channel, if it is numbered above `current`.
std::optional<Release> FindNewerRelease(const std::vector<Release>& releases,
                                        const BuildTag& current);

// The asset named <anything>-<platform><extension>, trying `extensions` in order.
std::optional<Asset> FindPlatformAsset(const std::vector<Asset>& assets, std::string_view platform,
                                       std::span<const std::string_view> extensions);

// Make a request via httplib, and return the response body if applicable.
std::optional<std::string> MakeRequest(const std::string& url, const std::string& path);

// Get the newest releases of the update repository, or nullopt if GitHub could not be reached
// or did not answer with a list of releases.
std::optional<std::vector<Release>> GetReleases();

// Get the newest releases of the update repository as GitHub's JSON.
std::optional<std::string> GetReleasesBody();

// Get up to `limit` commits of the update repository's history at `head`, newest first.
std::optional<std::vector<Commit>> GetCommits(const std::string& head, u32 limit);

// The update repository's web page comparing two commits.
std::string GetCompareUrl(const std::string& base, const std::string& head);

}
