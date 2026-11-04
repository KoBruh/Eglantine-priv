// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/net/net.h"

namespace {

using Common::Net::Asset;
using Common::Net::Release;

Release ReleaseWithTag(std::string tag) {
    Release release{};
    release.tag = std::move(tag);
    return release;
}

} // namespace

TEST_CASE("ParseBuildTag reads the tags the release workflow publishes", "[common][net]") {
    const auto plain = Common::Net::ParseBuildTag("build-123");
    REQUIRE(plain.has_value());
    CHECK(plain->channel.empty());
    CHECK(plain->number == 123u);

    const auto channel = Common::Net::ParseBuildTag("nx2-build-7");
    REQUIRE(channel.has_value());
    CHECK(channel->channel == "nx2");
    CHECK(channel->number == 7u);

    for (const std::string_view garbage :
         {"", "build-", "build-12a", "build-+5", "build- 5", "build-5 ", "-build-5", "Build-5",
          "v1.0.0", "nightly", "build-99999999999999999999999"}) {
        INFO(garbage);
        CHECK_FALSE(Common::Net::ParseBuildTag(garbage).has_value());
    }
}

TEST_CASE("FindNewerRelease offers the newest build of the same channel", "[common][net]") {
    const std::vector<Release> releases{
        ReleaseWithTag("build-120"),    ReleaseWithTag("build-125"), ReleaseWithTag("build-123"),
        ReleaseWithTag("nx2-build-999"), ReleaseWithTag("not-a-build"), ReleaseWithTag("build-124"),
    };

    const auto newer = Common::Net::FindNewerRelease(releases, {"", 123});
    REQUIRE(newer.has_value());
    CHECK(newer->tag == "build-125");

    // The newest build itself, and a build newer than every release, are up to date.
    CHECK_FALSE(Common::Net::FindNewerRelease(releases, {"", 125}).has_value());
    CHECK_FALSE(Common::Net::FindNewerRelease(releases, {"", 130}).has_value());

    const auto other_channel = Common::Net::FindNewerRelease(releases, {"nx2", 5});
    REQUIRE(other_channel.has_value());
    CHECK(other_channel->tag == "nx2-build-999");
}

TEST_CASE("Release::ListFromJson keeps releases whose fields are null", "[common][net]") {
    // GitHub sends null for a release without a name or notes, and for an asset without a digest.
    constexpr std::string_view json = R"([
        {"tag_name": "build-200", "name": null, "body": null, "published_at": null,
         "html_url": null, "id": 42, "prerelease": true, "target_commitish": "abc123",
         "assets": [
            {"name": "eglantine-windows-x86_64.zip", "browser_download_url": "https://example/zip",
             "digest": "sha256:0123456789ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef"},
            {"name": "eglantine-linux-x86_64.AppImage", "browser_download_url": null,
             "digest": null},
            {"name": "eglantine-linux-x86_64.tar.gz", "digest": "sha512:00"}
         ]},
        {"tag_name": "build-199", "name": "Eglantine build 199", "body": "notes\r\nline",
         "id": 41, "prerelease": null}
    ])";

    const auto releases = Release::ListFromJson(json);
    REQUIRE(releases.has_value());
    REQUIRE(releases->size() == 2u);

    const Release& first = (*releases)[0];
    CHECK(first.tag == "build-200");
    CHECK(first.title == "build-200");
    CHECK(first.body == "build-200");
    CHECK(first.published == 0u);
    CHECK(first.html_url.ends_with("/releases/tag/build-200"));
    CHECK(first.id == 42u);
    CHECK(first.prerelease);
    CHECK(first.commit == "abc123");
    REQUIRE(first.assets.size() == 3u);
    CHECK(first.assets[0].url == "https://example/zip");
    CHECK(first.assets[0].sha256 ==
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    CHECK(first.assets[1].url.empty());
    CHECK(first.assets[1].sha256.empty());
    CHECK(first.assets[2].sha256.empty());

    const Release& second = (*releases)[1];
    CHECK(second.title == "Eglantine build 199");
    CHECK(second.body == "notes\nline");
    CHECK_FALSE(second.prerelease);
}

TEST_CASE("Release::ListFromJson fails on a response that is not a release list",
          "[common][net]") {
    const auto parses = [](std::string_view body) {
        return Release::ListFromJson(body).has_value();
    };
    CHECK_FALSE(parses("<html>Service unavailable</html>"));
    CHECK_FALSE(parses(R"({"message": "API rate limit exceeded"})"));
    CHECK_FALSE(parses(R"([{"tag_name": "build-1")"));

    const auto empty = Release::ListFromJson(std::string_view{"[]"});
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("FindPlatformAsset picks the package of the build's platform", "[common][net]") {
    const std::vector<Asset> assets{
        {.name = "eglantine-windows-x86_64.zip", .url = "w", .sha256 = {}},
        {.name = "eglantine-linux-x86_64.AppImage", .url = "xa", .sha256 = {}},
        {.name = "eglantine-linux-x86_64.tar.gz", .url = "xt", .sha256 = {}},
        {.name = "eglantine-linux-aarch64.AppImage", .url = "aa", .sha256 = {}},
        {.name = "eglantine-linux-aarch64.tar.gz", .url = "at", .sha256 = {}},
    };
    constexpr std::array<std::string_view, 1> zip{".zip"};
    constexpr std::array<std::string_view, 2> tarball_first{".tar.gz", ".AppImage"};
    constexpr std::array<std::string_view, 2> appimage_first{".AppImage", ".tar.gz"};
    constexpr std::array<std::string_view, 1> tarball_only{".tar.gz"};

    const auto url_of = [&assets](std::string_view platform,
                                  std::span<const std::string_view> extensions) {
        const auto asset = Common::Net::FindPlatformAsset(assets, platform, extensions);
        return asset ? asset->url : std::string{"none"};
    };

    CHECK(url_of("windows-x86_64", zip) == "w");
    CHECK(url_of("linux-x86_64", tarball_first) == "xt");
    CHECK(url_of("linux-x86_64", appimage_first) == "xa");
    CHECK(url_of("linux-aarch64", tarball_only) == "at");
    CHECK(url_of("linux-aarch64", appimage_first) == "aa");
    CHECK(url_of("windows-arm64", zip) == "none");
    CHECK(url_of("", zip) == "none");
}
