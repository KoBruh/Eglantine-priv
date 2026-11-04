# User Handbook - The Basics

## Introduction

Eglantine is a very complicated piece of software, there are many knobs and toggles that can be configured. Most of these are invisible to normal users, however, power users may be able to leverage them to their advantage.

This handbook primarily describes such knobs and toggles. Normal configuration options are described within the emulator itself and will not be covered in detail.

## Requirements

The emulator is very demanding on hardware and requires a decent mid-range computer/cellphone.

See [the requirements page](https://archive.is/sv83h) for recommended and minimum specs.

The x86_64 release packages need a CPU with AVX2 (the x86-64-v3 level: AVX2, BMI2 and FMA), which Intel Core processors have since the 4th generation (Haswell, 2013) and AMD processors since Ryzen (and Excavator); some recent Intel Pentium, Celeron and Atom models lack it. On a CPU without it the emulator shows a message and quits. To run on an older CPU, build it yourself with the default `YUZU_BUILD_PRESET` ([Build options](../Options.md)). The arm64 packages have no such requirement. The GPU needs to support OpenGL 4.6 ([compatibility list](https://opengl.gpuinfo.org/)), or Vulkan 1.1 ([compatibility list](https://vulkan.gpuinfo.org/)).

If your GPU doesn't support or is just behind by a minor version, see Mesa environment variables below (*nix only).

## Releases and versions

Eglantine has no versioned or nightly releases. Every change to the `main` branch is built and published as a prerelease on the [Releases](https://github.com/notsapinho/Eglantine/releases) page; day-to-day work happens on `dev`, which has no automatic builds. The [README](../../README.md#download) lists the files for each system.

Android and macOS have no release builds; build them yourself ([Build guide](../Build.md)).

## User configuration

### Configuration directories

Eglantine will store configuration files in the following directories:

- **Windows**: `%AppData%\Roaming`.
- **Android**: Data is stored internally.
- **Linux, macOS, FreeBSD, Solaris, OpenBSD**: `$XDG_DATA_HOME`, `$XDG_CACHE_HOME`, `$XDG_CONFIG_HOME`.
- **HaikuOS**: `/boot/home/config/settings/eden`

If a `user` directory is present in the current working directory, that will override all global configuration directories and the emulator will use the `user` directory instead.

### Environment variables

Throughout the handbook, environment variables are mentioned. These are often either global (system wide) or local (set in a script, bound only to the current session). It's heavily recommended to use them in a local context only, as this allows you to rollback changes easily (if for example, there are regressions setting them).

The recommended way is to create a `.bat` file alongside the emulator `.exe`; contents of which could resemble something like:

```bat
set "__GL_THREADED_OPTIMIZATIONS=1"
set "SOME_OTHER_VAR=1"
eglantine.exe
```

Android doesn't have a convenient way to set environment variables.

For other platforms, the recommended method is using a shell script:

```sh
export __GL_THREADED_OPTIMIZATIONS=1
export SOME_OTHER_VAR=1
./eglantine
```

Then just running `chmod +x script.sh && source script.sh`.

## Compatibility list

The [README](../../README.md#compatibility) lists the games known to run; a full compatibility list will follow. [EmuReady](https://www.emuready.com/) has compatibility information for other Switch emulators.
