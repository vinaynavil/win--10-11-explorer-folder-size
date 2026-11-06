# Explorer Folder Size for Windows 11 & 10

<p align="center">
  <img src="assets/icon.svg" width="128" height="128" alt="Explorer Folder Size Icon" />
</p>

<p align="center">
  <strong>Native, zero-lag in-process folder size display & adaptive file size formatting for Windows 11 & 10 File Explorer.</strong><br>
  Natively populates folder sizes directly inside File Explorer's Details view with zero scroll lag, zero column resize stutter, and adaptive KB/MB/GB/TB size formatting.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011%20%7C%2010-0078D6?logo=windows" alt="Windows Platform" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue?logo=c%2B%2B" alt="C++20" />
  <img src="https://img.shields.io/badge/Architecture-In--Process%20Shell%20Hook-00A4EF" alt="In-Process Shell Hook" />
  <img src="https://img.shields.io/badge/RAM%20Usage-~2%20MB-success" alt="RAM Usage" />
  <img src="https://img.shields.io/badge/License-MIT-green" alt="MIT License" />
</p>

---

## ⚡ Key Highlights

- **100% Native Details View Integration**: Renders directly inside File Explorer's native list view cells. Native hover pills, multiselect highlights, focus rectangles, and theming remain 100% untouched.
- **Zero Scroll Stutter & Zero Column Resize Lag**: Eliminates external transparent overlay windows and UI Automation walks. Vertical scrollbar drags and horizontal column divider resizes operate at your monitor's native refresh rate (60Hz / 120Hz / 144Hz / 240Hz) with 0ms lag.
- **Adaptive Size Formatting (KB / MB / GB / TB)**: Extends Windows Explorer's native `Size` column to format large files (>= 1 MB) and folders with adaptive units (`50.0 MB`, `2.50 GB`, `1.20 TB`) while preserving native KB display for smaller files (`< 1 MB`).
- **High-Performance Multi-Threaded Engine**: Asynchronously calculates folder sizes in the background using `FindFirstFileExW` with `FIND_FIRST_EX_LARGE_FETCH` and validates directory write times (`ftLastWriteTime`) for instant cache retrieval.
- **Instant Shell Change Notification**: As soon as a folder's size calculation finishes in the background, `SHChangeNotify` instructs File Explorer to invalidate and re-query that specific row immediately.
- **Ultra-Lightweight System Tray Host**: `ExplorerFolderSize.exe` operates as a minimal tray host (~44 KB binary, ~2 MB RAM) managing the injection and lifecycle of `ExplorerFolderSizeHook.dll`.
- **Safe Dynamic Detours**: Safely detours `PSFormatForDisplay`, `CFSPropertyStore::GetValue`, and `CFSFolder::GetDetailsEx` with re-entrancy protection and clean unhooking on shutdown.

---

## 🛠️ Architecture & How It Works

```
┌────────────────────────────────────────────────────────┐
│           ExplorerFolderSize.exe (Tray Host)           │
│  - System tray menu (Pause, Clear Cache, Autostart)    │
│  - Discovers Explorer threads & installs CallWndProc   │
│  - Shared IPC Control (Local\Win11ExplorerFolderSize)  │
└───────────────────────────┬────────────────────────────┘
                            │ SetWindowsHookEx (WH_CALLWNDPROC)
                            ▼
┌────────────────────────────────────────────────────────┐
│          ExplorerFolderSizeHook.dll (explorer.exe)     │
│  - IAT Hook: PSFormatForDisplay (propsys.dll)          │
│  - Vtable Hook: CFSPropertyStore::GetValue (Slot 5)    │
│  - Vtable Hook: CFSFolder::GetDetailsEx (Slot 17)      │
│  - Async SizeCalculator Thread Pool                    │
│  - SHChangeNotify(SHCNE_UPDATEITEM) on completion      │
└────────────────────────────────────────────────────────┘
```

1. **In-Process Shell Detours**: When loaded into `explorer.exe`, `ExplorerFolderSizeHook.dll` intercepts Explorer's queries for `PKEY_Size`.
2. **Adaptive Formatting**: For files >= 1 MB, clearing `PDFF_ALWAYSKB` instructs Windows Property System (`propsys.dll`) to display natural MB/GB/TB units.
3. **Async Folder Sizes**: Folders query the in-process `SizeCalculator`. If cached, the size is returned immediately. If calculating, background worker threads compute the directory size and dispatch `SHChangeNotify` to trigger an instant single-item refresh.

---

## 🚀 Getting Started

### Running Pre-Built Binaries
1. Place `ExplorerFolderSize.exe` and `ExplorerFolderSizeHook.dll` together in the same directory.
2. Run `ExplorerFolderSize.exe`.
3. Open Windows File Explorer, switch to **Details** view, and ensure the **Size** column is visible. Folder sizes and adaptive formatting appear automatically!

---

## ⚙️ System Tray Controls

Explorer Folder Size lives unobtrusively in your system tray:

Right-click the tray icon to access:
- **Pause Folder Sizes**: Temporarily suspends custom formatting and restores native Explorer behavior.
- **Clear Size Cache**: Flushes in-memory folder size caches to force recalculation.
- **Run at Windows Startup**: Toggles automatic startup with Windows (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- **Uninstall & Clean Up...**: Safely unhooks File Explorer, removes all Windows startup registry entries, and exits the app so binary files can be cleanly deleted.
- **Exit**: Unhooks `explorer.exe` threads cleanly and terminates the tray application.

You can also run uninstallation directly from PowerShell / Command Prompt:
```powershell
.\ExplorerFolderSize.exe --uninstall
```

---

## 🔨 Building from Source

### Requirements
- **Visual Studio 2022** (MSVC C++20 with Windows 10/11 SDK)
- **CMake** 3.20 or later

### Build Instructions

```powershell
# 1. Clone the repository
git clone https://github.com/vinaynavil/win--10-11-explorer-folder-size.git
cd win--10-11-explorer-folder-size

# 2. Configure with CMake
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. Build Release binaries
cmake --build build --config Release
```

The optimized binaries will be generated in `build/Release/`:
- `ExplorerFolderSize.exe` (Tray Host)
- `ExplorerFolderSizeHook.dll` (In-Process Hook)

---

## 📂 Project Structure

```
win11-explorer-folder-size/
├── assets/
│   ├── icon.svg             # Vector master icon
│   └── icon.ico             # Embedded multi-resolution icon
├── src/
│   ├── main.cpp             # Tray host, hook injection & IPC manager
│   ├── ShellHook.h          # Shared IPC struct & export declarations
│   ├── ShellHookDll.cpp     # In-process Shell & Property System hook DLL
│   ├── SizeCalculator.h     # Multi-threaded folder size calculation header
│   ├── SizeCalculator.cpp   # FindFirstFileExW async engine & SHChangeNotify
│   ├── app.rc               # Windows resource definitions (App & Tray Icon)
│   └── resource.h           # Resource identifiers
├── CMakeLists.txt           # CMake build script
├── .gitignore               # Git ignore rules
├── LICENSE                  # MIT License
└── README.md                # Project documentation
```

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
