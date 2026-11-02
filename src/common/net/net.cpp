// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
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

std::optional<Asset> Release::GetPlatformAsset() const {
    const std::string_view platform = Common::g_build_release_platform;
    if (platform.empty())
        return std::nullopt;

    // Release files are named eglantine-<tag>-<platform>.<extension>.
    const auto suffix = fmt::format("-{}", platform);
    for (const Asset& asset : assets) {
        const std::string_view name = asset.name;
        if (name.substr(0, name.find_last_of('.')).ends_with(suffix))
            return asset;
    }

    return std::nullopt;
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

    rel.tag = json.value("tag_name", std::string{});
    if (rel.tag.empty())
        return std::nullopt;

    rel.title = json.value("name", rel.tag);
    rel.id = json.value("id", std::hash<std::string>{}(rel.title));
    rel.commit = json.value("target_commitish", std::string{});

    rel.published = ParseIsoTimestamp(json.value("published_at", std::string{}));
    rel.prerelease = json.value("prerelease", false);

    auto body = json.value("body", rel.title);
    boost::replace_all(body, "\r", "");
    rel.body = body;

    rel.html_url = json.value(
        "html_url", fmt::format("https://github.com/{}/releases/tag/{}",
                                Common::g_build_auto_update_repo, rel.tag));

    for (const auto& obj : json.value("assets", nlohmann::json::array())) {
        rel.assets.push_back({
            .name = obj.value("name", std::string{}),
            .url = obj.value("browser_download_url", std::string{}),
        });
    }

    return rel;
}

std::vector<Release> Release::ListFromJson(const nlohmann::json& json) {
    if (!json.is_array())
        return {};

    std::vector<Release> releases;
    for (const auto& obj : json) {
        auto rel = Release::FromJson(obj);
        if (rel)
            releases.emplace_back(rel.value());
    }
    return releases;
}

std::vector<Release> Release::ListFromJson(const std::string_view& json) {
    try {
        return ListFromJson(nlohmann::json::parse(json));
    } catch (std::exception& e) {
        LOG_WARNING(Common, "Failed to parse JSON: {}", e.what());
    }

    return {};
}

std::optional<std::string> MakeRequest(const std::string& url, const std::string& path) {
    try {
        constexpr std::size_t timeout_seconds = 15;

        std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(url);
        client->set_connection_timeout(timeout_seconds);
        client->set_read_timeout(timeout_seconds);
        client->set_write_timeout(timeout_seconds);

#ifdef YUZU_BUNDLED_OPENSSL
        client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

        if (client == nullptr) {
            LOG_ERROR(Common, "Invalid URL {}{}", url, path);
            return {};
        }

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

    return Release::ListFromJson(std::string_view{body.value()});
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
            const auto message = commit.value("message", std::string{});
            commits.push_back({
                .sha = obj.value("sha", std::string{}),
                .summary = message.substr(0, message.find('\n')),
                .author = commit.at("author").value("name", std::string{}),
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
