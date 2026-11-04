// SPDX-FileCopyrightText: Copyright 2026 Eglantine Project
// SPDX-License-Identifier: GPL-3.0-or-later

// This file is built for the x86-64 baseline whatever -march the rest of the program uses
// (src/common/CMakeLists.txt), so it runs on the CPUs it has to turn away. It calls only the C
// runtime and the OS: an inline C++ library function used here could be merged at link time with
// a copy built for AVX2.

#include "common/x64/cpu_requirements.h"

#ifdef YUZU_REQUIRE_X86_64_V3
#include <cpuid.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

namespace Common::X64 {

#ifdef YUZU_REQUIRE_X86_64_V3
namespace {

struct Feature {
    const char* name;
    unsigned leaf;
    int reg; // 0 EBX, 1 ECX, 2 EDX
    unsigned bit;
};

// x86-64-v3 (AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, XSAVE) and the v2 level under it.
constexpr Feature features[] = {
    {"SSE3", 1, 1, 0},          {"SSSE3", 1, 1, 9},       {"FMA", 1, 1, 12},
    {"CMPXCHG16B", 1, 1, 13},   {"SSE4.1", 1, 1, 19},     {"SSE4.2", 1, 1, 20},
    {"MOVBE", 1, 1, 22},        {"POPCNT", 1, 1, 23},     {"XSAVE", 1, 1, 26},
    {"AVX", 1, 1, 28},          {"F16C", 1, 1, 29},       {"BMI1", 7, 0, 3},
    {"AVX2", 7, 0, 5},          {"BMI2", 7, 0, 8},        {"LAHF/SAHF", 0x80000001, 1, 0},
    {"LZCNT", 0x80000001, 1, 5},
};

bool HasFeature(const Feature& feature) {
    unsigned regs[4]{};
    if (__get_cpuid_max(feature.leaf & 0x80000000, nullptr) < feature.leaf) {
        return false;
    }
    __cpuid_count(feature.leaf, 0, regs[3], regs[0], regs[1], regs[2]);
    return (regs[feature.reg] >> feature.bit) & 1;
}

// The OS has to save the YMM registers on a context switch (XCR0 bits 1 and 2), or AVX
// instructions fault even on a CPU that has them (some virtual machines).
bool OsSavesAvxState() {
    unsigned eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !((ecx >> 27) & 1)) {
        return false;
    }
    unsigned xcr0_low, xcr0_high;
    __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
    return (xcr0_low & 6) == 6;
}

void Append(char* out, unsigned capacity, const char* text) {
    unsigned length = 0;
    while (length < capacity - 1 && out[length] != '\0') {
        ++length;
    }
    while (length < capacity - 1 && *text != '\0') {
        out[length++] = *text++;
    }
    out[length] = '\0';
}

[[noreturn]] void Refuse(const char* missing) {
    char message[768]{};
    Append(message, sizeof(message),
           "This build of Eglantine needs a processor with AVX2 (x86-64-v3): an Intel Core of the "
           "4th generation (Haswell, 2013) or newer, or an AMD Ryzen or newer. Some Intel "
           "Pentium, Celeron and Atom processors lack it even when they are recent.\n\n"
           "This processor is missing: ");
    Append(message, sizeof(message), missing);
    Append(message, sizeof(message), "\n");
#ifdef _WIN32
    // Both frontends are GUI programs on Windows, so there is no console to print to.
    wchar_t wide[768]{};
    MultiByteToWideChar(CP_UTF8, 0, message, -1, wide, 768);
    MessageBoxW(nullptr, wide, L"Eglantine", MB_OK | MB_ICONERROR);
    ExitProcess(1);
#else
    unsigned length = 0;
    while (message[length] != '\0') {
        ++length;
    }
    [[maybe_unused]] const auto written = write(STDERR_FILENO, message, length);
    _exit(1);
#endif
}

void Check() {
    char missing[256]{};
    for (const Feature& feature : features) {
        if (!HasFeature(feature)) {
            Append(missing, sizeof(missing), missing[0] != '\0' ? ", " : "");
            Append(missing, sizeof(missing), feature.name);
        }
    }
    if (missing[0] == '\0' && !OsSavesAvxState()) {
        Append(missing, sizeof(missing), "AVX state saving (the operating system leaves it off)");
    }
    if (missing[0] != '\0') {
        Refuse(missing);
    }
}

// Priority 101 runs before the program's other global constructors, which are built for AVX2.
[[gnu::constructor(101)]] void CheckAtStartup() {
    Check();
}

} // Anonymous namespace

void CheckCpuRequirements() {
    Check();
}
#else
void CheckCpuRequirements() {}
#endif

} // namespace Common::X64
