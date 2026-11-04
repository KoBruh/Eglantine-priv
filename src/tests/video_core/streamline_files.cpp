// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Built only with NVIDIA DLSS (Windows x86-64): the Streamline loader is part of video_core then.
#ifdef EGLANTINE_STREAMLINE

#include <filesystem>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/streamline.h"

namespace {

using Vulkan::Streamline::DlssFiles;
using Path = std::filesystem::path;

// FindDlssFiles' search order: EDEN_STREAMLINE_DIR, the user folder (where the settings
// download to), then the folder next to the executable.
const Path env_dir{"C:/streamline-env"};
const Path user_dir{"C:/user/streamline"};
const Path exe_dir{"C:/eglantine/streamline"};
const Path loader{"sl.interposer.dll"};
const Path model{"nvngx_dlss.dll"};

DlssFiles Locate(const std::set<Path>& existing) {
    return Vulkan::Streamline::LocateDlssFiles(
        {env_dir, user_dir, exe_dir}, user_dir,
        [&existing](const Path& path) { return existing.contains(path); });
}

} // Anonymous namespace

TEST_CASE("Streamline finds a downloaded DLSS model when the loader is elsewhere", "[streamline]") {
    // The settings download a missing nvngx_dlss.dll into the user folder even when
    // sl.interposer.dll sits next to the executable.
    const DlssFiles files = Locate({exe_dir / loader, user_dir / model});
    CHECK(files.folder == exe_dir);
    CHECK(files.interposer == exe_dir / loader);
    CHECK(files.model == user_dir / model);
}

TEST_CASE("Streamline prefers the DLSS model next to the loader", "[streamline]") {
    const DlssFiles files = Locate({exe_dir / loader, exe_dir / model, user_dir / model});
    CHECK(files.folder == exe_dir);
    CHECK(files.model == exe_dir / model);
}

TEST_CASE("Streamline takes the first loader and model in search order", "[streamline]") {
    const DlssFiles files =
        Locate({env_dir / loader, user_dir / loader, user_dir / model, exe_dir / model});
    CHECK(files.folder == env_dir);
    CHECK(files.interposer == env_dir / loader);
    CHECK(files.model == user_dir / model);
}

TEST_CASE("Streamline without a loader points at the download folder", "[streamline]") {
    const DlssFiles with_model = Locate({exe_dir / model});
    CHECK(with_model.folder == user_dir);
    CHECK(with_model.interposer.empty());
    CHECK(with_model.model == exe_dir / model);

    const DlssFiles nothing = Locate({});
    CHECK(nothing.folder == user_dir);
    CHECK(nothing.interposer.empty());
    CHECK(nothing.model.empty());
}

#endif
