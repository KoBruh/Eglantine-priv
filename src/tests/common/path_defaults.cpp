// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"

TEST_CASE("Windows config defaults to roaming AppData without a portable profile", "[common][paths]") {
    namespace FS = Common::FS;
    const auto portable = FS::GetExeDirectory() / PORTABLE_DIR;
    if (std::filesystem::is_directory(portable)) {
        SKIP("This test executable has an adjacent portable profile");
    }
    REQUIRE(FS::GetEdenPath(FS::EdenPath::ConfigDir) ==
            FS::GetAppDataRoamingDirectory() / EDEN_DIR / CONFIG_DIR);
}
#endif
