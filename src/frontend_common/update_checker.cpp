// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <optional>

#include "common/logging.h"
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "update_checker.h"

namespace UpdateChecker {

namespace {

constexpr u32 max_changes = 100;

} // namespace

Result Check() {
    // CI publishes release builds as build-<run number> (Common::Net::ParseBuildTag).
    const auto current = Common::Net::ParseBuildTag(Common::g_build_release_tag);
    if (!current)
        return Failure::NotReleaseBuild;

    const auto releases = Common::Net::GetReleases();
    if (!releases)
        return Failure::Unreachable;

    const auto latest = Common::Net::FindNewerRelease(*releases, *current);
    if (!latest)
        return UpToDate{};

    LOG_INFO(Frontend, "Update available: {}", latest->tag);

    Update update{
        .release = *latest,
        .changes = {},
        .changes_complete = false,
        .compare_url = Common::Net::GetCompareUrl(Common::g_scm_rev, latest->commit),
    };

    if (auto commits = Common::Net::GetCommits(latest->commit, max_changes)) {
        for (auto& commit : *commits) {
            if (commit.sha == Common::g_scm_rev) {
                update.changes_complete = true;
                break;
            }
            update.changes.push_back(std::move(commit));
        }
    }

    return update;
}

} // namespace UpdateChecker
