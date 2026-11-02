// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <mutex>
#include <numeric>
#include <unordered_map>

#include <Hypervisor/Hypervisor.h>
#include <mach/mach_time.h>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/arm/hypervisor/arm_hypervisor.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/arm/nce/interpreter_visitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u64 PageMask = 0xFFF;

// Exception classes (ESR_ELx.EC).
constexpr u32 EcUnknown = 0x00;
constexpr u32 EcWfx = 0x01;
constexpr u32 EcSvc64 = 0x15;
constexpr u32 EcHvc64 = 0x16;
constexpr u32 EcSysReg = 0x18;
constexpr u32 EcInstructionAbortLower = 0x20;
constexpr u32 EcDataAbortLower = 0x24;
constexpr u32 EcBrk64 = 0x3C;

// Vector table slots (VBAR_EL1 offset / 0x80).
constexpr u64 VectorLowerSync = 8;

constexpr u64 CpsrEl1hMasked = 0x3C5; // EL1h, D, A, I and F masked
constexpr u64 SpsrEl0Masked = 0x3C0;  // EL0t, D, A, I and F masked
constexpr u64 NzcvMask = 0xF000'0000;

u32 ExceptionClass(u64 esr) {
    return static_cast<u32>((esr >> 26) & 0x3F);
}

u64 HostDczid() {
    static const u64 dczid = [] {
        u64 value;
        asm volatile("mrs %0, dczid_el0" : "=r"(value));
        return value;
    }();
    return dczid;
}

// Accesses the guest makes to pages it may not touch: the first few of each page are logged.
bool ReportFaultingAccess(u64 page) {
    static std::mutex mutex;
    static std::unordered_map<u64, u32> counts;
    static u64 total{};
    std::scoped_lock lk{mutex};
    ++total;
    if (counts.size() >= 256 && !counts.contains(page)) {
        return total % 65536 == 0;
    }
    return counts[page]++ < 4;
}

/// mach_continuous_time() - mach_absolute_time(): the time the host has slept since it booted.
/// steady_clock (and so Core::Timing) counts it; mach_absolute_time, which the guest's CNTVCT
/// follows, does not.
u64 HostSleepTicks() {
    while (true) {
        const u64 before = mach_absolute_time();
        const u64 continuous = mach_continuous_time();
        if (mach_absolute_time() == before) {
            return continuous - before; // both read the counter at the same tick
        }
    }
}

/// How host counter ticks become steady_clock nanoseconds, if the fast counter path can serve
/// guest counter reads: steady_clock must be floor(mach_continuous_time * numer / denom), which
/// is checked once against the clock itself.
struct HostTimebase {
    bool usable;
    u64 numer;
    u64 denom;
};
const HostTimebase& GetHostTimebase() {
    static const HostTimebase timebase = [] {
        mach_timebase_info_data_t info{};
        if (mach_timebase_info(&info) != KERN_SUCCESS || info.numer == 0 || info.denom == 0) {
            return HostTimebase{};
        }
        const auto to_ns = [&](u64 ticks) {
            return (ticks / info.denom) * info.numer + ((ticks % info.denom) * info.numer) / info.denom;
        };
        int exact = 0;
        for (int i = 0; i < 20000 && exact < 1000; ++i) {
            const u64 before = mach_continuous_time();
            const auto ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
            if (mach_continuous_time() != before) {
                continue;
            }
            if (ns != to_ns(before)) {
                LOG_WARNING(Core_ARM,
                            "Hypervisor: steady_clock ({} ns) is not the host counter ({} ticks) "
                            "times {}/{}; counter reads will exit to the host",
                            ns, before, info.numer, info.denom);
                return HostTimebase{};
            }
            ++exact;
        }
        return HostTimebase{exact > 0, info.numer, info.denom};
    }();
    return timebase;
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

ArmHypervisor::ArmHypervisor(System& system, Kernel::KProcess* process, std::size_t core_index)
    : ArmInterface{true}, m_system{system}, m_process{process}, m_core_index{core_index} {
    const char* stats = std::getenv("EDEN_NX2_HYPERVISOR_STATS");
    m_stats_enabled = stats != nullptr && stats[0] == '1';
    LOG_INFO(Core_ARM, "Hypervisor: core {} runs natively{}", core_index,
             m_stats_enabled ? ", exit statistics on" : "");
}

ArmHypervisor::~ArmHypervisor() = default;

void ArmHypervisor::WriteState(Vcpu& vcpu) {
    const hv_vcpu_t id = vcpu.id;
    for (u32 i = 0; i < 31; ++i) {
        hv_vcpu_set_reg(id, static_cast<hv_reg_t>(HV_REG_X0 + i), m_ctx.x[i]);
    }
    hv_vcpu_set_reg(id, HV_REG_FPCR, m_ctx.fpcr);
    hv_vcpu_set_reg(id, HV_REG_FPSR, m_ctx.fpsr);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_SP_EL0, m_ctx.sp);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_TPIDR_EL0, m_ctx.tpidr_el0);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_TPIDRRO_EL0, m_ctx.tpidrro_el0);
    for (u32 i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value;
        std::memcpy(&value, m_ctx.v[i].data(), sizeof(value));
        hv_vcpu_set_simd_fp_reg(id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), value);
    }
}

void ArmHypervisor::ReadState(Vcpu& vcpu) {
    const hv_vcpu_t id = vcpu.id;
    for (u32 i = 0; i < 31; ++i) {
        hv_vcpu_get_reg(id, static_cast<hv_reg_t>(HV_REG_X0 + i), &m_ctx.x[i]);
    }
    u64 value{};
    hv_vcpu_get_reg(id, HV_REG_FPCR, &value);
    m_ctx.fpcr = static_cast<u32>(value);
    hv_vcpu_get_reg(id, HV_REG_FPSR, &value);
    m_ctx.fpsr = static_cast<u32>(value);
    hv_vcpu_get_sys_reg(id, HV_SYS_REG_SP_EL0, &m_ctx.sp);
    hv_vcpu_get_sys_reg(id, HV_SYS_REG_TPIDR_EL0, &m_ctx.tpidr_el0);
    for (u32 i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t q;
        hv_vcpu_get_simd_fp_reg(id, static_cast<hv_simd_fp_reg_t>(HV_SIMD_FP_REG_Q0 + i), &q);
        std::memcpy(m_ctx.v[i].data(), &q, sizeof(q));
    }
}

void ArmHypervisor::EmulateSystemRegister(Vcpu& vcpu, u64 esr) {
    const u32 iss = static_cast<u32>(esr & 0x1FF'FFFF);
    const bool is_read = (iss & 1) != 0;
    const u32 crm = (iss >> 1) & 0xF;
    const u32 rt = (iss >> 5) & 0x1F;
    const u32 crn = (iss >> 10) & 0xF;
    const u32 op1 = (iss >> 14) & 0x7;
    const u32 op2 = (iss >> 17) & 0x7;
    const u32 op0 = (iss >> 20) & 0x3;

    u64 value = 0;
    if (op0 == 3 && op1 == 3 && crn == 14 && crm == 0) {
        // CNTFRQ_EL0, CNTPCT_EL0, CNTVCT_EL0, CNTPCTSS_EL0, CNTVCTSS_EL0: the guest's counter,
        // which runs at its own frequency (19.2 MHz, 31.25 MHz for Switch 2 titles).
        value = op2 == 0 ? Common::g_wall_clock.GetGuestCNTFRQ()
                         : m_system.CoreTiming().GetClockTicks();
    } else {
        static int logged = 0;
        if (logged++ < 16) {
            LOG_WARNING(Core_ARM,
                        "Hypervisor: unhandled {} of S{}_{}_C{}_C{}_{} at {:#x}, reading zero",
                        is_read ? "read" : "write", op0, op1, crn, crm, op2, m_ctx.pc - 4);
        }
    }
    if (is_read && rt != 31) {
        hv_vcpu_set_reg(vcpu.id, static_cast<hv_reg_t>(HV_REG_X0 + rt), value);
    }
}

void ArmHypervisor::UpdateCounter(Vcpu& vcpu) {
    // Counter reads are served inside the VM when the guest clock is the host clock scaled to
    // the guest frequency, exactly as Core::Timing computes it (CoreTiming::GetClockTicks). The
    // CPU clock setting (fast_cpu_time) and sync_core_speed scale it further: then they exit to
    // the host, which applies those. Checked each time a core enters, so a change takes effect at
    // the next SVC or interrupt.
    const auto& timebase = GetHostTimebase();
    const u64 frequency = Common::g_wall_clock.GetGuestCNTFRQ();
    const bool enabled = timebase.usable && frequency != 0 &&
                         Settings::values.cpu_clock.GetValue() == Settings::CpuClock::Normal &&
                         !Settings::values.sync_core_speed.GetValue();
    u64* slot = Hv::Vm::Get().SlotHost(vcpu.slot);
    using Slot = Hv::Vm::CounterSlot;
    if (frequency != m_counter_frequency) {
        m_counter_checked = false;
        const u64 divisor = std::gcd(frequency, u64{1'000'000'000});
        slot[Slot::Frequency] = frequency;
        slot[Slot::TicksToNsNum] = timebase.numer;
        slot[Slot::TicksToNsDen] = timebase.denom == 0 ? 1 : timebase.denom;
        slot[Slot::NsToGuestNum] = frequency / divisor;
        slot[Slot::NsToGuestDen] = 1'000'000'000 / divisor;
        m_counter_frequency = frequency;
    }

    // CNTVCT = mach_absolute_time() - offset; the guest's must be mach_continuous_time(), like
    // steady_clock: the offset is minus the time the host has slept, kept up to date after a sleep.
    // (The two reads are not taken at the same tick, so a difference within a millisecond is
    // noise; a sleep moves it by much more.)
    constexpr u64 Tolerance = 24'000; // ticks: 1 ms at 24 MHz
    const u64 sleep = mach_continuous_time() - mach_absolute_time();
    if (!m_vtimer_set || sleep - m_sleep_ticks + Tolerance > 2 * Tolerance) {
        m_sleep_ticks = HostSleepTicks();
        hv_vcpu_set_vtimer_offset(vcpu.id, u64{0} - m_sleep_ticks);
        m_vtimer_set = true;
        m_counter_checked = false;
    }

    // Check once (and after a sleep) that the guest's counter is Core::Timing's: read CNTVCT in the
    // VM between two reads of the emulator's clock and scale it as the handler does.
    if (enabled && !m_counter_checked) {
        m_counter_checked = true;
        auto& timing = m_system.CoreTiming();
        const u64 before = timing.GetClockTicks();
        hv_vcpu_set_reg(vcpu.id, HV_REG_CPSR, CpsrEl1hMasked);
        hv_vcpu_set_reg(vcpu.id, HV_REG_PC, Hv::Vm::CounterProbeVa);
        u64 cntvct = 0;
        bool read = false;
        for (int attempt = 0; attempt < 8 && !read; ++attempt) {
            read = hv_vcpu_run(vcpu.id) == HV_SUCCESS &&
                   vcpu.exit->reason == HV_EXIT_REASON_EXCEPTION &&
                   (vcpu.exit->exception.syndrome & 0xFFFF) == Hv::Vm::CounterProbeHvc;
            hv_vcpu_set_reg(vcpu.id, HV_REG_PC, Hv::Vm::CounterProbeVa);
        }
        hv_vcpu_get_reg(vcpu.id, HV_REG_X0, &cntvct);
        const u64 after = timing.GetClockTicks();
        const auto scale = [](u64 value, u64 num, u64 den) {
            return (value / den) * num + ((value % den) * num) / den;
        };
        const u64 guest = scale(scale(cntvct, slot[Slot::TicksToNsNum], slot[Slot::TicksToNsDen]),
                                slot[Slot::NsToGuestNum], slot[Slot::NsToGuestDen]);
        if (!read || guest < before || guest > after) {
            LOG_WARNING(Core_ARM,
                        "Hypervisor: core {}: the guest counter would read {} between the "
                        "emulator's {} and {}; counter reads will exit to the host",
                        m_core_index, guest, before, after);
            m_counter_broken = true;
        } else if (m_core_index == 0) {
            LOG_INFO(Core_ARM,
                     "Hypervisor: counter reads served in the VM: CNTVCT * {}/{} ns * {}/{} "
                     "({} Hz), host slept {} ticks",
                     slot[Slot::TicksToNsNum], slot[Slot::TicksToNsDen],
                     slot[Slot::NsToGuestNum], slot[Slot::NsToGuestDen], frequency,
                     m_sleep_ticks);
        }
    }
    slot[Slot::Enabled] = enabled && !m_counter_broken ? 1 : 0;
}

void ArmHypervisor::StoppedInCounterHandler(Vcpu& vcpu, u64 pc, bool& resume_as_is) {
    // A kick stopped the vCPU in the EL1 handler of synchronous exceptions. If it is serving a
    // counter read it would return to guest code without exiting and the kick would be lost, so
    // roll the read back: the guest is at ELR_EL1 (the MRS, or past it once the handler moved
    // ELR_EL1 on, by which point the register is written), with x0-x5 in the slot once saved.
    // Anything else ends in HVC #8 shortly: let it run there.
    using Slot = Hv::Vm::CounterSlot;
    const u64* slot = Hv::Vm::Get().SlotHost(vcpu.slot);
    u64 esr{};
    hv_vcpu_get_sys_reg(vcpu.id, HV_SYS_REG_ESR_EL1, &esr);
    const u32 iss = static_cast<u32>(esr & 0x1FF'FFFF);
    const bool counter_read = ExceptionClass(esr) == EcSysReg && (iss & 0x1F1'FC1F) == 0x30'F801 &&
                              ((iss >> 17) & 7) <= 2;
    if (!counter_read || slot[Slot::Enabled] == 0) {
        resume_as_is = true;
        return;
    }
    u64 elr{}, spsr{};
    hv_vcpu_get_sys_reg(vcpu.id, HV_SYS_REG_ELR_EL1, &elr);
    hv_vcpu_get_sys_reg(vcpu.id, HV_SYS_REG_SPSR_EL1, &spsr);
    m_ctx.pc = elr;
    m_ctx.pstate = static_cast<u32>(spsr & NzcvMask);
    if (pc >= Hv::Vm::CounterHandlerVa + Hv::Vm::CounterSavedOffset) {
        for (u32 i = 0; i < 6; ++i) {
            hv_vcpu_set_reg(vcpu.id, static_cast<hv_reg_t>(HV_REG_X0 + i), slot[Slot::SavedX0 + i]);
        }
    }
    hv_vcpu_set_sys_reg(vcpu.id, HV_SYS_REG_SP_EL1, Hv::Vm::SlotStackTop(vcpu.slot));
}

void ArmHypervisor::InvalidatePageOnEntry(Vcpu& vcpu, u64 page) {
    if (vcpu.flush_page != ~u64{0} && vcpu.flush_page != page) {
        vcpu.flush_all = true; // two pages: not worth an entry sequence of its own
    }
    vcpu.flush_page = page;
}

ArmHypervisor::Resolution ArmHypervisor::HandleDataAbort(Vcpu& vcpu, u64 esr, u64 far, u64 elr) {
    const bool is_write = ((esr >> 6) & 1) != 0;
    const u64 page = far & ~PageMask;
    if (page == m_last_fault_page) {
        ++m_fault_repeats;
    } else {
        m_last_fault_page = page;
        m_fault_repeats = 0;
    }

    auto& vm = Hv::Vm::Get();
    const auto state = vm.Query(far);
    if (state.mapped) {
        const bool allowed = is_write ? state.writable : state.readable;
        if (allowed) {
            // The tables already allow it: a stale TLB entry. Flush and retry, unless flushing
            // has not cleared it for this instruction (an alignment fault, say).
            InvalidatePageOnEntry(vcpu, page);
            if (elr == m_last_stale_pc) {
                ++m_stale_repeats;
            } else {
                m_last_stale_pc = elr;
                m_stale_repeats = 0;
            }
            if (m_stale_repeats < 8) {
                return Resolution::Continue;
            }
        } else if (state.cached) {
            // A page the GPU caches protect: let them see the access, as fastmem's fault handler
            // does. They unprotect the page; then the access is retried. A read must say so: with
            // reactive flushing the page is read-protected, and only a read copies the GPU's data back.
            m_process->GetMemory().InvalidateNCE(page, PageMask + 1, is_write);
            InvalidatePageOnEntry(vcpu, page);
            if (m_fault_repeats > 1000) {
                LOG_ERROR(Core_ARM, "Hypervisor: page {:#x} stays protected, unprotecting it", page);
                vm.MarkCached(page, PageMask + 1, false, false);
                m_fault_repeats = 0;
            }
            return Resolution::Continue;
        }
    }

    // Not mapped for the guest, a permission it lacks, or a fault a TLB flush does not clear.
    PerformFaultingAccess(vcpu, esr, far, elr, state.mapped);
    return Resolution::Continue;
}

void ArmHypervisor::PerformFaultingAccess(Vcpu& vcpu, u64 esr, u64 far, u64 elr, bool mapped) {
    // Perform the access through Core::Memory, as Dynarmic does: unmapped bytes read as zero and
    // writes to them are dropped. The instruction then completes and the guest goes on, so a
    // bad pointer never traps it on one instruction.
    auto& memory = m_process->GetMemory();
    const bool is_write = ((esr >> 6) & 1) != 0;
    const bool report = ReportFaultingAccess(far & ~PageMask);
    m_stats.emulated++;
    if (report) {
        LOG_ERROR(Core_ARM,
                  "Hypervisor: {} at {:#x} by pc {:#x}, on a page the guest {} (esr {:#x}); "
                  "performed through emulated memory",
                  is_write ? "write" : "read", far, elr,
                  !mapped ? (memory.IsValidVirtualAddress(far) ? "has but the VM does not map"
                                                               : "has not mapped")
                          : "may not access that way",
                  esr);
    }
    // Past the first reports of a page, Core::Memory's own lines for it are not wanted either.
    bool& quiet = Core::Memory::QuietUnmappedAccesses();
    const bool was_quiet = quiet;
    quiet = !report;
    SCOPE_EXIT {
        quiet = was_quiet;
    };

    if (((esr >> 8) & 1) != 0) {
        // Cache maintenance (DC CIVAC and the like) on such an address: no data to move.
        m_ctx.pc = elr + 4;
        return;
    }
    ReadState(vcpu);
    u64 sp = m_ctx.sp;
    if (const auto next = MatchAndExecuteOneInstruction(memory, m_ctx.x, m_ctx.v, sp, elr)) {
        m_ctx.sp = sp;
        m_ctx.pc = *next;
        WriteState(vcpu);
        return;
    }
    m_ctx.pc = elr + 4;
    const u32 insn = memory.Read32(elr);
    if (PerformExclusiveOrAtomic(insn)) {
        WriteState(vcpu);
        return;
    }
    if (PerformSimdStructure(insn)) {
        WriteState(vcpu);
        return;
    }
    if ((insn & 0xFFFF'FFE0U) == 0xD50B'7420U) {
        // DC ZVA: zero the block holding the address (the guest reads the host's DCZID_EL0).
        const u32 rt = insn & 0x1F;
        const u64 block = u64{4} << (HostDczid() & 0xF);
        const u64 address = (rt == 31 ? 0 : m_ctx.x[rt]) & ~(block - 1);
        static constexpr std::array<u8, 2048> zeros{};
        memory.WriteBlock(address, zeros.data(), std::min<u64>(block, zeros.size()));
        return;
    }
    // Anything else: a single register access without writeback is described by the syndrome;
    // the rest is skipped (and counted).
    if (!PerformFromSyndrome(vcpu, esr, far)) {
        m_stats.skipped++;
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_ERROR(Core_ARM,
                      "Hypervisor: could not perform instruction {:#010x} at {:#x} (address {:#x}), "
                      "skipped it",
                      insn, elr, far);
        }
    }
}

bool ArmHypervisor::PerformExclusiveOrAtomic(u32 insn) {
    // Exclusives and atomics, performed through Core::Memory without atomicity, as Dynarmic treats
    // unmapped memory: loads read zero, stores are dropped and store-exclusives succeed (so a
    // retry loop around them ends). Operates on m_ctx.
    auto& memory = m_process->GetMemory();
    const u32 size = insn >> 30;
    const u32 rt = insn & 0x1F;
    const u32 rn = (insn >> 5) & 0x1F;
    const u32 rt2 = (insn >> 10) & 0x1F;
    const u32 rs = (insn >> 16) & 0x1F;
    const u64 address = rn == 31 ? m_ctx.sp : m_ctx.x[rn];
    const auto get = [&](u32 r) { return r == 31 ? u64{0} : m_ctx.x[r]; };
    const auto set = [&](u32 r, u64 value) {
        if (r != 31) {
            m_ctx.x[r] = value;
        }
    };
    const auto read = [&](u64 at, u32 log2) -> u64 {
        switch (log2) {
        case 0: return memory.Read8(at);
        case 1: return memory.Read16(at);
        case 2: return memory.Read32(at);
        default: return memory.Read64(at);
        }
    };
    const auto write = [&](u64 at, u32 log2, u64 value) {
        switch (log2) {
        case 0: memory.Write8(at, static_cast<u8>(value)); break;
        case 1: memory.Write16(at, static_cast<u16>(value)); break;
        case 2: memory.Write32(at, static_cast<u32>(value)); break;
        default: memory.Write64(at, value); break;
        }
    };
    const u32 bits = 8U << size;
    const u64 mask = size == 3 ? ~u64{0} : (u64{1} << bits) - 1;
    const auto sext = [&](u64 value) -> s64 {
        const u64 sign = u64{1} << (bits - 1);
        return static_cast<s64>(((value & mask) ^ sign) - sign);
    };

    if ((insn & 0x3F00'0000U) == 0x0800'0000U) {
        // Load/store exclusive and compare-and-swap: size 001000 o2 L o1 Rs o0 Rt2 Rn Rt.
        const bool o2 = ((insn >> 23) & 1) != 0;
        const bool load = ((insn >> 22) & 1) != 0;
        const bool o1 = ((insn >> 21) & 1) != 0;
        if (!o2 && !o1) { // LDXR, LDAXR, STXR, STLXR
            if (load) {
                set(rt, read(address, size));
            } else {
                write(address, size, get(rt));
                set(rs, 0);
            }
            return true;
        }
        if (!o2 && o1 && (insn >> 31) != 0) { // LDXP, LDAXP, STXP, STLXP
            const u64 step = u64{1} << size;
            if (load) {
                set(rt, read(address, size));
                set(rt2, read(address + step, size));
            } else {
                write(address, size, get(rt));
                write(address + step, size, get(rt2));
                set(rs, 0);
            }
            return true;
        }
        if (o2 && o1 && rt2 == 31) { // CAS, CASA, CASL, CASAL (all sizes)
            const u64 old = read(address, size);
            if (old == (get(rs) & mask)) {
                write(address, size, get(rt));
            }
            set(rs, old);
            return true;
        }
        if (!o2 && o1 && (insn >> 31) == 0 && rt2 == 31 && rs % 2 == 0 && rt % 2 == 0) {
            // CASP, CASPA, CASPL, CASPAL: bit 30 selects 64-bit registers.
            const u32 log2 = 2 + ((insn >> 30) & 1);
            const u64 step = u64{1} << log2;
            const u64 element_mask = log2 == 3 ? ~u64{0} : 0xFFFF'FFFFULL;
            const u64 old0 = read(address, log2);
            const u64 old1 = read(address + step, log2);
            if (old0 == (get(rs) & element_mask) && old1 == (get(rs + 1) & element_mask)) {
                write(address, log2, get(rt));
                write(address + step, log2, get(rt + 1));
            }
            set(rs, old0);
            set(rs + 1, old1);
            return true;
        }
        return false;
    }
    if ((insn & 0x3F20'0C00U) == 0x3820'0000U) {
        // Atomic memory operations and LDAPR: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt.
        const bool o3 = ((insn >> 15) & 1) != 0;
        const u32 opc = (insn >> 12) & 7;
        if (o3 && opc == 4 && rs == 31) { // LDAPR: an acquire load
            set(rt, read(address, size));
            return true;
        }
        if (o3 && opc != 0) {
            return false;
        }
        const u64 old = read(address, size) & mask;
        const u64 operand = get(rs) & mask;
        u64 result{};
        if (o3) {
            result = operand; // SWP
        } else {
            switch (opc) {
            case 0: result = old + operand; break;                                 // LDADD
            case 1: result = old & ~operand; break;                                // LDCLR
            case 2: result = old ^ operand; break;                                 // LDEOR
            case 3: result = old | operand; break;                                 // LDSET
            case 4: result = sext(old) > sext(operand) ? old : operand; break;     // LDSMAX
            case 5: result = sext(old) < sext(operand) ? old : operand; break;     // LDSMIN
            case 6: result = old > operand ? old : operand; break;                 // LDUMAX
            default: result = old < operand ? old : operand; break;                // LDUMIN
            }
        }
        write(address, size, result & mask);
        set(rt, old);
        return true;
    }
    return false;
}

bool ArmHypervisor::PerformSimdStructure(u32 insn) {
    // AdvSIMD structure loads and stores (LD1-LD4, ST1-ST4, single lanes, LDnR), with or without
    // post-index, as the architecture's pseudocode describes them. Operates on m_ctx.
    const bool multiple = (insn & 0xBFBF'0000U) == 0x0C00'0000U || (insn & 0xBFA0'0000U) == 0x0C80'0000U;
    const bool single = (insn & 0xBF9F'0000U) == 0x0D00'0000U || (insn & 0xBF80'0000U) == 0x0D80'0000U;
    if (!multiple && !single) {
        return false;
    }
    auto& memory = m_process->GetMemory();
    const bool q = ((insn >> 30) & 1) != 0;
    const bool post_index = ((insn >> 23) & 1) != 0;
    const bool load = ((insn >> 22) & 1) != 0;
    const u32 rm = (insn >> 16) & 0x1F;
    const u32 opcode = (insn >> 12) & 0xF;
    const u32 size = (insn >> 10) & 3;
    const u32 rn = (insn >> 5) & 0x1F;
    u32 t = insn & 0x1F;
    const u64 address = rn == 31 ? m_ctx.sp : m_ctx.x[rn];
    const u64 datasize = q ? 16 : 8;

    const auto element_ptr = [&](u32 reg, u64 index, u64 bytes) {
        return reinterpret_cast<u8*>(m_ctx.v[reg].data()) + index * bytes;
    };
    u64 offset = 0;
    if (multiple) {
        u32 rpt{}, selem{};
        switch (opcode) {
        case 0b0000: rpt = 1; selem = 4; break;
        case 0b0010: rpt = 4; selem = 1; break;
        case 0b0100: rpt = 1; selem = 3; break;
        case 0b0110: rpt = 3; selem = 1; break;
        case 0b0111: rpt = 1; selem = 1; break;
        case 0b1000: rpt = 1; selem = 2; break;
        case 0b1010: rpt = 2; selem = 1; break;
        default: return false;
        }
        if (size == 3 && !q && selem != 1) {
            return false; // reserved
        }
        const u64 ebytes = u64{1} << size;
        const u64 elements = datasize / ebytes;
        if (load) {
            // A 64-bit register loses its upper half.
            for (u32 r = 0; r < rpt * selem; ++r) {
                if (!q) {
                    m_ctx.v[(t + r) % 32][1] = 0;
                }
            }
        }
        for (u32 r = 0; r < rpt; ++r) {
            for (u64 e = 0; e < elements; ++e) {
                u32 tt = (t + r) % 32;
                for (u32 s = 0; s < selem; ++s) {
                    u8* element = element_ptr(tt, e, ebytes);
                    if (load) {
                        memory.ReadBlock(address + offset, element, ebytes);
                    } else {
                        memory.WriteBlock(address + offset, element, ebytes);
                    }
                    offset += ebytes;
                    tt = (tt + 1) % 32;
                }
            }
        }
    } else {
        const bool s_bit = ((insn >> 12) & 1) != 0;
        const bool r_bit = ((insn >> 21) & 1) != 0;
        const u32 selem = ((((opcode >> 1) & 1) << 1) | (r_bit ? 1 : 0)) + 1;
        u32 scale = opcode >> 2;
        u64 index{};
        bool replicate = false;
        switch (scale) {
        case 3:
            if (!load || s_bit) {
                return false;
            }
            scale = size;
            replicate = true;
            break;
        case 0:
            index = (q ? 8 : 0) | (s_bit ? 4 : 0) | size;
            break;
        case 1:
            if ((size & 1) != 0) {
                return false;
            }
            index = (q ? 4 : 0) | (s_bit ? 2 : 0) | (size >> 1);
            break;
        default: // 2
            if ((size & 2) != 0) {
                return false;
            }
            if ((size & 1) == 0) {
                index = (q ? 2 : 0) | (s_bit ? 1 : 0);
            } else {
                if (s_bit) {
                    return false;
                }
                index = q ? 1 : 0;
                scale = 3;
            }
            break;
        }
        const u64 ebytes = u64{1} << scale;
        for (u32 s = 0; s < selem; ++s) {
            if (replicate) {
                std::array<u8, 8> element{};
                memory.ReadBlock(address + offset, element.data(), ebytes);
                m_ctx.v[t] = {};
                for (u64 e = 0; e < datasize / ebytes; ++e) {
                    std::memcpy(element_ptr(t, e, ebytes), element.data(), ebytes);
                }
            } else if (load) {
                memory.ReadBlock(address + offset, element_ptr(t, index, ebytes), ebytes);
            } else {
                memory.WriteBlock(address + offset, element_ptr(t, index, ebytes), ebytes);
            }
            offset += ebytes;
            t = (t + 1) % 32;
        }
    }
    if (post_index) {
        const u64 next = address + (rm == 31 ? offset : m_ctx.x[rm]);
        if (rn == 31) {
            m_ctx.sp = next;
        } else {
            m_ctx.x[rn] = next;
        }
    }
    return true;
}

bool ArmHypervisor::PerformFromSyndrome(Vcpu& vcpu, u64 esr, u64 far) {
    if (((esr >> 24) & 1) == 0) {
        return false; // ISV clear: the syndrome does not describe the access
    }
    const u32 size_log2 = static_cast<u32>((esr >> 22) & 3);
    const bool sign_extend = ((esr >> 21) & 1) != 0;
    const u32 rt = static_cast<u32>((esr >> 16) & 0x1F);
    const bool is_64bit = ((esr >> 15) & 1) != 0;
    const bool is_write = ((esr >> 6) & 1) != 0;
    auto& memory = m_process->GetMemory();
    const auto reg = static_cast<hv_reg_t>(HV_REG_X0 + rt);
    if (is_write) {
        u64 value = 0;
        if (rt != 31) {
            hv_vcpu_get_reg(vcpu.id, reg, &value);
        }
        switch (size_log2) {
        case 0: memory.Write8(far, static_cast<u8>(value)); break;
        case 1: memory.Write16(far, static_cast<u16>(value)); break;
        case 2: memory.Write32(far, static_cast<u32>(value)); break;
        default: memory.Write64(far, value); break;
        }
        return true;
    }
    u64 value{};
    switch (size_log2) {
    case 0: value = memory.Read8(far); break;
    case 1: value = memory.Read16(far); break;
    case 2: value = memory.Read32(far); break;
    default: value = memory.Read64(far); break;
    }
    if (sign_extend && size_log2 < 3) {
        const u32 bits = 8U << size_log2;
        const u64 sign = 1ULL << (bits - 1);
        value = (value ^ sign) - sign;
        if (!is_64bit) {
            value &= 0xFFFF'FFFFULL;
        }
    }
    if (rt != 31) {
        hv_vcpu_set_reg(vcpu.id, reg, value);
    }
    return true;
}

ArmHypervisor::Resolution ArmHypervisor::HandleLowerSync(Vcpu& vcpu, u64 esr, u64 far, u64 elr,
                                                         u64 spsr, HaltReason& out) {
    m_ctx.pc = elr;
    m_ctx.pstate = static_cast<u32>(spsr & NzcvMask);
    switch (ExceptionClass(esr)) {
    case EcSvc64:
        // ELR already points past the SVC.
        m_svc = static_cast<u32>(esr & 0xFFFF);
        out = HaltReason::SupervisorCall;
        return Resolution::Return;
    case EcSysReg:
        m_ctx.pc = elr + 4;
        EmulateSystemRegister(vcpu, esr);
        return Resolution::Continue;
    case EcWfx:
        m_ctx.pc = elr + 4;
        return Resolution::Continue;
    case EcDataAbortLower:
        return HandleDataAbort(vcpu, esr, far, elr);
    case EcInstructionAbortLower: {
        const auto state = Hv::Vm::Get().Query(far);
        if (state.mapped && state.executable) {
            InvalidatePageOnEntry(vcpu, far & ~PageMask);
            return Resolution::Continue;
        }
        LOG_CRITICAL(Core_ARM, "Hypervisor: instruction fetch fault at {:#x} (esr {:#x})", far,
                     esr);
        out = HaltReason::PrefetchAbort;
        return Resolution::Return;
    }
    case EcBrk64:
        out = HaltReason::InstructionBreakpoint;
        return Resolution::Return;
    case EcUnknown:
        LOG_CRITICAL(Core_ARM, "Hypervisor: undefined instruction {:#010x} at {:#x}",
                     m_process->GetMemory().Read32(elr), elr);
        out = HaltReason::PrefetchAbort;
        return Resolution::Return;
    default:
        LOG_CRITICAL(Core_ARM, "Hypervisor: unhandled exception class {:#x} at {:#x} (esr {:#x}, "
                     "far {:#x})", ExceptionClass(esr), elr, esr, far);
        out = HaltReason::PrefetchAbort;
        return Resolution::Return;
    }
}

void ArmHypervisor::CountAndReport(u64 run_ns) {
    m_stats.native_ns += run_ns;
    const s64 now = NowNs();
    if (m_stats.window_start_ns == 0) {
        m_stats.window_start_ns = now;
        return;
    }
    const s64 window = now - m_stats.window_start_ns;
    if (window < 10'000'000'000LL) {
        return;
    }
    const double seconds = static_cast<double>(window) / 1e9;
    u64 served = 0;
    if (Hv::Vcpu* vcpu = Hv::Vm::Get().CurrentVcpu(); vcpu != nullptr && vcpu->slot != ~0U) {
        u64* slot = Hv::Vm::Get().SlotHost(vcpu->slot);
        served = slot[Hv::Vm::CounterSlot::Served];
        slot[Hv::Vm::CounterSlot::Served] = 0;
    }
    const auto per_s = [seconds](u64 n) { return static_cast<u64>(static_cast<double>(n) / seconds); };
    const u64 exits = m_stats.svc + m_stats.counter + m_stats.data_abort + m_stats.interrupt +
                      m_stats.cancel + m_stats.vtimer + m_stats.other;
    LOG_INFO(Core_ARM,
             "Hypervisor core {}: native {:.0f}% of the time, {} exits/s (SVC {}, counter {}, "
             "data abort {}, interrupt {}, kick {}, vtimer {}, other {}), {} runs/s, "
             "{} emulated accesses/s ({} skipped), {} counter reads/s served in the VM",
             m_core_index, 100.0 * static_cast<double>(m_stats.native_ns) / static_cast<double>(window),
             per_s(exits), per_s(m_stats.svc), per_s(m_stats.counter), per_s(m_stats.data_abort),
             per_s(m_stats.interrupt), per_s(m_stats.cancel), per_s(m_stats.vtimer),
             per_s(m_stats.other), per_s(m_stats.runs), per_s(m_stats.emulated),
             per_s(m_stats.skipped), per_s(served));
    if (m_core_index == 0) {
        const auto inv = Hv::Vm::Get().TakeInvalidationStats();
        LOG_INFO(Core_ARM,
                 "Hypervisor VM: {} TLB invalidations/s ({} pages/s one by one, {} whole TLB, {} "
                 "through kicks)",
                 per_s(inv.calls), per_s(inv.pages), per_s(inv.full), per_s(inv.fallback));
    }
    m_stats = Stats{};
    m_stats.window_start_ns = now;
}

HaltReason ArmHypervisor::RunThread(Kernel::KThread* thread) {
    if (m_break.exchange(false)) {
        return HaltReason::BreakLoop;
    }
    if (m_stats_enabled) {
        m_stats.runs++;
    }

    auto& vm = Hv::Vm::Get();
    Hv::Vcpu* const current = vm.CurrentVcpu();
    if (current == nullptr || !vm.ClaimSlot(*current)) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: core {} has no vCPU", m_core_index);
        return HaltReason::PrefetchAbort;
    }
    Vcpu& vcpu = *current;
    const hv_vcpu_t id = vcpu.id;
    UpdateCounter(vcpu); // before WriteState: it may run a probe on the vCPU
    WriteState(vcpu);

    vm.SetCoreVcpu(m_core_index, id);
    m_running_vcpu.store(id, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_seq_cst);
    SCOPE_EXIT {
        m_running.store(false, std::memory_order_release);
    };
    if (m_break.exchange(false)) {
        return HaltReason::BreakLoop;
    }

    bool resume_as_is = false;
    while (true) {
        if (!resume_as_is) {
            if (m_break.exchange(false)) {
                ReadState(vcpu);
                return HaltReason::BreakLoop;
            }
            // Enter through the EL1 stub, which invalidates what this vCPU must not use any more:
            // a page whose fault said its TLB entry was stale, or everything when a thread
            // without a vCPU changed mappings (TlbGeneration) or code changed (CodeGeneration).
            // In guest code from here on, for InvalidateTranslations' fallback: the generations
            // are read after this is published.
            vm.EnterGuest(m_core_index);
            const u64 tlb = vm.TlbGeneration();
            const u64 code = vm.CodeGeneration();
            u64 entry = Hv::Vm::EntryPlainVa;
            if (vcpu.flush_all || vcpu.seen_code != code) {
                entry = Hv::Vm::EntryFlushVa;
            } else if (vcpu.seen_tlb != tlb) {
                entry = Hv::Vm::EntryTlbVa;
            } else if (vcpu.flush_page != ~u64{0}) {
                entry = Hv::Vm::EntryPageVa;
                hv_vcpu_set_sys_reg(id, HV_SYS_REG_TPIDR_EL1, vcpu.flush_page >> 12);
            }
            vcpu.seen_tlb = tlb;
            vcpu.seen_code = code;
            vcpu.flush_all = false;
            vcpu.flush_page = ~u64{0};
            hv_vcpu_set_sys_reg(id, HV_SYS_REG_ELR_EL1, m_ctx.pc);
            hv_vcpu_set_sys_reg(id, HV_SYS_REG_SPSR_EL1, (m_ctx.pstate & NzcvMask) | SpsrEl0Masked);
            hv_vcpu_set_reg(id, HV_REG_CPSR, CpsrEl1hMasked);
            hv_vcpu_set_reg(id, HV_REG_PC, entry);
        } else {
            vm.EnterGuest(m_core_index);
        }
        resume_as_is = false;

        const s64 run_start = m_stats_enabled ? NowNs() : 0;
        const hv_return_t run_result = hv_vcpu_run(id);
        vm.LeaveGuest(m_core_index);
        if (m_stats_enabled) {
            CountAndReport(static_cast<u64>(NowNs() - run_start));
        }
        if (run_result != HV_SUCCESS) {
            LOG_CRITICAL(Core_ARM, "Hypervisor: hv_vcpu_run failed");
            ReadState(vcpu);
            return HaltReason::PrefetchAbort;
        }

        const hv_vcpu_exit_t& exit = *vcpu.exit;
        switch (exit.reason) {
        case HV_EXIT_REASON_CANCELED: {
            m_stats.cancel++;
            u64 cpsr{}, pc{};
            hv_vcpu_get_reg(id, HV_REG_CPSR, &cpsr);
            hv_vcpu_get_reg(id, HV_REG_PC, &pc);
            if ((cpsr & 0xF) == 0) {
                // Stopped in guest code.
                m_ctx.pc = pc;
                m_ctx.pstate = static_cast<u32>(cpsr & NzcvMask);
            } else if (pc == Hv::Vm::LowerSyncVectorVa ||
                       (pc >= Hv::Vm::CounterHandlerVa && pc < vm.CounterHandlerEndVa())) {
                StoppedInCounterHandler(vcpu, pc, resume_as_is);
            } else if (pc < Hv::Vm::EntryFlushVa) {
                // Stopped in a vector before its HVC: let it deliver the exception.
                resume_as_is = true;
            } else {
                // Stopped in an entry sequence: enter again (m_ctx still holds the target), and
                // invalidate everything, as the invalidation it was doing may not have happened.
                // The page entry keeps x0 on the EL1 stack between its first and last load.
                if (pc >= Hv::Vm::EntryPageVa + 4 && pc <= Hv::Vm::EntryPageVa + 12) {
                    const u64* slot = vm.SlotHost(vcpu.slot);
                    hv_vcpu_set_reg(id, HV_REG_X0, slot[Hv::Vm::SlotSize / 8 - 2]);
                }
                hv_vcpu_set_sys_reg(id, HV_SYS_REG_SP_EL1, Hv::Vm::SlotStackTop(vcpu.slot));
                vcpu.flush_all = true;
            }
            continue;
        }
        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            m_stats.vtimer++;
            hv_vcpu_set_vtimer_mask(id, true);
            resume_as_is = true;
            continue;
        case HV_EXIT_REASON_EXCEPTION:
            break;
        default:
            LOG_CRITICAL(Core_ARM, "Hypervisor: unexpected exit reason {}",
                         static_cast<u32>(exit.reason));
            ReadState(vcpu);
            return HaltReason::PrefetchAbort;
        }

        const u64 syndrome = exit.exception.syndrome;
        if (ExceptionClass(syndrome) != EcHvc64) {
            LOG_CRITICAL(Core_ARM,
                         "Hypervisor: exception to the host, syndrome {:#x}, va {:#x}, ipa {:#x}",
                         syndrome, exit.exception.virtual_address, exit.exception.physical_address);
            ReadState(vcpu);
            return HaltReason::PrefetchAbort;
        }

        u64 esr{}, far{}, elr{}, spsr{};
        hv_vcpu_get_sys_reg(id, HV_SYS_REG_ESR_EL1, &esr);
        hv_vcpu_get_sys_reg(id, HV_SYS_REG_FAR_EL1, &far);
        hv_vcpu_get_sys_reg(id, HV_SYS_REG_ELR_EL1, &elr);
        hv_vcpu_get_sys_reg(id, HV_SYS_REG_SPSR_EL1, &spsr);

        const u64 vector = syndrome & 0xFFFF;
        if (vector == VectorLowerSync) {
            switch (ExceptionClass(esr)) {
            case EcSvc64:
                m_stats.svc++;
                break;
            case EcSysReg:
                m_stats.counter++;
                break;
            case EcDataAbortLower:
                m_stats.data_abort++;
                break;
            default:
                m_stats.other++;
                break;
            }
            HaltReason reason{};
            if (HandleLowerSync(vcpu, esr, far, elr, spsr, reason) == Resolution::Return) {
                ReadState(vcpu);
                return reason;
            }
            continue;
        }
        if (vector > VectorLowerSync) {
            // An interrupt or SError from guest code: nothing to deliver, resume it.
            m_stats.interrupt++;
            m_ctx.pc = elr;
            m_ctx.pstate = static_cast<u32>(spsr & NzcvMask);
            continue;
        }

        // An exception in the stub itself.
        LOG_CRITICAL(Core_ARM, "Hypervisor: EL1 exception (vector {}), esr {:#x}, elr {:#x}, far {:#x}",
                     vector, esr, elr, far);
        ReadState(vcpu);
        return HaltReason::PrefetchAbort;
    }
}

HaltReason ArmHypervisor::StepThread(Kernel::KThread* thread) {
    return HaltReason::StepThread;
}

void ArmHypervisor::ClearInstructionCache() {
    Hv::Vm::Get().InvalidateAllCode();
}

void ArmHypervisor::InvalidateCacheRange(u64 addr, std::size_t size) {
    Hv::Vm::Get().SyncInstructionCache(addr, size);
}

void ArmHypervisor::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = m_ctx.x[i];
    }
    ctx.fp = m_ctx.x[29];
    ctx.lr = m_ctx.x[30];
    ctx.sp = m_ctx.sp;
    ctx.pc = m_ctx.pc;
    ctx.pstate = m_ctx.pstate;
    ctx.v = m_ctx.v;
    ctx.fpcr = m_ctx.fpcr;
    ctx.fpsr = m_ctx.fpsr;
    ctx.tpidr = m_ctx.tpidr_el0;
}

void ArmHypervisor::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; ++i) {
        m_ctx.x[i] = ctx.r[i];
    }
    m_ctx.x[29] = ctx.fp;
    m_ctx.x[30] = ctx.lr;
    m_ctx.sp = ctx.sp;
    m_ctx.pc = ctx.pc;
    m_ctx.pstate = ctx.pstate;
    m_ctx.v = ctx.v;
    m_ctx.fpcr = ctx.fpcr;
    m_ctx.fpsr = ctx.fpsr;
    m_ctx.tpidr_el0 = ctx.tpidr;
}

void ArmHypervisor::SetTpidrroEl0(u64 value) {
    m_ctx.tpidrro_el0 = value;
}

void ArmHypervisor::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = m_ctx.x[i];
    }
}

void ArmHypervisor::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        m_ctx.x[i] = args[i];
    }
}

u32 ArmHypervisor::GetSvcNumber() const {
    return m_svc;
}

void ArmHypervisor::SignalInterrupt(Kernel::KThread* thread) {
    m_break.store(true, std::memory_order_seq_cst);
    if (m_running.load(std::memory_order_seq_cst)) {
        hv_vcpu_t id = static_cast<hv_vcpu_t>(m_running_vcpu.load(std::memory_order_relaxed));
        hv_vcpus_exit(&id, 1);
    }
}

} // namespace Core
