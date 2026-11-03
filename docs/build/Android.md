# Android

## Dependencies

* [Android Studio](https://developer.android.com/studio)
* [NDK 27+ and CMake 3.22.1](https://developer.android.com/studio/projects/install-ndk#default-version)
* [Git](https://git-scm.com/download)

## WINDOWS ONLY - Additional Dependencies

* **[Visual Studio 2022 Community](https://visualstudio.microsoft.com/downloads/)** - **Make sure to select "Desktop development with C++" support in the installer. Make sure to update to the latest version if already installed.**
* **[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** - **Make sure to select Latest SDK.**
  * A convenience script to install the latest SDK is provided in `.ci\windows\install-vulkan-sdk.ps1`.

## Cloning Eglantine with Git

```sh
git clone --recursive https://github.com/notsapinho/Eglantine.git
```

Eglantine by default will be cloned into:

* `C:\Users\<user-name>\Eglantine` on Windows
* `~/Eglantine` on Linux and macOS

## Building

1. Start Android Studio, on the startup dialog select `Open`.
2. Navigate to the `Eglantine/src/android` directory and click on `OK`.
3. In `Build > Select Build Variant`, select `release` or `relWithDebInfo` as the "Active build variant".
4. Build the project with `Build > Make Project` or run it on an Android device with `Run > Run 'app'`.

## Building with Terminal

1. Download the SDK and NDK from Android Studio.
2. Navigate to SDK and NDK paths.
3. Then set ANDROID_SDK_ROOT and ANDROID_NDK_ROOT in terminal via
`export ANDROID_SDK_ROOT=path/to/sdk`
`export ANDROID_NDK_ROOT=path/to/ndk`.
4. Navigate to `Eglantine/src/android`.
5. Then Build with `./gradlew assembleRelWithDebInfo`.
6. To build the optimised build use `./gradlew assembleGenshinSpoofRelWithDebInfo`.
7. You can pass extra variables to cmake via `-PYUZU_ANDROID_ARGS="-D..."`

Remember to have a Java SDK installed if not already, on Debian and similar this is done with `sudo apt install openjdk-17-jdk`.

### Additional Resources

<https://developer.android.com/studio/intro>
