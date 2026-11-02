// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <Hypervisor/Hypervisor.h>

#include "common/common_types.h"
#include "common/host_memory.h"
#include "core/hardware_properties.h"

namespace Common {
struct PageTable;
}

namespace Core::Hv {

/// Whether application code should run natively through Hypervisor.framework instead of being
/// translated by Dynarmic: cpu_backend = Hypervisor, the default on Apple silicon.
/// EDEN_NX2_HYPERVISOR=0 or =1 overrides the setting.
bool IsRequested();
/// EDEN_NX2_HYPERVISOR=0 turned the backend off although cpu_backend selects it.
bool IsTurnedOffByEnvironment();

/**
 * A vCPU of the VM. Hypervisor.framework binds a vCPU to the thread that creates it and lets a
 * thread create only one, so each thread that needs one gets its own (Vm::CurrentVcpu): the
 * threads of the emulated cores run guest code on theirs; any other thread uses its own only to
 * invalidate translations (Vm::InvalidateTranslations).
 */
struct Vcpu {
    hv_vcpu_t id{};
    hv_vcpu_exit_t* exit{};
    bool created{};
    u64 ttbr0{~u64{0}}; ///< The translation its registers are set up for.
    u32 slot{~0U};      ///< Its slot in the EL1 data page (Vm::ClaimSlot), for guest code.

    // What the core's thread must invalidate before it next enters guest code.
    u64 seen_tlb{};
    u64 seen_code{};
    bool flush_all{true};          ///< The whole TLB and the instruction caches.
    u64 flush_page{~u64{0}};       ///< Just this page's TLB entries.

    Vcpu() = default;
    Vcpu(const Vcpu&) = delete;
    Vcpu& operator=(const Vcpu&) = delete;
    ~Vcpu();
};

/**
 * The process-wide virtual machine that application code runs in.
 *
 * Guest physical memory (the IPA space) holds the emulated DRAM at its Switch physical addresses
 * (DramMemoryMap::Base and up), backed by the DeviceMemory buffer, and a pool of host pages for
 * the stage-1 page tables and the EL1 exception stub. The stage-1 tables use a 4 KiB granule, so
 * guest pages map one to one regardless of the host's 16 KiB pages: they mirror the emulated
 * process page table, which Core::Memory reports here as it changes.
 */
class Vm {
public:
    static Vm& Get();

    /// Attaches the application page table: (re)creates the VM if needed, maps the DRAM backing
    /// and starts an empty set of stage-1 tables. Returns false if the VM can't be used.
    bool Attach(Common::PageTable& table, size_t address_space_bits, u8* backing,
                size_t backing_size);
    /// Stops mirroring any page table: the application runs on another backend, for `reason`.
    void Detach(std::string reason);
    /// Why the last application could not use this backend (empty if it could).
    std::string UnavailableReason() const;
    bool IsAttached(const Common::PageTable& table) const {
        return attached_table == &table;
    }
    bool IsReady() const {
        return ready.load(std::memory_order_acquire);
    }

    // Mirror of the process page table. Addresses and sizes are 4 KiB aligned.
    void Map(u64 vaddr, u64 size, u64 paddr, Common::MemoryPermission perms);
    void Unmap(u64 vaddr, u64 size);
    void Protect(u64 vaddr, u64 size, Common::MemoryPermission perms);
    /// Pages the GPU caches track: writes (and reads, with reactive flushing) fault so the
    /// caches see them, as fastmem's protections do.
    void MarkCached(u64 vaddr, u64 size, bool cached, bool block_reads);

    struct PageState {
        bool mapped;
        bool readable;
        bool writable;
        bool executable;
        bool cached;
        u64 paddr;
    };
    PageState Query(u64 vaddr) const;

    /// Makes host writes to guest code visible to instruction fetches.
    void SyncInstructionCache(u64 vaddr, u64 size);
    /// Invalidates every instruction cache (and TLB) of the VM.
    void InvalidateAllCode();

    /// Invalidates the TLB entries of [vaddr, vaddr + size) on every vCPU (all of them when the
    /// range is large), and the instruction caches as well if `code`. Returns once no vCPU can use
    /// the old entries, without stopping the vCPUs: a broadcast TLBI from any vCPU of the VM
    /// reaches all of them, so it runs on the calling thread's own vCPU. Without one, the running
    /// vCPUs are kicked and this waits until each has left guest code (they invalidate on entry).
    void InvalidateTranslations(u64 vaddr, u64 size, bool code);

    /// The calling thread's vCPU, created (and set up for the attached translation) on first use.
    /// nullptr if it can't be created.
    Vcpu* CurrentVcpu();
    /// Gives a vCPU that runs guest code its EL1 data slot (EL1 stack and scratch). Returns false
    /// if none is left.
    bool ClaimSlot(Vcpu& vcpu);
    void ReleaseSlot(u32 slot);
    /// The host address of a slot (SlotSize bytes).
    u64* SlotHost(u32 slot) const;

    /// Invalidations since the last call (exit statistics).
    struct InvalidationStats {
        u64 calls;    ///< InvalidateTranslations calls
        u64 pages;    ///< pages invalidated one by one
        u64 full;     ///< calls that invalidated the whole TLB
        u64 fallback; ///< calls from threads without a vCPU (kick and wait)
    };
    InvalidationStats TakeInvalidationStats();

    // Register values for the vCPUs.
    u64 Ttbr0() const {
        return ttbr0_ipa;
    }
    u64 Ttbr1() const {
        return ttbr1_ipa;
    }
    u64 Tcr() const;
    // The EL1 stub page, at the top of the address space (TTBR1), and the entry sequences that
    // put a vCPU into guest code (ELR_EL1 and SPSR_EL1 hold the guest's PC and PSTATE).
    static constexpr u64 StubVa = 0xFFFF'FFFF'FFFF'F000ULL; ///< Vector table, then entry code.
    static constexpr u64 EntryFlushVa = StubVa + 0x800; ///< Whole TLB and I-caches, then ERET.
    static constexpr u64 EntryPlainVa = StubVa + 0x820; ///< ERET.
    static constexpr u64 EntryPageVa = StubVa + 0x840;  ///< TLB entries of page TPIDR_EL1 << 12.
    static constexpr u64 EntryTlbVa = StubVa + 0x860;   ///< Whole TLB, then ERET.
    static constexpr u64 EntryEndVa = StubVa + 0x880;
    /// x0 = first page (VA >> 12), x1 = pages (0: all), x2 = also the I-caches; ends in HVC.
    static constexpr u64 InvalidateStubVa = StubVa + 0x880;
    static constexpr u32 InvalidateStubHvc = 0x20;
    // Per-vCPU slots below the stub page: SP_EL1 starts at the top of a vCPU's slot.
    static constexpr u64 SlotSize = 128;
    static constexpr u32 SlotCount = 64;
    static constexpr u64 DataVa = StubVa - SlotSize * SlotCount;
    static u64 SlotStackTop(u32 slot) {
        return DataVa + SlotSize * (slot + 1);
    }

    /// Bumped instead of invalidating directly when a thread has no vCPU: vCPUs invalidate the
    /// TLB (or the TLB and I-caches) before running again.
    u64 TlbGeneration() const {
        return tlb_generation.load(std::memory_order_seq_cst);
    }
    u64 CodeGeneration() const {
        return code_generation.load(std::memory_order_seq_cst);
    }

    // The core threads report when their vCPU enters and leaves guest code, so the fallback in
    // InvalidateTranslations knows whom to wait for.
    void SetCoreVcpu(size_t core, hv_vcpu_t vcpu);
    void EnterGuest(size_t core) {
        guest_epoch[core].fetch_add(1, std::memory_order_seq_cst); // odd: in guest code
    }
    void LeaveGuest(size_t core) {
        guest_epoch[core].fetch_add(1, std::memory_order_seq_cst);
    }

private:
    Vm();

    bool CreateVm();
    u64* TableFor(u64 vaddr, bool create);
    u64 AllocateTable();
    u64* HostPointer(u64 ipa) const;
    void StoreEntry(u64* entry, u64 value);
    static u64 Descriptor(u64 paddr, u64 software);
    bool RunInvalidateStub(Vcpu& vcpu, u64 first_page, u64 pages, bool code);
    void KickAndWait();
    void ConfigureVcpu(Vcpu& vcpu);

    mutable std::mutex mutex;
    std::string unavailable_reason;
    std::atomic<bool> ready{};
    bool vm_created{};
    Common::PageTable* attached_table{};
    size_t as_bits{};
    u32 ipa_bits{36};

    u8* mapped_backing{};
    size_t mapped_backing_size{};

    // Page-table pool: host memory mapped at PoolIpa.
    u8* pool{};
    size_t pool_used{};
    u64 ttbr0_ipa{};
    u64 ttbr1_ipa{};
    u64 data_ipa{}; ///< SlotCount * SlotSize bytes of vCPU slots.
    std::atomic<u64> slots_used{};

    std::atomic<u64> tlb_generation{1};
    std::atomic<u64> code_generation{1};
    std::array<std::atomic<u64>, Core::Hardware::NUM_CPU_CORES> guest_epoch{};
    std::array<std::atomic<u64>, Core::Hardware::NUM_CPU_CORES> core_vcpu{};

    std::atomic<u64> invalidations{};
    std::atomic<u64> invalidated_pages{};
    std::atomic<u64> full_invalidations{};
    std::atomic<u64> fallback_invalidations{};
};

} // namespace Core::Hv
