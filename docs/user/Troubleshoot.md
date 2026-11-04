# User Handbook - Troubleshooting

## Vulkan initialization error

- Ensure you have the latest drivers
- Uninstall old drivers, for Windows you can use [Display Driver Uninstaller](https://www.guru3d.com/download/display-driver-uninstaller-download/)
- Change backend manually in the settings file (set it from `0` to `1` or vice versa).
- Disconnect your second monitor, if any

## This mod only works on an Emulator

- Enable RNG seed
- Set RNG seed to 0

## Eglantine Fails to Launch and Does Not Leave Any Logs

**[See here](https://evilperson1337.notion.site/Windows-Eden-Fails-to-Launch-and-Does-Not-Leave-Any-Logs-2b057c2edaf68156b640cf1ac549870a) for an illustrated guide (written for Eden).**

### Error Details

*Behavior*:  Program appears not to launch or exits immediately without leaving any log entries.
*Platform(s) Affected*:
- **Windows**

**Error Log Entries:** 

```text
None
```
**Example Error Message Entry in Windows Event Viewer**
```text
Faulting application name: eglantine.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
Exception code: 0xc0000005
Fault offset: 0x0000000000014778
Faulting process id: 0x2AF0
Faulting application start time: 0x1DB7C30D2972402
Faulting application path: C:\temp\eglantine-windows-x86_64\eglantine.exe
Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
Faulting package full name: 
Faulting package-relative application ID:
```

---

### Causes

<aside>

#### Issue 1: Missing libraries next to eglantine.exe

*The release zip carries every library Eglantine needs (Qt, the C++ runtime and the rest) as DLLs next to `eglantine.exe`. If only the exe was copied out of the folder, or the zip was only partly extracted, Windows cannot find them and the program cannot start.*

1. Extract the whole release zip into a folder of its own.
2. Run `eglantine.exe` from that folder. Make a shortcut to it if you want it elsewhere, instead of moving the exe.
3. Launch Eglantine again; if the main window appears, you are good to go. If not, proceed to the next issue.

A build you compiled yourself with MSVC also needs Microsoft's [Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170) on a PC without Visual Studio.
</aside>

<aside>

#### Issue 2: Corrupted System Files

*A corruption of necessary system files can cause odd behaviors when Eglantine tries to access them.  It is a very rare case and you would likely see other programs misbehaving if this is what your issue is, but you can try if you have no other options.*

1. Launch Eglantine to generate a crash.
2. Confirm there are no logs created in the log directory.
    1. See the [How to Access Logs](./HowToAccessLogs.md) page for the log location if you need it.
    2. If there are any entries in here since you tried step 1, this is likely not your issue.
3. Navigate to your *Windows Event Viewer* (Start Menu > **eventvwr.msc)**. 
4. Expand **Windows Logs** and select **Application.**
    
5. Look for an entry with the Level of Error, and look for a message similar to the following
    
    ```text
    Faulting application name: eglantine.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
    Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
    Exception code: 0xc0000005
    Fault offset: 0x0000000000014778
    Faulting process id: 0x2AF0
    Faulting application start time: 0x1DB7C30D2972402
    Faulting application path: C:\temp\eglantine-windows-x86_64\eglantine.exe
    Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
    Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
    Faulting package full name: 
    Faulting package-relative application ID:
    ```
    
6. Run a Command Prompt terminal Window as Administrator.
7. Enter the following command and wait for it to complete.  It will take a while, just be patient and do other things while it completes.
    
    ```bat
    DISM /Online /Cleanup-Image /RestoreHealth
    ```
    
8. Reboot your computer.
9. Launch Eglantine and verify it is now working.
</aside>
