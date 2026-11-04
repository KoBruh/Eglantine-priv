# SPDX-FileCopyrightText: Copyright 2025 crueter
# SPDX-License-Identifier: LGPL-3.0-or-later

## UseLTO ##

# Enable Interprocedural Optimization (IPO).
# Self-explanatory.

option(ENABLE_LTO "Enable Link-Time Optimization (LTO)" OFF)

if (ENABLE_LTO)
    include(CheckIPOSupported)
    # The check links its test program with the language flags but not CMAKE_EXE_LINKER_FLAGS, so
    # it would try the toolchain's default linker rather than one chosen there (-fuse-ld=lld):
    # with MinGW clang that is GNU ld, which cannot link ThinLTO objects. It gets the build's
    # linker flags for the duration of the check.
    set(_lto_c_flags "${CMAKE_C_FLAGS}")
    set(_lto_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
    check_ipo_supported(RESULT COMPILER_SUPPORTS_LTO OUTPUT _lto_check_output)
    set(CMAKE_C_FLAGS "${_lto_c_flags}")
    set(CMAKE_CXX_FLAGS "${_lto_cxx_flags}")
    if(NOT COMPILER_SUPPORTS_LTO)
        message(FATAL_ERROR
        "Your compiler does not support interprocedural optimization"
        " (IPO). Disable ENABLE_LTO and try again.\n${_lto_check_output}")
    endif()
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ${COMPILER_SUPPORTS_LTO})
    # CMake compiles with clang's -flto=thin (ThinLTO) but links without it. The driver then
    # passes lld neither the optimization level nor the CPU, and lld's LTO backend optimizes at
    # -O2 instead of the build's -O3.
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
        set(CMAKE_C_LINK_OPTIONS_IPO -flto=thin)
        set(CMAKE_CXX_LINK_OPTIONS_IPO -flto=thin)
    endif()
endif()