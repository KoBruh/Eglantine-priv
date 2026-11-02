// SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_types.h"

namespace Service::HID {

// Records the controller input a game reads and plays it back, for runs that repeat a session:
//
//   EDEN_NX2_INPUT_RECORD=<file>  writes each change of a controller's buttons or sticks
//   EDEN_NX2_INPUT_REPLAY=<file>  feeds a recording to the game in place of the real input
//
// A line is "<ms> <npad id> <buttons hex> <left x> <left y> <right x> <right y>", ms counted from
// the first present, so a replay lines up with the recording however long the boot took. Taken
// where the HID service samples the controller: keyboard, gamepad, any frontend, sticks analog.
// A replay plays the controller with the most changes in the file into every connected one.
class Nx2InputTape {
public:
    static void Process(u32 npad_id, Core::HID::NpadButtonState& buttons,
                        Core::HID::AnalogSticks& sticks) {
        static Nx2InputTape tape;
        tape.Apply(npad_id, buttons, sticks);
    }

private:
    struct Entry {
        s64 ms;
        u64 buttons;
        s32 lx, ly, rx, ry;
    };

    Nx2InputTape() {
        if (const char* path = std::getenv("EDEN_NX2_INPUT_REPLAY")) {
            Load(path);
        } else if (const char* record_path = std::getenv("EDEN_NX2_INPUT_RECORD")) {
            record = std::fopen(record_path, "w");
            if (record == nullptr) {
                LOG_ERROR(Service_HID, "Input tape: can't write {}", record_path);
            } else {
                LOG_INFO(Service_HID, "Input tape: recording to {}", record_path);
            }
        }
    }

    ~Nx2InputTape() {
        if (record != nullptr) {
            std::fclose(record);
        }
    }

    void Load(const char* path) {
        std::FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            LOG_ERROR(Service_HID, "Input tape: can't read {}", path);
            return;
        }
        std::map<u32, std::vector<Entry>> by_id;
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            long long ms{};
            unsigned id{};
            unsigned long long button_bits{};
            int lx{}, ly{}, rx{}, ry{};
            if (std::sscanf(line, "%lld %u %llx %d %d %d %d", &ms, &id, &button_bits, &lx, &ly,
                            &rx, &ry) == 7) {
                by_id[id].push_back({ms, button_bits, lx, ly, rx, ry});
            }
        }
        std::fclose(file);
        for (auto& [id, list] : by_id) {
            if (list.size() > replay.size()) {
                replay = std::move(list);
            }
        }
        std::ranges::stable_sort(replay, {}, &Entry::ms);
        replaying = !replay.empty();
        LOG_INFO(Service_HID, "Input tape: replaying {} changes from {} ({:.1f} s)", replay.size(),
                 path, replaying ? replay.back().ms / 1000.0 : 0.0);
    }

    // Milliseconds since the first present, or nothing before it.
    std::optional<s64> Now() {
        if (!anchor) {
            if (Core::g_nx2_present_count.load(std::memory_order_relaxed) == 0) {
                return std::nullopt;
            }
            anchor = std::chrono::steady_clock::now();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - *anchor)
            .count();
    }

    void Apply(u32 npad_id, Core::HID::NpadButtonState& buttons, Core::HID::AnalogSticks& sticks) {
        if (record == nullptr && !replaying) {
            return;
        }
        std::scoped_lock lock{mutex};
        const auto now = Now();
        if (replaying) {
            Entry state{};
            if (now) {
                while (next < replay.size() && replay[next].ms <= *now) {
                    ++next;
                }
                if (next == replay.size() && !finished) {
                    finished = true;
                    LOG_INFO(Service_HID, "Input tape: replay finished at {} ms", *now);
                }
                if (next > 0 && !finished) {
                    state = replay[next - 1];
                }
            }
            buttons.raw = static_cast<Core::HID::NpadButton>(state.buttons);
            sticks.left = {state.lx, state.ly};
            sticks.right = {state.rx, state.ry};
            return;
        }
        if (!now) {
            return;
        }
        const Entry state{*now,           static_cast<u64>(buttons.raw), sticks.left.x,
                          sticks.left.y,  sticks.right.x,                sticks.right.y};
        Entry& last = last_recorded[npad_id];
        if (last.buttons == state.buttons && last.lx == state.lx && last.ly == state.ly &&
            last.rx == state.rx && last.ry == state.ry) {
            return;
        }
        last = state;
        std::fprintf(record, "%lld %u %" PRIx64 " %d %d %d %d\n", static_cast<long long>(state.ms),
                     npad_id, state.buttons, state.lx, state.ly, state.rx, state.ry);
        std::fflush(record);
    }

    std::mutex mutex;
    std::FILE* record{};
    std::map<u32, Entry> last_recorded;
    std::vector<Entry> replay;
    size_t next{};
    bool replaying{};
    bool finished{};
    std::optional<std::chrono::steady_clock::time_point> anchor;
};

} // namespace Service::HID
