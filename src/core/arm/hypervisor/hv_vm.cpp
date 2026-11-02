// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/hypervisor/hv_vm.h"
#include "core/device_memory.h"

namespace Core::Hv {

namespace {

constexpr u64 PageSize = 0x1000;
constexpr u64 PageMask = PageSize - 1;
constexpr u64 HostPageSize = 0x4000;

// IPA layout: emulated DRAM at its physical addresses, the table pool above it.
constexpr u64 PoolIpa = 0x8'0000'0000ULL; // 32 GiB
constexpr size_t PoolSize = 512ULL << 20;

// Descriptor fields (4 KiB granule).
constexpr u64 DescValid = 0b11;          // table (levels 0-2) or page (level 3)
constexpr u64 DescAddressMask = 0x0000'FFFF'FFFF'F000ULL;
constexpr u64 DescApShift = 6;
constexpr u64 DescShInner = 0b11ULL << 8;
constexpr u64 DescAf = 1ULL << 10;
constexpr u64 DescPxn = 1ULL << 53;
constexpr u64 DescUxn = 1ULL << 54;
// Software bits (ignored by the MMU): the kernel's permissions and the GPU-cache protections, so
// the effective permissions can be recomputed when either changes.
constexpr u64 SwRead = 1ULL << 55;
constexpr u64 SwWrite = 1ULL << 56;
constexpr u64 SwExecute = 1ULL << 57;
constexpr u64 SwCachedNoWrite = 1ULL << 58;
constexpr u64 SwCachedNoRead = 1ULL << 59;
constexpr u64 SwMask = SwRead | SwWrite | SwExecute | SwCachedNoWrite | SwCachedNoRead;

u64 SoftwareFor(Common::MemoryPermission perms) {
    u64 sw = 0;
    if (True(perms & Common::MemoryPermission::Read)) sw |= SwRead;
    if (True(perms & Common::MemoryPermission::Write)) sw |= SwWrite;
    if (True(perms & Common::MemoryPermission::Execute)) sw |= SwExecute;
    return sw;
}

// Exception stub instructions.
constexpr u32 InsnHvc(u32 imm) {
    return 0xD4000002U | (imm << 5);
}
constexpr u32 InsnBranchSelf = 0x14000000U;

// Above this many pages a range is invalidated with one TLBI for the whole VM.
constexpr u64 MaxPagesByAddress = 64;

/// AP rank of a level 3 descriptor: 0 no EL0 access (or invalid), 1 read-only, 2 read/write.
u32 AccessRank(u64 descriptor) {
    if ((descriptor & DescValid) != DescValid) {
        return 0;
    }
    switch ((descriptor >> DescApShift) & 0b11) {
    case 0b01: return 2;
    case 0b11: return 1;
    default: return 0;
    }
}

/// Whether replacing `old` with `value` can leave a TLB entry that allows more than the tables.
bool LosesAccess(u64 old, u64 value) {
    if ((old & DescValid) != DescValid) {
        return false; // translation faults are not cached
    }
    if ((value & DescValid) != DescValid ||
        (old & DescAddressMask) != (value & DescAddressMask)) {
        return true;
    }
    return AccessRank(value) < AccessRank(old) || ((value & DescUxn) && !(old & DescUxn));
}

} // namespace

// The EL1 code the stub page carries besides its vectors. Each piece is copied to its place in
// the page; they are position independent.
extern "C" char eden_hv_entry_flush[], eden_hv_entry_flush_end[];
extern "C" char eden_hv_entry_plain[], eden_hv_entry_plain_end[];
extern "C" char eden_hv_entry_page[], eden_hv_entry_page_end[];
extern "C" char eden_hv_entry_tlb[], eden_hv_entry_tlb_end[];
extern "C" char eden_hv_invalidate[], eden_hv_invalidate_end[];
extern "C" char eden_hv_counter[], eden_hv_counter_saved[], eden_hv_counter_end[];
extern "C" char eden_hv_counter_probe[], eden_hv_counter_probe_end[];
asm(R"(
.text
.p2align 2
; Whole TLB and instruction caches, then into guest code.
.global _eden_hv_entry_flush
.global _eden_hv_entry_flush_end
_eden_hv_entry_flush:
    tlbi vmalle1is
    dsb  ish
    ic   ialluis
    dsb  ish
    isb
    eret
_eden_hv_entry_flush_end:

.global _eden_hv_entry_plain
.global _eden_hv_entry_plain_end
_eden_hv_entry_plain:
    eret
_eden_hv_entry_plain_end:

; One page's TLB entries (TPIDR_EL1 = its address >> 12). x0 is saved on the EL1 stack.
.global _eden_hv_entry_page
.global _eden_hv_entry_page_end
_eden_hv_entry_page:
    str  x0, [sp, #-16]!
    mrs  x0, tpidr_el1
    tlbi vaale1is, x0
    ldr  x0, [sp], #16
    dsb  ish
    isb
    eret
_eden_hv_entry_page_end:

.global _eden_hv_entry_tlb
.global _eden_hv_entry_tlb_end
_eden_hv_entry_tlb:
    tlbi vmalle1is
    dsb  ish
    isb
    eret
_eden_hv_entry_tlb_end:

; x0 = first page (address >> 12), x1 = pages (0: the whole TLB), x2 != 0: instruction caches too.
; The broadcast reaches every vCPU of the VM; the DSB waits until all of them have done it.
.global _eden_hv_invalidate
.global _eden_hv_invalidate_end
_eden_hv_invalidate:
    dsb  ishst
    cbz  x1, 2f
1:  tlbi vaale1is, x0
    add  x0, x0, #1
    subs x1, x1, #1
    b.ne 1b
    b    3f
2:  tlbi vmalle1is
3:  dsb  ish
    cbz  x2, 4f
    ic   ialluis
    dsb  ish
4:  isb
    hvc  #0x20
_eden_hv_invalidate_end:

; The counter as guest code sees it, for a check from the host (x0).
.global _eden_hv_counter_probe
.global _eden_hv_counter_probe_end
_eden_hv_counter_probe:
    isb
    mrs  x0, cntvct_el0
    hvc  #0x21
_eden_hv_counter_probe_end:

; Synchronous exceptions from guest code (the lower-EL vector branches here). Counter reads
; (MRS of CNTFRQ_EL0, CNTPCT_EL0 or CNTVCT_EL0, which CNTKCTL_EL1 traps) are served here without
; leaving the VM; anything else goes to the host as before (HVC #8). SP_EL1 is the top of the
; vCPU's slot, which holds the parameters (see Vm::CounterSlot):
;   value = floor(floor(CNTVCT * a / b) * c / d)
; CNTVCT runs at the host frequency on mach_continuous_time's base (the vtimer offset sees to
; that), a/b turns its ticks into nanoseconds as steady_clock does, c/d is the guest frequency over
; 1e9: exactly what Core::Timing returns (GetClockTicks: MultiplyAndDivide64(ns, freq, 1e9)).
; Each step is split (x = q*b + r) so nothing overflows. Only x0-x5 are used, saved on the slot's
; stack; NZCV changes are undone by the ERET (SPSR_EL1).
.global _eden_hv_counter
.global _eden_hv_counter_saved
.global _eden_hv_counter_end
_eden_hv_counter:
    stp  x0, x1, [sp, #-48]!
    stp  x2, x3, [sp, #16]
    stp  x4, x5, [sp, #32]
_eden_hv_counter_saved:
    mrs  x0, esr_el1
    ubfx x1, x0, #26, #6
    cmp  x1, #0x18                  ; MSR/MRS trap
    b.ne 9f
    mov  x1, #0xFC1F                ; ISS without Rt and Op2
    movk x1, #0x1F1, lsl #16
    and  x1, x0, x1
    mov  x2, #0xF801                ; MRS S3_3_C14_C0_x
    movk x2, #0x30, lsl #16
    cmp  x1, x2
    b.ne 9f
    ldur x1, [sp, #-80]             ; enabled
    cbz  x1, 9f
    ubfx x4, x0, #5, #5             ; Rt
    ubfx x3, x0, #17, #3            ; Op2: 0 CNTFRQ, 1 CNTPCT, 2 CNTVCT
    cbz  x3, 2f
    cmp  x3, #2
    b.hi 9f
    mrs  x0, cntvct_el0
    ldp  x1, x2, [sp, #-64]         ; a, b
    udiv x3, x0, x2
    msub x0, x3, x2, x0
    mul  x0, x0, x1
    udiv x0, x0, x2
    madd x0, x3, x1, x0             ; nanoseconds
    ldp  x1, x2, [sp, #-48]         ; c, d
    udiv x3, x0, x2
    msub x0, x3, x2, x0
    mul  x0, x0, x1
    udiv x0, x0, x2
    madd x0, x3, x1, x0             ; guest ticks
    b    3f
2:  ldur x0, [sp, #-72]             ; guest frequency
3:  ldur x1, [sp, #-32]             ; reads served
    add  x1, x1, #1
    stur x1, [sp, #-32]
    cmp  x4, #31                    ; XZR
    b.eq 5f
    cmp  x4, #5
    b.hi 4f
    str  x0, [sp, x4, lsl #3]       ; x0-x5: into the saved copy
    b    5f
4:  adr  x1, 6f
    add  x1, x1, x4, lsl #3
    br   x1
5:  mrs  x1, elr_el1
    add  x1, x1, #4
    msr  elr_el1, x1
    ldp  x4, x5, [sp, #32]
    ldp  x2, x3, [sp, #16]
    ldp  x0, x1, [sp], #48
    eret
6:
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    mov  x\n, x0
    b    5b
    .endr
9:  ldp  x4, x5, [sp, #32]
    ldp  x2, x3, [sp, #16]
    ldp  x0, x1, [sp], #48
    hvc  #8
_eden_hv_counter_end:
)");

namespace {
void PlaceStubCode(u8* stub, u64 va, const char* begin, const char* end, u64 room) {
    const size_t size = static_cast<size_t>(end - begin);
    ASSERT_MSG(size <= room, "Hypervisor stub code does not fit");
    std::memcpy(stub + (va - Vm::StubVa), begin, size);
}

u32 IpsFor(u32 bits) {
    if (bits <= 32) return 0;
    if (bits <= 36) return 1;
    if (bits <= 40) return 2;
    if (bits <= 42) return 3;
    if (bits <= 44) return 4;
    return 5;
}

} // namespace

namespace {
/// EDEN_NX2_HYPERVISOR: -1 when unset, else 0 or 1.
int EnvironmentOverride() {
    static const int forced = [] {
        const char* value = std::getenv("EDEN_NX2_HYPERVISOR");
        if (value == nullptr || value[0] == '\0') {
            return -1;
        }
        return value[0] != '0' ? 1 : 0;
    }();
    return forced;
}

bool IsSelected() {
    return Settings::values.cpu_backend.GetValue() == Settings::CpuBackend::Hypervisor;
}
} // namespace

bool IsRequested() {
    const int forced = EnvironmentOverride();
    return forced >= 0 ? forced == 1 : IsSelected();
}

bool IsTurnedOffByEnvironment() {
    return EnvironmentOverride() == 0 && IsSelected();
}

Vm::Vm() = default;

Vm& Vm::Get() {
    static Vm vm;
    return vm;
}

bool Vm::CreateVm() {
    if (vm_created) {
        return true;
    }
    int hv_support = 0;
    size_t hv_support_size = sizeof(hv_support);
    if (sysctlbyname("kern.hv_support", &hv_support, &hv_support_size, nullptr, 0) != 0 ||
        hv_support == 0) {
        unavailable_reason = "this Mac does not offer Hypervisor.framework (kern.hv_support is 0)";
        return false;
    }
    hv_vm_config_t config = hv_vm_config_create();
    uint32_t max_ipa = 0;
    if (hv_vm_config_get_max_ipa_size(&max_ipa) == HV_SUCCESS && max_ipa >= 36) {
        ipa_bits = std::min<uint32_t>(max_ipa, 40);
        hv_vm_config_set_ipa_size(config, ipa_bits);
    }
    const hv_return_t result = hv_vm_create(config);
    os_release(config);
    if (result != HV_SUCCESS) {
        unavailable_reason = fmt::format(
            "hv_vm_create failed ({:#x}): the binary needs the com.apple.security.hypervisor "
            "entitlement",
            static_cast<u32>(result));
        return false;
    }

    pool = static_cast<u8*>(
        mmap(nullptr, PoolSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (pool == MAP_FAILED) {
        pool = nullptr;
        unavailable_reason = "the page table pool could not be allocated";
        return false;
    }
    if (hv_vm_map(pool, PoolIpa, PoolSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) !=
        HV_SUCCESS) {
        unavailable_reason = "the page table pool could not be mapped into the VM";
        return false;
    }
    vm_created = true;
    LOG_INFO(Core_ARM, "Hypervisor: VM created, {}-bit guest physical addresses", ipa_bits);
    return true;
}

bool Vm::Attach(Common::PageTable& table, size_t address_space_bits, u8* backing,
                size_t backing_size) {
    std::scoped_lock lk{mutex};
    ready.store(false, std::memory_order_release);
    attached_table = nullptr;
    unavailable_reason.clear();

    if (address_space_bits > 48 || address_space_bits < 32) {
        unavailable_reason = fmt::format("unsupported {}-bit address space", address_space_bits);
        return false;
    }
    if (!CreateVm()) {
        return false;
    }

    // Guest DRAM.
    backing_size = Common::AlignDown(backing_size, HostPageSize);
    if (mapped_backing != backing || mapped_backing_size != backing_size) {
        if (mapped_backing != nullptr) {
            hv_vm_unmap(DramMemoryMap::Base, mapped_backing_size);
            mapped_backing = nullptr;
        }
        const hv_return_t result =
            hv_vm_map(backing, DramMemoryMap::Base, backing_size,
                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        if (result != HV_SUCCESS) {
            unavailable_reason = fmt::format("mapping {:#x} bytes of guest DRAM failed ({:#x})",
                                             backing_size, static_cast<u32>(result));
            return false;
        }
        mapped_backing = backing;
        mapped_backing_size = backing_size;
    }

    // Fresh tables: page 0 is the stub, then the TTBR0 root and the TTBR1 tables, then the vCPU
    // slots.
    std::memset(pool, 0, pool_used);
    pool_used = 0;
    const u64 stub_ipa = AllocateTable();
    ttbr0_ipa = AllocateTable();
    ttbr1_ipa = AllocateTable();
    as_bits = address_space_bits;

    // TTBR1 (T1SZ = 25, starting at level 1) maps only the top of the address space, for EL1:
    // the stub page (read/execute) and below it the vCPU slots (read/write).
    {
        u64* l1 = HostPointer(ttbr1_ipa);
        const u64 l2_ipa = AllocateTable();
        const u64 l3_ipa = AllocateTable();
        l1[511] = l2_ipa | DescValid;
        HostPointer(l2_ipa)[511] = l3_ipa | DescValid;
        u64* l3 = HostPointer(l3_ipa);
        l3[511] = (stub_ipa & DescAddressMask) | DescValid | (0b10ULL << DescApShift) |
                  DescShInner | DescAf | DescUxn;
        constexpr u64 data_pages = SlotSize * SlotCount / PageSize;
        static_assert(DataVa % PageSize == 0);
        for (u64 i = 0; i < data_pages; ++i) {
            const u64 page_ipa = AllocateTable();
            if (i == 0) {
                data_ipa = page_ipa;
            }
            l3[511 - data_pages + i] = (page_ipa & DescAddressMask) | DescValid | DescShInner |
                                       DescAf | DescPxn | DescUxn;
        }
    }

    // The stub: vectors that hand every exception to the host, then the entry sequences.
    u8* stub_bytes = reinterpret_cast<u8*>(HostPointer(stub_ipa));
    u32* stub = reinterpret_cast<u32*>(stub_bytes);
    for (u32 vector = 0; vector < 16; ++vector) {
        stub[vector * 0x20] = InsnHvc(vector);
        stub[vector * 0x20 + 1] = InsnBranchSelf;
    }
    PlaceStubCode(stub_bytes, EntryFlushVa, eden_hv_entry_flush, eden_hv_entry_flush_end, 0x20);
    PlaceStubCode(stub_bytes, EntryPlainVa, eden_hv_entry_plain, eden_hv_entry_plain_end, 0x20);
    PlaceStubCode(stub_bytes, EntryPageVa, eden_hv_entry_page, eden_hv_entry_page_end, 0x20);
    PlaceStubCode(stub_bytes, EntryTlbVa, eden_hv_entry_tlb, eden_hv_entry_tlb_end, 0x20);
    PlaceStubCode(stub_bytes, InvalidateStubVa, eden_hv_invalidate, eden_hv_invalidate_end, 0x80);
    PlaceStubCode(stub_bytes, CounterProbeVa, eden_hv_counter_probe, eden_hv_counter_probe_end,
                  CounterHandlerVa - CounterProbeVa);
    // Synchronous exceptions from guest code go through the counter handler.
    PlaceStubCode(stub_bytes, CounterHandlerVa, eden_hv_counter, eden_hv_counter_end,
                  StubVa + PageSize - CounterHandlerVa);
    static_assert(CounterSavedOffset == 12);
    ASSERT(eden_hv_counter_saved - eden_hv_counter == CounterSavedOffset);
    counter_end_va = CounterHandlerVa + static_cast<u64>(eden_hv_counter_end - eden_hv_counter);
    stub[LowerSyncVectorVa / 4 - StubVa / 4] =
        0x14000000U | static_cast<u32>((CounterHandlerVa - LowerSyncVectorVa) / 4); // B

    sys_icache_invalidate(stub, PageSize);

    attached_table = &table;
    tlb_generation.fetch_add(1, std::memory_order_acq_rel);
    code_generation.fetch_add(1, std::memory_order_acq_rel);
    ready.store(true, std::memory_order_release);
    LOG_INFO(Core_ARM, "Hypervisor: attached a {}-bit address space, {:#x} bytes of DRAM",
             address_space_bits, backing_size);
    return true;
}

void Vm::Detach(std::string reason) {
    std::scoped_lock lk{mutex};
    ready.store(false, std::memory_order_release);
    attached_table = nullptr;
    unavailable_reason = std::move(reason);
}

std::string Vm::UnavailableReason() const {
    std::scoped_lock lk{mutex};
    return unavailable_reason;
}

u64 Vm::Tcr() const {
    const u64 t0sz = 64 - as_bits;
    const u64 t1sz = 25;
    u64 tcr = 0;
    tcr |= t0sz;                 // T0SZ
    tcr |= 0b01ULL << 8;         // IRGN0: write-back, write-allocate
    tcr |= 0b01ULL << 10;        // ORGN0
    tcr |= 0b11ULL << 12;        // SH0: inner shareable
    tcr |= 0b00ULL << 14;        // TG0: 4 KiB
    tcr |= t1sz << 16;           // T1SZ
    tcr |= 0b01ULL << 24;        // IRGN1
    tcr |= 0b01ULL << 26;        // ORGN1
    tcr |= 0b11ULL << 28;        // SH1
    tcr |= 0b10ULL << 30;        // TG1: 4 KiB
    tcr |= u64{IpsFor(ipa_bits)} << 32; // IPS
    return tcr;
}

u64* Vm::HostPointer(u64 ipa) const {
    return reinterpret_cast<u64*>(pool + (ipa - PoolIpa));
}

u64 Vm::AllocateTable() {
    if (pool_used + PageSize > PoolSize) {
        LOG_CRITICAL(Core_ARM, "Hypervisor: out of page table memory");
        return 0;
    }
    const u64 ipa = PoolIpa + pool_used;
    pool_used += PageSize;
    return ipa;
}

void Vm::StoreEntry(u64* entry, u64 value) {
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
}

u64* Vm::TableFor(u64 vaddr, bool create) {
    // Walk from the starting level down to the level 3 table holding vaddr's entry.
    const u32 start_level = as_bits > 39 ? 0 : (as_bits > 30 ? 1 : 2);
    u64* table = HostPointer(ttbr0_ipa);
    for (u32 level = start_level; level < 3; ++level) {
        const u32 shift = 39 - level * 9;
        u64* entry = &table[(vaddr >> shift) & 511];
        u64 value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
        if ((value & DescValid) != DescValid) {
            if (!create) {
                return nullptr;
            }
            const u64 next = AllocateTable();
            if (next == 0) {
                return nullptr;
            }
            value = next | DescValid;
            StoreEntry(entry, value);
        }
        table = HostPointer(value & DescAddressMask);
    }
    return &table[(vaddr >> 12) & 511];
}

u64 Vm::Descriptor(u64 paddr, u64 software) {
    const bool readable = (software & SwRead) && !(software & SwCachedNoRead);
    const bool writable = readable && (software & SwWrite) && !(software & SwCachedNoWrite);
    // AP: 01 = EL0 read/write, 11 = EL0 read-only, 00 = no EL0 access.
    const u64 ap = readable ? (writable ? 0b01 : 0b11) : 0b00;
    u64 value = (paddr & DescAddressMask) | DescValid | (ap << DescApShift) | DescShInner | DescAf |
                DescPxn | (software & SwMask);
    if (!(software & SwExecute)) {
        value |= DescUxn;
    }
    return value;
}

Vcpu::~Vcpu() {
    if (slot != ~0U) {
        Vm::Get().ReleaseSlot(slot);
    }
    if (created) {
        hv_vcpu_destroy(id);
    }
}

Vcpu* Vm::CurrentVcpu() {
    static thread_local Vcpu vcpu;
    if (!vcpu.created) {
        if (hv_vcpu_create(&vcpu.id, &vcpu.exit, nullptr) != HV_SUCCESS) {
            return nullptr;
        }
        vcpu.created = true;
        hv_vcpu_set_trap_debug_exceptions(vcpu.id, false);
        hv_vcpu_set_vtimer_mask(vcpu.id, true);
    }
    if (vcpu.ttbr0 != ttbr0_ipa) {
        ConfigureVcpu(vcpu);
    }
    return &vcpu;
}

void Vm::ConfigureVcpu(Vcpu& vcpu) {
    // System registers (SCTLR_EL1 bits).
    constexpr u64 SctlrM = 1ULL << 0;
    constexpr u64 SctlrC = 1ULL << 2;
    constexpr u64 SctlrI = 1ULL << 12;
    constexpr u64 SctlrDze = 1ULL << 14;  // DC ZVA at EL0
    constexpr u64 SctlrUct = 1ULL << 15;  // CTR_EL0 at EL0
    constexpr u64 SctlrNTwi = 1ULL << 16; // WFI not trapped
    constexpr u64 SctlrNTwe = 1ULL << 18; // WFE not trapped
    constexpr u64 SctlrUci = 1ULL << 26;  // cache maintenance at EL0
    constexpr u64 SctlrRes1 = (1ULL << 11) | (1ULL << 20) | (1ULL << 22) | (1ULL << 23) |
                              (1ULL << 28) | (1ULL << 29);
    const u64 sctlr = SctlrRes1 | SctlrM | SctlrC | SctlrI | SctlrDze | SctlrUct | SctlrNTwi |
                      SctlrNTwe | SctlrUci;
    const hv_vcpu_t id = vcpu.id;
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_MAIR_EL1, 0xFF);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_TCR_EL1, Tcr());
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_TTBR0_EL1, ttbr0_ipa);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_TTBR1_EL1, ttbr1_ipa);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_VBAR_EL1, StubVa);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_CPACR_EL1, 0b11ULL << 20); // FP/SIMD at EL0
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_CNTKCTL_EL1, 0);           // counter reads trap to EL1
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_MDSCR_EL1, 0);
    hv_vcpu_set_sys_reg(id, HV_SYS_REG_SCTLR_EL1, sctlr);
    if (vcpu.slot != ~0U) {
        hv_vcpu_set_sys_reg(id, HV_SYS_REG_SP_EL1, SlotStackTop(vcpu.slot));
    }
    vcpu.ttbr0 = ttbr0_ipa;
    vcpu.flush_all = true;
}

bool Vm::ClaimSlot(Vcpu& vcpu) {
    if (vcpu.slot != ~0U) {
        return true;
    }
    u64 used = slots_used.load(std::memory_order_relaxed);
    while (true) {
        if (used == ~u64{0}) {
            return false;
        }
        const u32 slot = static_cast<u32>(std::countr_one(used));
        if (slots_used.compare_exchange_weak(used, used | (u64{1} << slot),
                                             std::memory_order_acq_rel)) {
            vcpu.slot = slot;
            hv_vcpu_set_sys_reg(vcpu.id, HV_SYS_REG_SP_EL1, SlotStackTop(slot));
            return true;
        }
    }
}

void Vm::ReleaseSlot(u32 slot) {
    slots_used.fetch_and(~(u64{1} << slot), std::memory_order_acq_rel);
}

u64* Vm::SlotHost(u32 slot) const {
    return HostPointer(data_ipa + SlotSize * slot);
}

void Vm::SetCoreVcpu(size_t core, hv_vcpu_t vcpu) {
    core_vcpu[core].store(vcpu, std::memory_order_release);
}

void Vm::InvalidateTranslations(u64 vaddr, u64 size, bool code) {
    const u64 first = vaddr >> 12;
    u64 pages = (Common::AlignUp(vaddr + size, PageSize) - Common::AlignDown(vaddr, PageSize)) >> 12;
    if (pages == 0 || pages > MaxPagesByAddress) {
        pages = 0; // the whole TLB
    }
    invalidations.fetch_add(1, std::memory_order_relaxed);
    invalidated_pages.fetch_add(pages, std::memory_order_relaxed);
    if (pages == 0) {
        full_invalidations.fetch_add(1, std::memory_order_relaxed);
    }
    if (Vcpu* vcpu = ready.load(std::memory_order_acquire) ? CurrentVcpu() : nullptr;
        vcpu != nullptr && RunInvalidateStub(*vcpu, first, pages, code)) {
        return;
    }
    // No vCPU on this thread: the vCPUs invalidate before they next enter guest code; make the
    // ones in guest code leave it and wait for that.
    fallback_invalidations.fetch_add(1, std::memory_order_relaxed);
    (code ? code_generation : tlb_generation).fetch_add(1, std::memory_order_seq_cst);
    KickAndWait();
}

bool Vm::RunInvalidateStub(Vcpu& vcpu, u64 first_page, u64 pages, bool code) {
    // On a core's thread the vCPU may hold guest registers (a fault being handled): keep them.
    const hv_vcpu_t id = vcpu.id;
    u64 saved[5]{};
    hv_vcpu_get_reg(id, HV_REG_X0, &saved[0]);
    hv_vcpu_get_reg(id, HV_REG_X1, &saved[1]);
    hv_vcpu_get_reg(id, HV_REG_X2, &saved[2]);
    hv_vcpu_get_reg(id, HV_REG_PC, &saved[3]);
    hv_vcpu_get_reg(id, HV_REG_CPSR, &saved[4]);
    bool done = false;
    for (int attempt = 0; attempt < 16 && !done; ++attempt) {
        // A pending kick cancels the run before (or while) it does anything: start over.
        hv_vcpu_set_reg(id, HV_REG_X0, first_page);
        hv_vcpu_set_reg(id, HV_REG_X1, pages);
        hv_vcpu_set_reg(id, HV_REG_X2, code ? 1 : 0);
        hv_vcpu_set_reg(id, HV_REG_CPSR, 0x3C5); // EL1h, interrupts masked
        hv_vcpu_set_reg(id, HV_REG_PC, InvalidateStubVa);
        if (hv_vcpu_run(id) != HV_SUCCESS) {
            break;
        }
        if (vcpu.exit->reason == HV_EXIT_REASON_EXCEPTION &&
            ((vcpu.exit->exception.syndrome >> 26) & 0x3F) == 0x16 &&
            (vcpu.exit->exception.syndrome & 0xFFFF) == InvalidateStubHvc) {
            done = true;
        } else if (vcpu.exit->reason != HV_EXIT_REASON_CANCELED &&
                   vcpu.exit->reason != HV_EXIT_REASON_VTIMER_ACTIVATED) {
            LOG_ERROR(Core_ARM, "Hypervisor: TLB invalidation stopped (exit {}, syndrome {:#x})",
                      static_cast<u32>(vcpu.exit->reason), vcpu.exit->exception.syndrome);
            break;
        }
    }
    hv_vcpu_set_reg(id, HV_REG_X0, saved[0]);
    hv_vcpu_set_reg(id, HV_REG_X1, saved[1]);
    hv_vcpu_set_reg(id, HV_REG_X2, saved[2]);
    hv_vcpu_set_reg(id, HV_REG_PC, saved[3]);
    hv_vcpu_set_reg(id, HV_REG_CPSR, saved[4]);
    return done;
}

void Vm::KickAndWait() {
    for (size_t core = 0; core < guest_epoch.size(); ++core) {
        const u64 epoch = guest_epoch[core].load(std::memory_order_seq_cst);
        if ((epoch & 1) == 0) {
            continue; // not in guest code: it invalidates before it enters
        }
        hv_vcpu_t id = static_cast<hv_vcpu_t>(core_vcpu[core].load(std::memory_order_acquire));
        hv_vcpus_exit(&id, 1);
        const auto start = std::chrono::steady_clock::now();
        auto last_kick = start;
        while (guest_epoch[core].load(std::memory_order_seq_cst) == epoch) {
            std::this_thread::yield();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_kick > std::chrono::milliseconds(1)) {
                hv_vcpus_exit(&id, 1);
                last_kick = now;
            }
            if (now - start > std::chrono::seconds(1)) {
                LOG_ERROR(Core_ARM, "Hypervisor: core {} did not leave guest code", core);
                break;
            }
        }
    }
}

Vm::InvalidationStats Vm::TakeInvalidationStats() {
    InvalidationStats stats{};
    stats.calls = invalidations.exchange(0, std::memory_order_relaxed);
    stats.pages = invalidated_pages.exchange(0, std::memory_order_relaxed);
    stats.full = full_invalidations.exchange(0, std::memory_order_relaxed);
    stats.fallback = fallback_invalidations.exchange(0, std::memory_order_relaxed);
    return stats;
}

void Vm::Map(u64 vaddr, u64 size, u64 paddr, Common::MemoryPermission perms) {
    bool lost = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        const u64 software = SoftwareFor(perms);
        for (u64 offset = 0; offset < size; offset += PageSize) {
            u64* entry = TableFor(vaddr + offset, true);
            if (entry == nullptr) {
                return;
            }
            const u64 old = *entry;
            const u64 value = Descriptor(paddr + offset, software);
            lost |= LosesAccess(old, value);
            StoreEntry(entry, value);
        }
    }
    const bool code = True(perms & Common::MemoryPermission::Execute) &&
                      paddr + size <= DramMemoryMap::Base + mapped_backing_size;
    if (code) {
        sys_icache_invalidate(mapped_backing + (paddr - DramMemoryMap::Base), size);
    }
    if (lost || code) {
        InvalidateTranslations(vaddr, size, code);
    }
}

void Vm::Unmap(u64 vaddr, u64 size) {
    bool lost = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        for (u64 offset = 0; offset < size; offset += PageSize) {
            if (u64* entry = TableFor(vaddr + offset, false)) {
                lost |= LosesAccess(*entry, 0);
                StoreEntry(entry, 0);
            }
        }
    }
    if (lost) {
        InvalidateTranslations(vaddr, size, false);
    }
}

void Vm::Protect(u64 vaddr, u64 size, Common::MemoryPermission perms) {
    bool lost = false;
    bool gained_execute = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        const u64 software = SoftwareFor(perms);
        for (u64 offset = 0; offset < size; offset += PageSize) {
            u64* entry = TableFor(vaddr + offset, false);
            if (entry == nullptr || (*entry & DescValid) != DescValid) {
                continue;
            }
            const u64 old = *entry;
            const u64 old_sw = old & (SwRead | SwWrite | SwExecute);
            const u64 new_sw = (old & (SwCachedNoWrite | SwCachedNoRead)) | software;
            const u64 value = Descriptor(old & DescAddressMask, new_sw);
            lost |= LosesAccess(old, value);
            if ((software & SwExecute) && !(old_sw & SwExecute)) {
                gained_execute = true;
            }
            StoreEntry(entry, value);
        }
    }
    if (gained_execute) {
        SyncInstructionCache(vaddr, size); // invalidates the TLBs as well
    } else if (lost) {
        InvalidateTranslations(vaddr, size, false);
    }
}

void Vm::MarkCached(u64 vaddr, u64 size, bool cached, bool block_reads) {
    const u64 begin = Common::AlignDown(vaddr, PageSize);
    const u64 end = Common::AlignUp(vaddr + size, PageSize);
    bool lost = false;
    {
        std::scoped_lock lk{mutex};
        if (!ready.load(std::memory_order_relaxed)) {
            return;
        }
        for (u64 page = begin; page < end; page += PageSize) {
            u64* entry = TableFor(page, false);
            if (entry == nullptr || (*entry & DescValid) != DescValid) {
                continue;
            }
            const u64 old = *entry;
            u64 software = old & SwMask;
            software &= ~(SwCachedNoWrite | SwCachedNoRead);
            if (cached) {
                software |= SwCachedNoWrite | (block_reads ? SwCachedNoRead : 0);
            }
            const u64 value = Descriptor(old & DescAddressMask, software);
            lost |= LosesAccess(old, value);
            StoreEntry(entry, value);
        }
    }
    // A protection only needs TLB entries gone (no code changed), and must be in force before
    // this returns: a write through a stale entry would slip past the caches.
    if (lost) {
        InvalidateTranslations(begin, end - begin, false);
    }
}

Vm::PageState Vm::Query(u64 vaddr) const {
    std::scoped_lock lk{mutex};
    PageState state{};
    if (!ready.load(std::memory_order_relaxed)) {
        return state;
    }
    u64* entry = const_cast<Vm*>(this)->TableFor(vaddr, false);
    if (entry == nullptr) {
        return state;
    }
    const u64 value = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    if ((value & DescValid) != DescValid) {
        return state;
    }
    state.mapped = true;
    state.readable = (value & SwRead) && !(value & SwCachedNoRead);
    state.writable = state.readable && (value & SwWrite) && !(value & SwCachedNoWrite);
    state.executable = (value & SwExecute) != 0;
    state.cached = (value & (SwCachedNoWrite | SwCachedNoRead)) != 0;
    state.paddr = (value & DescAddressMask) | (vaddr & PageMask);
    return state;
}

void Vm::SyncInstructionCache(u64 vaddr, u64 size) {
    const u64 begin = Common::AlignDown(vaddr, PageSize);
    const u64 end = Common::AlignUp(vaddr + size, PageSize);
    for (u64 page = begin; page < end; page += PageSize) {
        const PageState state = Query(page);
        if (!state.mapped || state.paddr < DramMemoryMap::Base ||
            state.paddr - DramMemoryMap::Base + PageSize > mapped_backing_size) {
            continue;
        }
        sys_icache_invalidate(mapped_backing + (state.paddr - DramMemoryMap::Base), PageSize);
    }
    InvalidateTranslations(begin, end - begin, true);
}

void Vm::InvalidateAllCode() {
    InvalidateTranslations(0, 0, true);
}

} // namespace Core::Hv
