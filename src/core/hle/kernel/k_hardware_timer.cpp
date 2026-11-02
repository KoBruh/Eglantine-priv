// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/nx2_sleep_stats.h"

namespace Kernel {

void KHardwareTimer::Initialize() {
    // Create the timing callback to register with CoreTiming.
    m_event_type = Core::Timing::CreateEvent("KHardwareTimer::Callback",
                                             [this](s64, std::chrono::nanoseconds) {
                                                 this->DoTask();
                                                 return std::nullopt;
                                             });
}

void KHardwareTimer::Finalize() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
    m_event_type.reset();
}

void KHardwareTimer::DoTask() {
    const bool stats = Nx2SleepStats::Enabled();
    const s64 entered = stats ? GetTick() : 0;
    s64 locked = 0, tasks_done = 0;
    SCOPE_EXIT {
        if (stats && locked != 0) {
            const s64 left = GetTick();
            Nx2SleepStats::TimerLock().Add(locked - entered);
            Nx2SleepStats::TimerTasks().Add(tasks_done - locked);
            Nx2SleepStats::TimerUnlock().Add(left - tasks_done);
        }
    };
    // Handle the interrupt.
    {
        KScopedSchedulerLock slk{m_kernel};
        KScopedSpinLock lk(this->GetLock());

        //! Ignore this event if needed.
        if (!this->GetInterruptEnabled()) {
            return;
        }
        if (stats) [[unlikely]] {
            locked = GetTick();
            Nx2SleepStats::Timer().Add(locked - m_wakeup_time);
        }

        // Disable the timer interrupt while we handle this.
        // Not necessary due to core timing already having popped this event to call it.
        // this->DisableInterrupt();
        m_wakeup_time = (std::numeric_limits<s64>::max)();

        const s64 next_time = this->DoInterruptTaskImpl(GetTick());
        if (stats) [[unlikely]] {
            tasks_done = GetTick();
        }
        if (0 < next_time && next_time <= m_wakeup_time) {
            // We have a next time, so we should set the time to interrupt and turn the interrupt
            // on.
            this->EnableInterrupt(next_time);
        }
    }

    // Clear the timer interrupt.
    // Kernel::GetInterruptManager().ClearInterrupt(KInterruptName_NonSecurePhysicalTimer,
    //                                              GetCurrentCoreId());
}

void KHardwareTimer::EnableInterrupt(s64 wakeup_time) {
    this->DisableInterrupt();

    m_wakeup_time = wakeup_time;
    m_kernel.System().CoreTiming().ScheduleEvent(std::chrono::nanoseconds{m_wakeup_time},
                                                 m_event_type, true);
}

void KHardwareTimer::DisableInterrupt() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type,
                                                   Core::Timing::UnscheduleEventType::NoWait);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
}

s64 KHardwareTimer::GetTick() const {
    return m_kernel.System().CoreTiming().GetGlobalTimeNs().count();
}

bool KHardwareTimer::GetInterruptEnabled() {
    return m_wakeup_time != (std::numeric_limits<s64>::max)();
}

} // namespace Kernel
