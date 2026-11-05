#pragma once
#include <windows.h>
#include <cstdint>

constexpr wchar_t kSharedHookControlName[] = L"Local\\Win11ExplorerFolderSize_SharedCtrl";

#pragma pack(push, 8)
struct SharedHookControl {
    volatile LONG paused;          // 0 = active, 1 = paused
    volatile LONG clearCacheSeq;   // incremented whenever user clicks "Clear Size Cache" in tray menu
    volatile LONG unloadRequested; // set to 1 on app exit
    DWORD reserved;
};
#pragma pack(pop)

#ifdef EXPLORERFOLDERSIZEHOOK_EXPORTS
#define SHELLHOOK_API extern "C" __declspec(dllexport)
#else
#define SHELLHOOK_API extern "C" __declspec(dllimport)
#endif

SHELLHOOK_API LRESULT CALLBACK ExplorerCallWndProc(int nCode, WPARAM wParam, LPARAM lParam);
SHELLHOOK_API void CALLBACK NotifyExplorerRefresh();

