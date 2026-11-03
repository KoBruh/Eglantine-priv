# User Handbook - Third party tools and extras

Eglantine by itself lacks some functionality - or otherwise requires external files (such as packaging) to operate correctly in a given OS.

While most of the links mentioned in this guide are relatively "safe"; we urge users to use their due diligence and appropriately verify the integrity of all files downloaded and ensure they're not compromised.

- [ES-DE Frontend Support](https://github.com/GlazedBelmont/es-de-android-custom-systems)

## Repository

Eglantine's repository is <https://github.com/notsapinho/Eglantine>, where its releases are published too. It has no official mirrors.

## Configuring ES-DE

### Method 1

1. Download the ZIP from [GitHub](https://github.com/GlazedBelmont/es-de-android-custom-systems)
2. Unzip the file and extract `es_systems.xml` and `es_find_rules.xml` to `\Odin2\Internal shared storage\ES-DE\custom_systems`.
3. Press `Start -> Other Settings -> Alternative Emulators` and set it to Eden (Standalone). The entry is named after Eden; Eglantine's Android app uses the same package name, `dev.eden.eden_emulator`.

### Method 2

1. Navigate to `\Odin2\Internal shared storage\ES-DE\custom_systems`.
2. Add this to your `es_find_rules.xml`:

```xml
<!-- Standard aka. normal release -->
<emulator name="EDEN">
    <rule type="androidpackage">
        <entry>dev.eden.eden_emulator/org.yuzu.yuzu_emu.activities.EmulationActivity</entry>
    </rule>
</emulator>

<!-- Optimized -->
<emulator name="EDEN">
    <rule type="androidpackage">
        <entry>com.miHoYo.Yuanshen/org.yuzu.yuzu_emu.activities.EmulationActivity</entry>
    </rule>
</emulator>
```

1. Add this line of text to your `es_systems.xml` underneath where the rest of your switch system entries are:

```xml
<command label="Eden (Standalone)">%EMULATOR_EDEN% %ACTION%=android.nfc.action.TECH_DISCOVERED %DATA%=%ROMPROVIDER%</command>
```

## Configuring GameMode

There is a checkbox to enable gamemode automatically. The `libgamemode.so` library must be findable on the standard `LD_LIBRARY_PATH` otherwise it will not properly be enabled. If for whatever reason it doesn't work, see [Arch wiki: GameMode](https://wiki.archlinux.org/title/GameMode) for more info.

You may launch the emulator directly via the wrapper `gamemode <program>`, and things should work out of the box.
