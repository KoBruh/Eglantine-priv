// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <variant>
#include <vector>
#include "common/net/net.h"

namespace UpdateChecker {

struct UpToDate {};

struct Update {
    Common::Net::Release release;
    // Commits between this build and the release, newest first.
    std::vector<Common::Net::Commit> changes;
    // False when this build's commit was not found, so `changes` is only the newest part.
    bool changes_complete;
    std::string compare_url;
};

enum class Failure {
    // Only builds CI publishes under a release tag know which releases follow them.
    NotReleaseBuild,
    Unreachable,
};

using Result = std::variant<UpToDate, Update, Failure>;

// Look for a newer release on this build's channel.
Result Check();

} // namespace UpdateChecker
