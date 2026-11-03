# Eglantine Developer Documentation

Are you just a casual user? Take a look at our [User Handbook](./user/README.md) then!

If you want to contribute, take a gander at [CONTRIBUTING.md](../CONTRIBUTING.md).

This contains documentation created by developers, build instructions, guidelines, developer notes, and more.

## Building

- **[General Build Instructions](./Build.md)**
  - **[Android](./build/Android.md)**
- **[Dependencies](./Deps.md)**
- **[CMake Options](./Options.md)**
- **[Cross Compiling](./CrossCompile.md)**
- **[Platform-Specific Caveats](./Caveats.md)**
- **[CPM - CMake Package Manager](./CPMUtil.md)**

## Development

- **[Development Guidelines](./Development.md)**
- **[Contributing](../CONTRIBUTING.md)**
- **[Debug Guidelines](./Debug.md)**
  - **[RenderDoc](./Debug.md#renderdoc)**
- **[The NVIDIA SM86 (Maxwell) GPU](./NvidiaGpu.md)**
- **[Driver Bugs](./DriverBugs.md)**
- Subsystems:
  - **[Design Overview](./DesignOverview.md)**
  - **[Dynarmic](./dynarmic/README.md)**
    - **[Design](./dynarmic/Design.md)**
  - **[HOS Kernel](./HosKernel.md)**
  - **[Settings](./Settings.md)**

## Developer notes

Notes on the Switch 2 work, kept in [dev](./dev):

- **[Switch 2 titles: setup, blockers and fixes](./dev/NX2_NOTES.md)**
- **[Splitting the GPU thread](./dev/NX2_GPU_THREAD_SPLIT.md)**
- **[Experimental NV cooperative-matrix lowering](./dev/experimental-cooperative-matrix.md)**
- Game bring-up:
  - **[Fire Emblem: Fortune's Weave](./dev/bringup/FE_FORTUNES_WEAVE.md)**
  - **[Kirby Air Riders](./dev/bringup/KIRBY_AIR_RIDERS.md)**
  - **[Welcome Tour](./dev/bringup/WELCOME_TOUR.md)**

## Externals

Other useful resources in general, take a quick read if you need.

- **[SwitchBrew](https://switchbrew.org/wiki/Main_Page)**
- **[IPS file format](https://zerosoft.zophar.net/ips.php)**
- **[IPSwitch file format](https://github.com/3096/ipswitch)**
