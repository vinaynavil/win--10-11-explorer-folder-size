# Explorer Folder Size for Windows 11 & 10

<p align="center">
  <img src="assets/icon.svg" width="128" height="128" alt="Explorer Folder Size Icon" />
</p>

<p align="center">
  <strong>Ultra-lightweight, hardware-accelerated folder size display for Windows 11 & 10 File Explorer.</strong><br>
  Renders folder sizes natively in File Explorer's Details view with zero DLL injection, zero system hooks, and zero impact on native Explorer behavior.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011%20%7C%2010-0078D6?logo=windows" alt="Windows Platform" />
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-blue?logo=c%2B%2B" alt="C++20" />
  <img src="https://img.shields.io/badge/Renderer-Direct2D%20%2F%20DirectWrite-00A4EF" alt="Direct2D" />
  <img src="https://img.shields.io/badge/RAM%20Usage-~350%20KB-success" alt="RAM Usage" />
  <img src="https://img.shields.io/badge/License-MIT-green" alt="MIT License" />
</p>

---

## ⚡ Key Highlights

- **Pure Transparent Overlay**: Direct2D hardware-accelerated text rendering aligned directly into the native File Explorer **Size** column. Only empty folder size cells are drawn—all file rows, native hover states, and selection pill borders remain 100% native Windows rendering.
- **Fluid Monitor Refresh Rate Dragging (60Hz / 144Hz / 240Hz)**: Column divider dragging and window resizing adapt dynamically to your display's native refresh rate via lightweight `OBJID_CURSOR` tracking with zero stutter.
- **Geometric Capacity Buffering**: Column widening uses over-allocated capacity buffers to eliminate per-pixel GDI/DIBSection reallocation stutter during drags.
- **Native DirectWrite Ellipsis Trimming**: Shrinking columns narrow automatically trims folder size text with clean character-level ellipsis (`...`).
- **High-Performance Multi-Threaded Engine**: Calculates folder sizes asynchronously using `FindFirstFileExW` with `FIND_FIRST_EX_LARGE_FETCH` and verifies directory write times (`ftLastWriteTime`) for instant cache retrieval.
- **Extremely Low Memory & Zero Idle CPU**: Active working set trimming keeps RAM consumption at **~350 KB – 1.5 MB** with **0.0% CPU** when idle.
- **Non-Invasive Architecture**: Runs as a standalone desktop utility—**no DLL injection**, **no shell namespace extensions**, and **no invasive API hooking** that could trigger antivirus or anti-cheat flags.
- **Windows 11 Tabs & Dark/Light Mode Aware**: Seamlessly resolves tabs in Windows 11 File Explorer (`ShellTabWindowClass`) and dynamically switches text colors to match the Windows theme.

---

## 🛠️ Architecture & How It Works

```
┌────────────────────────────────────────────────────────┐
│             Windows 11 / 10 File Explorer               │
│   (CabinetWClass -> ShellTabWindowClass -> DirectUIHWND) │
└───────────────────────────┬────────────────────────────┘
                            │
               UIA Bounds & Window Tracking
               (EVENT_OBJECT_LOCATIONCHANGE / OBJID_CURSOR)
                            │
                            ▼
              ┌───────────────────────────┐
              │      ExplorerTracker      │
              │  - Frame pacing (QPC)     │
              │  - Fast Geometry Path     │
              │  - Header & Tab Cache     │
              └─────────────┬─────────────┘
                            │
             ┌──────────────┴──────────────┐
             ▼                             ▼
┌───────────────────────────┐ ┌───────────────────────────┐
│      SizeCalculator       │ │      OverlayRenderer      │
│  - Worker Thread Pool     │ │  - Direct2D / DirectWrite │
│  - FindFirstFileExW Fetch │ │  - Capacity DIBSection    │
│  - Write-Time Cache       │ │  - WS_EX_LAYERED Overlay  │
└───────────────────────────┘ └───────────────────────────┘
```

1. **Window & Column Tracking (`ExplorerTracker`)**: Monitors the active File Explorer window using lightweight WinEvents and UI Automation caching. Live cursor motion (`OBJID_CURSOR`) drives updates during column divider dragging without polling.
2. **Fast Geometry Path (`TryFastGeometryRefresh`)**: When dragging dividers or moving the window, geometry calculations execute in **~60 µs**, completely bypassing full UI Automation tree walks.
3. **Background Calculation (`SizeCalculator`)**: Directories are traversed asynchronously across background worker threads. Results are cached and validated against the folder's write timestamp.
4. **Transparent Rendering (`OverlayRenderer`)**: A click-through, non-activating layered window (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`) renders folder size text with sub-pixel DirectWrite anti-aliasing directly over the Explorer viewport.

---

## 🚀 Getting Started

### Prerequisites
- **Operating System**: Windows 11 or Windows 10 (version 1809 or later)
- **File Explorer View**: Details view with the **Size** column enabled.

### Running Pre-Built Binary
1. Download `ExplorerFolderSize.exe` from the latest release.
2. Run `ExplorerFolderSize.exe`.
3. Open Windows File Explorer, switch to **Details** view, and ensure the **Size** column is visible. Folder sizes will appear seamlessly!

---

## ⚙️ System Tray Controls

Explorer Folder Size lives unobtrusively in your system tray:

<p align="center">
  <img src="assets/icon.svg" width="48" height="48" alt="Tray Icon" />
</p>

Right-click the tray icon to access:
- **Pause Folder Sizes**: Temporarily hides the overlay and suspends folder tracking.
- **Clear Size Cache**: Flushes in-memory folder size caches to force recalculation.
- **Run at Windows Startup**: Toggles automatic startup with Windows (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- **Exit**: Unloads the overlay and gracefully terminates the application.

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

# 3. Build Release binary
cmake --build build --config Release
```

The optimized executable will be generated at:
`build/Release/ExplorerFolderSize.exe`

---

## 📂 Project Structure

```
win11-explorer-folder-size/
├── assets/
│   ├── icon.svg             # 2D flat vector master icon
│   └── icon.ico             # Multi-res embedded Windows icon (16, 24, 32, 48, 256 px)
├── src/
│   ├── main.cpp             # WinMain entry, tray manager, and message loop
│   ├── app.rc               # Windows resource definitions (App & Tray Icon)
│   ├── resource.h           # Resource identifiers
│   ├── ExplorerTracker.h    # Explorer window & header tracking interface
│   ├── ExplorerTracker.cpp  # WinEvent pacing, UIA tracking, and fast geometry refresh
│   ├── OverlayRenderer.h    # Direct2D transparent layered window overlay header
│   ├── OverlayRenderer.cpp  # Direct2D/DirectWrite rendering with capacity buffering
│   ├── SizeCalculator.h     # Multi-threaded folder size calculation header
│   └── SizeCalculator.cpp   # FindFirstFileExW async calculation and timestamp cache
├── CMakeLists.txt           # CMake build script
├── .gitignore               # Git ignore rules
├── LICENSE                  # MIT License
└── README.md                # Project documentation
```

---

## 📄 License

This project is licensed under the [MIT License](LICENSE) - feel free to use, modify, and distribute as you see fit.
