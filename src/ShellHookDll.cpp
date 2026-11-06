#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

#include "ShellHook.h"
#include "SizeCalculator.h"

namespace {

// ============================================================================
// Process Identity & Shared Memory
// ============================================================================

bool IsExplorerProcess() {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) > 0) {
        const wchar_t* fileName = wcsrchr(exePath, L'\\');
        if (fileName) {
            fileName++;
        } else {
            fileName = exePath;
        }
        return (_wcsicmp(fileName, L"explorer.exe") == 0);
    }
    return false;
}

HANDLE g_hSharedMap = nullptr;
SharedHookControl* g_pSharedCtrl = nullptr;

void InitSharedMemory() {
    if (g_pSharedCtrl) return;
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSharedHookControlName);
    if (!hMap) {
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedHookControl), kSharedHookControlName);
    }
    if (hMap) {
        g_hSharedMap = hMap;
        g_pSharedCtrl = static_cast<SharedHookControl*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedHookControl)));
    }
}

bool IsUnloadRequested() {
    if (g_pSharedCtrl) {
        return (InterlockedCompareExchange(&g_pSharedCtrl->unloadRequested, 0, 0) != 0);
    }
    return false;
}

bool IsPaused() {
    if (IsUnloadRequested()) {
        return true;
    }
    if (g_pSharedCtrl) {
        return (InterlockedCompareExchange(&g_pSharedCtrl->paused, 0, 0) != 0);
    }
    return false;
}

std::unique_ptr<SizeCalculator> g_pSizeCalc;
LONG g_lastClearCacheSeq = 0;

void CheckClearCache() {
    if (g_pSharedCtrl && g_pSizeCalc) {
        LONG seq = InterlockedCompareExchange(&g_pSharedCtrl->clearCacheSeq, 0, 0);
        if (seq != g_lastClearCacheSeq) {
            g_lastClearCacheSeq = seq;
            g_pSizeCalc->ClearCache();
        }
    }
}

// ============================================================================
// Hook 1: IAT Hooking for PSFormatForDisplay / PSFormatForDisplayAlloc
// ============================================================================

using PFN_PSFormatForDisplay = HRESULT (STDAPICALLTYPE*)(
    REFPROPERTYKEY propkey,
    REFPROPVARIANT propvar,
    PROPDESC_FORMAT_FLAGS pdffFlags,
    PWSTR pszText,
    DWORD cchText);

using PFN_PSFormatForDisplayAlloc = HRESULT (STDAPICALLTYPE*)(
    REFPROPERTYKEY propkey,
    REFPROPVARIANT propvar,
    PROPDESC_FORMAT_FLAGS pdffFlags,
    PWSTR* ppszDisplay);

PFN_PSFormatForDisplay g_realPSFormatForDisplay = nullptr;
PFN_PSFormatForDisplayAlloc g_realPSFormatForDisplayAlloc = nullptr;

HRESULT STDAPICALLTYPE Hook_PSFormatForDisplay(
    REFPROPERTYKEY key,
    REFPROPVARIANT propvar,
    PROPDESC_FORMAT_FLAGS pdffFlags,
    PWSTR pszText,
    DWORD cchText)
{
    if (!IsPaused() && IsEqualPropertyKey(key, PKEY_Size)) {
        if (propvar.vt == VT_UI8 && propvar.uhVal.QuadPart >= 1048576ULL) {
            pdffFlags = static_cast<PROPDESC_FORMAT_FLAGS>(pdffFlags & ~PDFF_ALWAYSKB);
        }
    }
    if (g_realPSFormatForDisplay) {
        return g_realPSFormatForDisplay(key, propvar, pdffFlags, pszText, cchText);
    }
    return E_UNEXPECTED;
}

HRESULT STDAPICALLTYPE Hook_PSFormatForDisplayAlloc(
    REFPROPERTYKEY key,
    REFPROPVARIANT propvar,
    PROPDESC_FORMAT_FLAGS pdffFlags,
    PWSTR* ppszDisplay)
{
    if (!IsPaused() && IsEqualPropertyKey(key, PKEY_Size)) {
        if (propvar.vt == VT_UI8 && propvar.uhVal.QuadPart >= 1048576ULL) {
            pdffFlags = static_cast<PROPDESC_FORMAT_FLAGS>(pdffFlags & ~PDFF_ALWAYSKB);
        }
    }
    if (g_realPSFormatForDisplayAlloc) {
        return g_realPSFormatForDisplayAlloc(key, propvar, pdffFlags, ppszDisplay);
    }
    return E_UNEXPECTED;
}

struct IATHookEntry {
    void** pSlot;
    void* origFunc;
};

std::vector<IATHookEntry> g_iatHooks;
std::mutex g_iatMutex;

void HookIATInModule(HMODULE hMod, void* targetFunc, void* hookFunc) {
    if (!hMod || !targetFunc || !hookFunc) return;
    BYTE* base = reinterpret_cast<BYTE*>(hMod);
    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

    std::lock_guard<std::mutex> lock(g_iatMutex);

    // Standard imports
    auto importDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress != 0 && importDir.Size != 0) {
        auto importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + importDir.VirtualAddress);
        for (; importDesc->Characteristics != 0 || importDesc->FirstThunk != 0; ++importDesc) {
            if (importDesc->FirstThunk == 0) continue;
            auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + importDesc->FirstThunk);
            for (; thunk->u1.Function != 0; ++thunk) {
                if (reinterpret_cast<void*>(thunk->u1.Function) == targetFunc) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect)) {
                        IATHookEntry entry;
                        entry.pSlot = reinterpret_cast<void**>(&thunk->u1.Function);
                        entry.origFunc = targetFunc;
                        g_iatHooks.push_back(entry);

                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunc);
                        VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), oldProtect, &oldProtect);
                    }
                }
            }
        }
    }

    // Delay-load imports
    auto delayDir = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayDir.VirtualAddress != 0 && delayDir.Size != 0) {
        auto delayDesc = reinterpret_cast<PIMAGE_DELAYLOAD_DESCRIPTOR>(base + delayDir.VirtualAddress);
        for (; delayDesc->DllNameRVA != 0; ++delayDesc) {
            if (delayDesc->ImportAddressTableRVA == 0) continue;
            auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + delayDesc->ImportAddressTableRVA);
            for (; thunk->u1.Function != 0; ++thunk) {
                if (reinterpret_cast<void*>(thunk->u1.Function) == targetFunc) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect)) {
                        IATHookEntry entry;
                        entry.pSlot = reinterpret_cast<void**>(&thunk->u1.Function);
                        entry.origFunc = targetFunc;
                        g_iatHooks.push_back(entry);

                        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunc);
                        VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), oldProtect, &oldProtect);
                    }
                }
            }
        }
    }
}

void InstallIATHooks() {
    HMODULE hPropsys = GetModuleHandleW(L"propsys.dll");
    if (!hPropsys) {
        hPropsys = LoadLibraryW(L"propsys.dll");
    }
    if (hPropsys) {
        g_realPSFormatForDisplay = reinterpret_cast<PFN_PSFormatForDisplay>(GetProcAddress(hPropsys, "PSFormatForDisplay"));
        g_realPSFormatForDisplayAlloc = reinterpret_cast<PFN_PSFormatForDisplayAlloc>(GetProcAddress(hPropsys, "PSFormatForDisplayAlloc"));
    }

    const wchar_t* targetModules[] = {
        L"windows.storage.dll",
        L"shell32.dll",
        L"ExplorerFrame.dll",
        nullptr
    };

    for (const wchar_t* modName : targetModules) {
        HMODULE hMod = modName ? GetModuleHandleW(modName) : GetModuleHandleW(nullptr);
        if (hMod) {
            if (g_realPSFormatForDisplay) {
                HookIATInModule(hMod, reinterpret_cast<void*>(g_realPSFormatForDisplay), reinterpret_cast<void*>(Hook_PSFormatForDisplay));
            }
            if (g_realPSFormatForDisplayAlloc) {
                HookIATInModule(hMod, reinterpret_cast<void*>(g_realPSFormatForDisplayAlloc), reinterpret_cast<void*>(Hook_PSFormatForDisplayAlloc));
            }
        }
    }
}

// ============================================================================
// Hook 2: COM Vtable Hooking for CFSPropertyStore & CFSFolder
// ============================================================================

using CFSPropStore_GetValue_t = HRESULT (STDMETHODCALLTYPE*)(IPropertyStore* pThis, REFPROPERTYKEY key, PROPVARIANT* pv);
using CFSFolder_GetDetailsEx_t = HRESULT (STDMETHODCALLTYPE*)(IShellFolder2* pThis, PCUITEMID_CHILD pidl, const SHCOLUMNID* pscid, VARIANT* pv);
using CFSFolder_GetDetailsOf_t = HRESULT (STDMETHODCALLTYPE*)(IShellFolder2* pThis, PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS* psd);

CFSPropStore_GetValue_t g_origCFSPropStoreGetValue = nullptr;
CFSFolder_GetDetailsEx_t g_origCFSFolderGetDetailsEx = nullptr;
CFSFolder_GetDetailsOf_t g_origCFSFolderGetDetailsOf = nullptr;

struct VtblHookEntry {
    void** pSlot;
    void* origFunc;
};

std::vector<VtblHookEntry> g_vtblHooks;
std::mutex g_vtblMutex;

bool PatchVtableSlot(void** vtbl, size_t slot, void* hookFunc, void** ppOrigFunc) {
    if (!vtbl || !hookFunc) return false;
    void** pSlot = &vtbl[slot];
    if (*pSlot == hookFunc) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_vtblMutex);
    DWORD oldProtect = 0;
    if (VirtualProtect(pSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        if (ppOrigFunc && !*ppOrigFunc) {
            *ppOrigFunc = *pSlot;
        }
        VtblHookEntry entry;
        entry.pSlot = pSlot;
        entry.origFunc = *pSlot;
        g_vtblHooks.push_back(entry);

        *pSlot = hookFunc;
        VirtualProtect(pSlot, sizeof(void*), oldProtect, &oldProtect);
        return true;
    }
    return false;
}

HRESULT STDMETHODCALLTYPE Hook_CFSPropertyStore_GetValue(IPropertyStore* pStore, REFPROPERTYKEY key, PROPVARIANT* pv) {
    if (!g_origCFSPropStoreGetValue) return E_UNEXPECTED;
    HRESULT hr = g_origCFSPropStoreGetValue(pStore, key, pv);

    if (IsPaused() || !g_pSizeCalc) {
        return hr;
    }

    if (IsEqualPropertyKey(key, PKEY_Size) && (FAILED(hr) || pv->vt == VT_EMPTY || pv->vt == VT_NULL)) {
        thread_local bool s_inHook = false;
        if (s_inHook) {
            return hr;
        }
        s_inHook = true;
        struct HookGuard {
            bool& ref;
            HookGuard(bool& r) : ref(r) {}
            ~HookGuard() { ref = false; }
        } guard(s_inHook);

        CheckClearCache();

        // 1. Verify item is a directory
        PROPVARIANT pvAttr{};
        PropVariantInit(&pvAttr);
        HRESULT hrAttr = g_origCFSPropStoreGetValue(pStore, PKEY_FileAttributes, &pvAttr);
        bool isDir = (SUCCEEDED(hrAttr) && pvAttr.vt == VT_UI4 && (pvAttr.ulVal & FILE_ATTRIBUTE_DIRECTORY) != 0);
        PropVariantClear(&pvAttr);

        if (!isDir) {
            return hr;
        }

        // 2. Query PKEY_ParsingPath
        PROPVARIANT pvPath{};
        PropVariantInit(&pvPath);
        HRESULT hrPath = g_origCFSPropStoreGetValue(pStore, PKEY_ParsingPath, &pvPath);
        std::wstring folderPath;
        if (SUCCEEDED(hrPath)) {
            if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal) {
                folderPath = pvPath.pwszVal;
            } else if (pvPath.vt == VT_BSTR && pvPath.bstrVal) {
                folderPath = pvPath.bstrVal;
            }
        }
        PropVariantClear(&pvPath);

        if (folderPath.empty()) {
            return hr;
        }

        // 3. Query in-process SizeCalculator
        FolderSizeResult res = g_pSizeCalc->RequestFolderSize(folderPath);
        if (res.status == CalculationStatus::Ready) {
            PropVariantClear(pv);
            pv->vt = VT_UI8;
            pv->uhVal.QuadPart = res.sizeBytes;
            return S_OK;
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CFSFolder_GetDetailsEx(
    IShellFolder2* psf2,
    PCUITEMID_CHILD pidlChild,
    const SHCOLUMNID* pscid,
    VARIANT* pv)
{
    if (!g_origCFSFolderGetDetailsEx) return E_UNEXPECTED;
    HRESULT hr = g_origCFSFolderGetDetailsEx(psf2, pidlChild, pscid, pv);

    if (IsPaused() || !g_pSizeCalc || !pscid || !pidlChild || !pv) {
        return hr;
    }

    if (IsEqualPropertyKey(*pscid, PKEY_Size) && (FAILED(hr) || pv->vt == VT_EMPTY || pv->vt == VT_NULL)) {
        if (pidlChild->mkid.cb >= 3 && (pidlChild->mkid.abID[0] & 0x30) == 0x30) {
            STRRET strret{};
            if (SUCCEEDED(psf2->GetDisplayNameOf(pidlChild, SHGDN_FORPARSING, &strret))) {
                wchar_t pathBuf[MAX_PATH]{};
                HRESULT hrBuf = StrRetToBufW(&strret, pidlChild, pathBuf, ARRAYSIZE(pathBuf));
                if (SUCCEEDED(hrBuf) && pathBuf[0] != L'\0') {
                    CheckClearCache();
                    FolderSizeResult res = g_pSizeCalc->RequestFolderSize(pathBuf);
                    if (res.status == CalculationStatus::Ready) {
                        VariantClear(pv);
                        pv->vt = VT_UI8;
                        pv->ullVal = res.sizeBytes;
                        return S_OK;
                    }
                }
            }
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CFSFolder_GetDetailsOf(
    IShellFolder2* psf2,
    PCUITEMID_CHILD pidlChild,
    UINT iColumn,
    SHELLDETAILS* psd)
{
    if (!g_origCFSFolderGetDetailsOf) return E_UNEXPECTED;
    HRESULT hr = g_origCFSFolderGetDetailsOf(psf2, pidlChild, iColumn, psd);

    if (IsPaused() || !g_pSizeCalc || !pidlChild || !psd) {
        return hr;
    }

    bool isSizeCol = false;
    SHCOLUMNID scid{};
    if (SUCCEEDED(psf2->MapColumnToSCID(iColumn, &scid))) {
        if (IsEqualPropertyKey(scid, PKEY_Size)) {
            isSizeCol = true;
        }
    } else if (iColumn == 1) {
        isSizeCol = true;
    }

    if (isSizeCol) {
        if (pidlChild->mkid.cb >= 3 && (pidlChild->mkid.abID[0] & 0x30) == 0x30) {
            STRRET strret{};
            if (SUCCEEDED(psf2->GetDisplayNameOf(pidlChild, SHGDN_FORPARSING, &strret))) {
                wchar_t pathBuf[MAX_PATH]{};
                HRESULT hrBuf = StrRetToBufW(&strret, pidlChild, pathBuf, ARRAYSIZE(pathBuf));
                if (SUCCEEDED(hrBuf) && pathBuf[0] != L'\0') {
                    CheckClearCache();
                    FolderSizeResult res = g_pSizeCalc->RequestFolderSize(pathBuf);
                    if (res.status == CalculationStatus::Ready && !res.formattedText.empty()) {
                        PWSTR oleStr = nullptr;
                        if (SUCCEEDED(SHStrDupW(res.formattedText.c_str(), &oleStr))) {
                            if (psd->str.uType == STRRET_WSTR && psd->str.pOleStr) {
                                CoTaskMemFree(psd->str.pOleStr);
                            }
                            psd->str.uType = STRRET_WSTR;
                            psd->str.pOleStr = oleStr;
                            return S_OK;
                        }
                    }
                }
            }
        }
    }

    return hr;
}

bool TryDiscoverAndHookFolder(const wchar_t* dirPath) {
    if (!dirPath || dirPath[0] == L'\0') return false;

    PIDLIST_ABSOLUTE pidlFull = nullptr;
    if (FAILED(SHParseDisplayName(dirPath, nullptr, &pidlFull, 0, nullptr)) || !pidlFull) {
        return false;
    }

    IShellFolder2* psf2 = nullptr;
    PCUITEMID_CHILD pidlChild = nullptr;
    HRESULT hrBind = SHBindToParent(pidlFull, IID_PPV_ARGS(&psf2), &pidlChild);
    if (SUCCEEDED(hrBind) && psf2) {
        void** psf2Vtbl = *reinterpret_cast<void***>(psf2);
        PatchVtableSlot(psf2Vtbl, 17, reinterpret_cast<void*>(Hook_CFSFolder_GetDetailsEx), reinterpret_cast<void**>(&g_origCFSFolderGetDetailsEx));
        PatchVtableSlot(psf2Vtbl, 18, reinterpret_cast<void*>(Hook_CFSFolder_GetDetailsOf), reinterpret_cast<void**>(&g_origCFSFolderGetDetailsOf));

        if (!g_origCFSPropStoreGetValue && pidlChild) {
            IPropertyStoreFactory* pItemFactory = nullptr;
            if (SUCCEEDED(psf2->BindToObject(pidlChild, nullptr, IID_IPropertyStoreFactory, reinterpret_cast<void**>(&pItemFactory))) && pItemFactory) {
                IPropertyStore* pStore = nullptr;
                PROPERTYKEY keys[] = { PKEY_Size };
                if (SUCCEEDED(pItemFactory->GetPropertyStoreForKeys(keys, 1, GPS_FASTPROPERTIESONLY | GPS_HANDLERPROPERTIESONLY, IID_PPV_ARGS(&pStore))) && pStore) {
                    void** propStoreVtbl = *reinterpret_cast<void***>(pStore);
                    PatchVtableSlot(propStoreVtbl, 5, reinterpret_cast<void*>(Hook_CFSPropertyStore_GetValue), reinterpret_cast<void**>(&g_origCFSPropStoreGetValue));
                    pStore->Release();
                }
                pItemFactory->Release();
            }
        }
        psf2->Release();
    }
    CoTaskMemFree(pidlFull);
    return (g_origCFSPropStoreGetValue != nullptr && g_origCFSFolderGetDetailsEx != nullptr);
}

void InstallVtableHooks() {
    wchar_t path[MAX_PATH]{};
    if (GetSystemDirectoryW(path, MAX_PATH) > 0) {
        if (TryDiscoverAndHookFolder(path)) return;
    }
    if (GetWindowsDirectoryW(path, MAX_PATH) > 0) {
        if (TryDiscoverAndHookFolder(path)) return;
    }
    if (GetTempPathW(MAX_PATH, path) > 0) {
        size_t l = wcslen(path);
        if (l > 0 && path[l - 1] == L'\\') path[l - 1] = L'\0';
        TryDiscoverAndHookFolder(path);
    }
}

void EnsureInitialized() {
    static std::once_flag s_initOnce;
    std::call_once(s_initOnce, []() {
        if (!IsExplorerProcess()) return;

        InitSharedMemory();

        g_pSizeCalc = std::make_unique<SizeCalculator>([](const std::wstring& folderPath) {
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folderPath.c_str(), nullptr);
        });

        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needCoUninit = (hrCo == S_OK);

        InstallIATHooks();
        InstallVtableHooks();

        if (needCoUninit) {
            CoUninitialize();
        }
    });
}

void CleanupHooks() {
    // 1. Restore Vtable hooks first so new calls go to original methods
    {
        std::lock_guard<std::mutex> lock(g_vtblMutex);
        for (auto it = g_vtblHooks.rbegin(); it != g_vtblHooks.rend(); ++it) {
            DWORD oldProtect = 0;
            if (VirtualProtect(it->pSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *it->pSlot = it->origFunc;
                VirtualProtect(it->pSlot, sizeof(void*), oldProtect, &oldProtect);
            }
        }
        g_vtblHooks.clear();
    }

    // 2. Restore IAT hooks
    {
        std::lock_guard<std::mutex> lock(g_iatMutex);
        for (auto it = g_iatHooks.rbegin(); it != g_iatHooks.rend(); ++it) {
            DWORD oldProtect = 0;
            if (VirtualProtect(it->pSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *it->pSlot = it->origFunc;
                VirtualProtect(it->pSlot, sizeof(void*), oldProtect, &oldProtect);
            }
        }
        g_iatHooks.clear();
    }

    // 3. Grace period so any in-flight thread in explorer.exe exits prolog/epilog
    Sleep(40);

    // 4. Stop SizeCalculator workers cleanly
    if (g_pSizeCalc) {
        g_pSizeCalc.reset();
    }

    // 5. Clean up shared memory
    if (g_pSharedCtrl) {
        UnmapViewOfFile(g_pSharedCtrl);
        g_pSharedCtrl = nullptr;
    }
    if (g_hSharedMap) {
        CloseHandle(g_hSharedMap);
        g_hSharedMap = nullptr;
    }
}

} // namespace

// ============================================================================
// Exported Functions
// ============================================================================

SHELLHOOK_API LRESULT CALLBACK ExplorerCallWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && !IsUnloadRequested()) {
        EnsureInitialized();
        CheckClearCache();
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

SHELLHOOK_API void CALLBACK NotifyExplorerRefresh() {
    if (g_pSizeCalc) {
        g_pSizeCalc->ClearCache();
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

// ============================================================================
// DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    UNREFERENCED_PARAMETER(hModule);
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) {
            CleanupHooks();
        }
        break;
    }
    return TRUE;
}
