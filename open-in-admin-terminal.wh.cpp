// ==WindhawkMod==
// @id              open-in-admin-terminal
// @name            Open in Admin Terminal
// @description     Adds an Explorer classic context menu entry to open an elevated terminal in the current or selected folder.
// @version         1.10
// @author          aimagist
// @github          https://github.com/aimagist
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -luuid -lshlwapi -lshell32 -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Open in Admin Terminal

This mod adds an **Open in Admin Terminal** entry to Explorer's classic context
menu. It injects the menu item only while the menu is open and makes no
persistent registry changes.

By default it adds the entry in these places:

- Folder background in File Explorer
- Folder items
- Drive items

Notes:

- The command launches the selected terminal host elevated with UAC.
- On Windows 11, use **Show more options** to reach the classic menu.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- menuText: Open in Admin Terminal
  $name: Menu text
  $description: Leave empty to use a terminal-specific default label.
- terminalChoice: wt
  $name: Terminal type
  $description: Choose which terminal host to launch elevated.
  $options:
    - wt: Windows Terminal
    - pwsh: PowerShell 7
    - powershell: Windows PowerShell
    - cmd: Command Prompt
    - custom: Custom command
- customTerminalCommand: wt.exe
  $name: Custom terminal command
  $description: Used only when Terminal type is set to Custom command.
- showOnFolderBackground: true
  $name: Show on folder background
  $description: Add the entry when right-clicking empty space inside a folder.
- showOnFolderItem: true
  $name: Show on folder items
  $description: Add the entry when right-clicking a folder.
- showOnDriveItem: true
  $name: Show on drives
  $description: Add the entry when right-clicking a drive.
- position: Top
  $name: Menu position
  $description: Preferred position in the classic context menu.
  $options:
    - Top: Top
    - Bottom: Bottom
    - Default: Default
- appendTerminalName: false
  $name: Append terminal name
  $description: When Menu text is set, append the selected terminal name in parentheses.
- debugLogging: false
  $name: Debug logging
  $description: Log target detection, injection decisions, and successful launches.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shellapi.h>

#include <string>
#include <utility>
#include <vector>

struct Settings {
    std::wstring menuText;
    bool appendTerminalName;
    std::wstring terminalChoice;
    std::wstring customTerminalCommand;
    std::wstring terminalDisplayCommand;
    bool showOnFolderBackground;
    bool showOnFolderItem;
    bool showOnDriveItem;
    std::wstring position;
    bool debugLogging;
};

enum class TargetKind {
    None,
    FolderBackground,
    FolderItem,
    DriveItem,
};

struct MenuTarget {
    TargetKind kind = TargetKind::None;
    std::wstring path;
};

static Settings g_settings;
static SRWLOCK g_settingsLock = SRWLOCK_INIT;

static const UINT kMenuCommandId = 0xBF31;

static thread_local HWND g_currentMenuHwnd = nullptr;
static thread_local bool g_currentMenuEligible = false;
static thread_local MenuTarget g_currentTarget;

struct MenuBitmapCacheEntry {
    std::wstring key;
    HBITMAP bitmap;
};

static CLIPFORMAT g_shellIdListClipboardFormat = 0;
static SRWLOCK g_menuBitmapLock = SRWLOCK_INIT;
static std::vector<MenuBitmapCacheEntry> g_menuBitmapCache;

using TrackPopupMenuEx_t = BOOL(WINAPI*)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);
static TrackPopupMenuEx_t TrackPopupMenuEx_Orig;

using PostMessageW_t = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
static PostMessageW_t PostMessageW_Orig;

static std::wstring GetSettingString(PCWSTR name, const wchar_t* fallback) {
    std::wstring value = fallback;
    if (PCWSTR s = Wh_GetStringSetting(name)) {
        value = s;
        Wh_FreeStringSetting(s);
    }
    return value;
}

static bool GetSettingBool(PCWSTR name) {
    return Wh_GetIntSetting(name) != 0;
}

#define DEBUG_LOG(settings, ...) \
    do {                         \
        if ((settings).debugLogging) { \
            Wh_Log(__VA_ARGS__); \
        }                        \
    } while (false)

static std::wstring TrimString(const std::wstring& v) {
    auto first = v.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }

    auto last = v.find_last_not_of(L" \t\r\n");
    return v.substr(first, last - first + 1);
}

static std::wstring GetTerminalDisplayName(const Settings& s) {
    if (s.terminalChoice == L"wt") {
        return L"Windows Terminal";
    }
    if (s.terminalChoice == L"pwsh") {
        return L"PowerShell 7";
    }
    if (s.terminalChoice == L"powershell") {
        return L"Windows PowerShell";
    }
    if (s.terminalChoice == L"cmd") {
        return L"Command Prompt";
    }
    return L"Custom Terminal";
}

static Settings LoadSettings() {
    Settings s;
    s.menuText = TrimString(GetSettingString(L"menuText", L""));
    s.appendTerminalName = GetSettingBool(L"appendTerminalName");
    s.terminalChoice = GetSettingString(L"terminalChoice", L"wt");
    s.customTerminalCommand = GetSettingString(L"customTerminalCommand", L"wt.exe");
    s.showOnFolderBackground = GetSettingBool(L"showOnFolderBackground");
    s.showOnFolderItem = GetSettingBool(L"showOnFolderItem");
    s.showOnDriveItem = GetSettingBool(L"showOnDriveItem");
    s.position = GetSettingString(L"position", L"Top");
    s.debugLogging = GetSettingBool(L"debugLogging");

    if (s.terminalChoice == L"wt") {
        s.terminalDisplayCommand = L"wt.exe";
    } else if (s.terminalChoice == L"pwsh") {
        s.terminalDisplayCommand = L"pwsh.exe";
    } else if (s.terminalChoice == L"powershell") {
        s.terminalDisplayCommand = L"powershell.exe";
    } else if (s.terminalChoice == L"cmd") {
        s.terminalDisplayCommand = L"cmd.exe";
    } else {
        s.terminalDisplayCommand =
            s.customTerminalCommand.empty() ? L"wt.exe" : s.customTerminalCommand;
    }

    if (s.menuText.empty()) {
        s.menuText = L"Open " + GetTerminalDisplayName(s) + L" as Administrator";
    } else if (s.appendTerminalName) {
        s.menuText += L" (" + GetTerminalDisplayName(s) + L")";
    }

    return s;
}

static bool IsTargetEnabled(const Settings& s, TargetKind kind) {
    if (kind == TargetKind::FolderBackground) {
        return s.showOnFolderBackground;
    }
    if (kind == TargetKind::FolderItem) {
        return s.showOnFolderItem;
    }
    if (kind == TargetKind::DriveItem) {
        return s.showOnDriveItem;
    }
    return false;
}

static PCWSTR TargetKindName(TargetKind kind) {
    if (kind == TargetKind::FolderBackground) {
        return L"folder-background";
    }
    if (kind == TargetKind::FolderItem) {
        return L"folder-item";
    }
    if (kind == TargetKind::DriveItem) {
        return L"drive-item";
    }
    return L"none";
}

static bool IsDirectoryPath(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool IsDriveRootPath(const std::wstring& path) {
    return PathIsRootW(path.c_str()) != FALSE;
}

static std::wstring EscapePS(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\'') {
            out += L"''";
        } else {
            out += c;
        }
    }
    return out;
}

static std::wstring BuildStartProcess(const std::wstring& exe,
                                      const std::wstring& argLiteral) {
    std::wstring command =
        L"powershell.exe -NoProfile -WindowStyle Hidden -Command "
        L"\"Start-Process -FilePath '" + EscapePS(exe) + L"' -Verb RunAs";
    if (!argLiteral.empty()) {
        command += L" -ArgumentList " + argLiteral;
    }
    command += L"\"";
    return command;
}

static std::wstring BuildCommand(const Settings& s, const std::wstring& target) {
    if (s.terminalChoice == L"wt") {
        return BuildStartProcess(L"wt.exe", L"'-d','" + EscapePS(target) + L"'");
    }
    if (s.terminalChoice == L"pwsh") {
        return BuildStartProcess(
            L"pwsh.exe",
            L"'-NoExit','-Command','Set-Location -LiteralPath ''" +
                EscapePS(target) + L"'''");
    }
    if (s.terminalChoice == L"powershell") {
        return BuildStartProcess(
            L"powershell.exe",
            L"'-NoExit','-Command','Set-Location -LiteralPath ''" +
                EscapePS(target) + L"'''");
    }
    if (s.terminalChoice == L"cmd") {
        return BuildStartProcess(L"cmd.exe", L"'/k','cd /d \"" + target + L"\"'");
    }

    return BuildStartProcess(s.terminalDisplayCommand, {});
}

static IShellView* GetActiveShellViewForHwnd(HWND topLevel) {
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows,
                                  reinterpret_cast<void**>(&shellWindows));
    if (FAILED(hr) || !shellWindows) {
        return nullptr;
    }

    long count = 0;
    shellWindows->get_Count(&count);

    IShellView* result = nullptr;
    for (long i = 0; i < count && !result; i++) {
        VARIANT index = {};
        index.vt = VT_I4;
        index.lVal = i;

        IDispatch* dispatch = nullptr;
        if (FAILED(shellWindows->Item(index, &dispatch)) || !dispatch) {
            continue;
        }

        IWebBrowserApp* browser = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_IWebBrowserApp,
                                               reinterpret_cast<void**>(&browser))) &&
            browser) {
            HWND browserHwnd = nullptr;
            browser->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&browserHwnd));
            if (browserHwnd == topLevel) {
                IServiceProvider* serviceProvider = nullptr;
                if (SUCCEEDED(browser->QueryInterface(
                        IID_IServiceProvider,
                        reinterpret_cast<void**>(&serviceProvider))) &&
                    serviceProvider) {
                    IShellBrowser* shellBrowser = nullptr;
                    if (SUCCEEDED(serviceProvider->QueryService(
                            SID_STopLevelBrowser,
                            IID_IShellBrowser,
                            reinterpret_cast<void**>(&shellBrowser))) &&
                        shellBrowser) {
                        shellBrowser->QueryActiveShellView(&result);
                        shellBrowser->Release();
                    }
                    serviceProvider->Release();
                }
            }
            browser->Release();
        }
        dispatch->Release();
    }

    shellWindows->Release();
    return result;
}

static bool GetCurrentFolderPath(IFolderView* folderView, std::wstring& folderOut) {
    folderOut.clear();

    IPersistFolder2* persistFolder = nullptr;
    if (FAILED(folderView->GetFolder(IID_IPersistFolder2,
                                     reinterpret_cast<void**>(&persistFolder))) ||
        !persistFolder) {
        return false;
    }

    LPITEMIDLIST folderPidl = nullptr;
    bool ok = false;
    if (SUCCEEDED(persistFolder->GetCurFolder(&folderPidl)) && folderPidl) {
        WCHAR path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(folderPidl, path)) {
            folderOut = path;
            ok = true;
        }
        CoTaskMemFree(folderPidl);
    }

    persistFolder->Release();
    return ok;
}

static UINT GetSelectedPaths(IShellView* shellView,
                             std::vector<std::wstring>& selectionOut,
                             size_t maxPaths) {
    selectionOut.clear();
    if (maxPaths == 0 || !g_shellIdListClipboardFormat) {
        return 0;
    }

    IDataObject* dataObject = nullptr;
    if (FAILED(shellView->GetItemObject(SVGIO_SELECTION, IID_IDataObject,
                                        reinterpret_cast<void**>(&dataObject))) ||
        !dataObject) {
        return 0;
    }

    FORMATETC format = {
        g_shellIdListClipboardFormat,
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
    };
    STGMEDIUM medium = {};
    UINT selectedCount = 0;
    if (SUCCEEDED(dataObject->GetData(&format, &medium))) {
        CIDA* cida = static_cast<CIDA*>(GlobalLock(medium.hGlobal));
        if (cida) {
            selectedCount = cida->cidl;
            LPCITEMIDLIST parent =
                reinterpret_cast<LPCITEMIDLIST>(
                    reinterpret_cast<BYTE*>(cida) + cida->aoffset[0]);
            LPITEMIDLIST parentAbs = ILClone(parent);
            IShellFolder* folder = nullptr;
            if (parentAbs &&
                SUCCEEDED(SHBindToObject(nullptr, parentAbs, nullptr,
                                         IID_IShellFolder,
                                         reinterpret_cast<void**>(&folder))) &&
                folder) {
                UINT pathsToRead = cida->cidl;
                if (static_cast<size_t>(pathsToRead) > maxPaths) {
                    pathsToRead = static_cast<UINT>(maxPaths);
                }
                for (UINT i = 0; i < pathsToRead; i++) {
                    LPCITEMIDLIST child =
                        reinterpret_cast<LPCITEMIDLIST>(
                            reinterpret_cast<BYTE*>(cida) + cida->aoffset[i + 1]);
                    STRRET name = {};
                    if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING,
                                                           &name))) {
                        WCHAR path[MAX_PATH] = {};
                        if (SUCCEEDED(StrRetToBufW(&name, child, path, MAX_PATH)) &&
                            path[0]) {
                            selectionOut.push_back(path);
                        }
                    }
                }
                folder->Release();
            }
            if (parentAbs) {
                ILFree(parentAbs);
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
    }

    dataObject->Release();
    return selectedCount;
}

static bool ResolveMenuTarget(HWND hwnd, MenuTarget& targetOut) {
    targetOut = {};

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) {
        root = hwnd;
    }

    IShellView* shellView = GetActiveShellViewForHwnd(root);
    if (!shellView) {
        return false;
    }

    bool ok = false;
    IFolderView* folderView = nullptr;
    if (SUCCEEDED(shellView->QueryInterface(IID_IFolderView,
                                            reinterpret_cast<void**>(&folderView))) &&
        folderView) {
        int selectedCount = 0;
        bool hasSelectedCount =
            SUCCEEDED(folderView->ItemCount(SVGIO_SELECTION, &selectedCount));

        if (hasSelectedCount && selectedCount > 1) {
            ok = false;
        } else if (hasSelectedCount && selectedCount == 0) {
            std::wstring folderPath;
            GetCurrentFolderPath(folderView, folderPath);
            if (!folderPath.empty() && IsDirectoryPath(folderPath)) {
                targetOut.kind = TargetKind::FolderBackground;
                targetOut.path = folderPath;
                ok = true;
            }
        } else {
            std::vector<std::wstring> selectedPaths;
            UINT shellSelectedCount = GetSelectedPaths(shellView, selectedPaths, 2);

            if (selectedPaths.empty()) {
                if (!hasSelectedCount && shellSelectedCount == 0) {
                    std::wstring folderPath;
                    GetCurrentFolderPath(folderView, folderPath);
                    if (!folderPath.empty() && IsDirectoryPath(folderPath)) {
                        targetOut.kind = TargetKind::FolderBackground;
                        targetOut.path = folderPath;
                        ok = true;
                    }
                }
            } else if ((hasSelectedCount ? selectedCount == 1
                                         : shellSelectedCount == 1) &&
                       selectedPaths.size() == 1 &&
                       IsDirectoryPath(selectedPaths[0])) {
                targetOut.path = selectedPaths[0];
                targetOut.kind = IsDriveRootPath(targetOut.path) ? TargetKind::DriveItem
                                                                 : TargetKind::FolderItem;
                ok = true;
            }
        }

        folderView->Release();
    }

    shellView->Release();
    return ok;
}

static bool IsShellViewWindow(HWND hwnd) {
    HWND w = hwnd;
    while (w) {
        WCHAR className[64] = {};
        GetClassNameW(w, className, ARRAYSIZE(className));
        if (_wcsicmp(className, L"SHELLDLL_DefView") == 0) {
            return true;
        }

        HWND parent = GetParent(w);
        if (!parent) {
            parent = GetWindow(w, GW_OWNER);
        }
        w = parent;
    }
    return false;
}

static Settings GetSettingsSnapshot() {
    AcquireSRWLockShared(&g_settingsLock);
    Settings snapshot = g_settings;
    ReleaseSRWLockShared(&g_settingsLock);
    return snapshot;
}

static std::wstring TrimQuotes(const std::wstring& v) {
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') {
        return v.substr(1, v.size() - 2);
    }
    return v;
}

static bool ResolveExecutablePathForIcon(const Settings& settings, std::wstring& exeOut) {
    exeOut.clear();

    if (settings.terminalChoice == L"wt") {
        exeOut = L"wt.exe";
    } else if (settings.terminalChoice == L"pwsh") {
        exeOut = L"pwsh.exe";
    } else if (settings.terminalChoice == L"powershell") {
        exeOut = L"powershell.exe";
    } else if (settings.terminalChoice == L"cmd") {
        exeOut = L"cmd.exe";
    } else if (settings.terminalChoice == L"custom") {
        std::wstring candidate = TrimQuotes(TrimString(settings.customTerminalCommand));
        if (candidate.empty()) {
            return false;
        }
        DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            exeOut = candidate;
            return true;
        }
        return false;
    } else {
        return false;
    }

    WCHAR resolved[MAX_PATH] = {};
    DWORD len = SearchPathW(nullptr, exeOut.c_str(), nullptr, ARRAYSIZE(resolved), resolved,
                            nullptr);
    if (len == 0 || len >= ARRAYSIZE(resolved)) {
        return false;
    }
    exeOut = resolved;
    return true;
}

static HBITMAP CreateMenuBitmapFromIcon(HICON icon) {
    if (!icon) {
        return nullptr;
    }

    const int size = GetSystemMetrics(SM_CXMENUCHECK);
    if (size <= 0) {
        return nullptr;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        return nullptr;
    }
    HBITMAP bitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        DeleteObject(bitmap);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    if (bits) {
        ZeroMemory(bits, static_cast<SIZE_T>(size) * static_cast<SIZE_T>(size) * 4);
    }
    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    DrawIconEx(memDc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(memDc, oldBitmap);

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return bitmap;
}

static HBITMAP TryCreateMenuBitmapForTerminal(const Settings& settings) {
    std::wstring exePath;
    if (!ResolveExecutablePathForIcon(settings, exePath)) {
        return nullptr;
    }

    SHFILEINFOW shfi = {};
    if (!SHGetFileInfoW(exePath.c_str(), FILE_ATTRIBUTE_NORMAL, &shfi, sizeof(shfi),
                        SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        return nullptr;
    }

    HBITMAP bitmap = CreateMenuBitmapFromIcon(shfi.hIcon);
    DestroyIcon(shfi.hIcon);
    return bitmap;
}

static std::wstring GetMenuBitmapCacheKey(const Settings& settings) {
    return settings.terminalChoice + L"\n" + settings.terminalDisplayCommand;
}

static HBITMAP GetCachedMenuBitmapForTerminal(const Settings& settings) {
    std::wstring key = GetMenuBitmapCacheKey(settings);

    AcquireSRWLockShared(&g_menuBitmapLock);
    for (const auto& entry : g_menuBitmapCache) {
        if (entry.key == key) {
            HBITMAP bitmap = entry.bitmap;
            ReleaseSRWLockShared(&g_menuBitmapLock);
            return bitmap;
        }
    }
    ReleaseSRWLockShared(&g_menuBitmapLock);

    HBITMAP bitmap = TryCreateMenuBitmapForTerminal(settings);

    AcquireSRWLockExclusive(&g_menuBitmapLock);
    for (const auto& entry : g_menuBitmapCache) {
        if (entry.key == key) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            bitmap = entry.bitmap;
            ReleaseSRWLockExclusive(&g_menuBitmapLock);
            return bitmap;
        }
    }
    g_menuBitmapCache.push_back({std::move(key), bitmap});
    ReleaseSRWLockExclusive(&g_menuBitmapLock);

    return bitmap;
}

static void ClearMenuBitmapCache() {
    AcquireSRWLockExclusive(&g_menuBitmapLock);
    for (const auto& entry : g_menuBitmapCache) {
        if (entry.bitmap) {
            DeleteObject(entry.bitmap);
        }
    }
    g_menuBitmapCache.clear();
    ReleaseSRWLockExclusive(&g_menuBitmapLock);
}

static void ClearCurrentMenuState() {
    g_currentMenuEligible = false;
    g_currentTarget = {};
    g_currentMenuHwnd = nullptr;
}

static void InsertAdminTerminalMenuItem(HMENU menu, const Settings& settings) {
    int itemCount = GetMenuItemCount(menu);
    int insertPos = 0;
    bool separatorAbove = false;

    if (_wcsicmp(settings.position.c_str(), L"Bottom") == 0) {
        insertPos = itemCount < 0 ? 0 : itemCount;
        while (insertPos > 0) {
            MENUITEMINFOW itemInfo = {};
            itemInfo.cbSize = sizeof(itemInfo);
            itemInfo.fMask = MIIM_FTYPE;
            if (!GetMenuItemInfoW(menu, insertPos - 1, TRUE, &itemInfo) ||
                !(itemInfo.fType & MFT_SEPARATOR)) {
                break;
            }
            insertPos--;
        }
        separatorAbove = true;
    } else if (_wcsicmp(settings.position.c_str(), L"Default") == 0) {
        insertPos = 0;
        for (int i = 0; i < itemCount; i++) {
            WCHAR text[128] = {};
            if (GetMenuStringW(menu, i, text, ARRAYSIZE(text), MF_BYPOSITION) > 0 &&
                (StrStrIW(text, L"Open") || StrStrIW(text, L"Terminal"))) {
                insertPos = i + 1;
                break;
            }
        }
    } else {
        insertPos = 0;
    }

    if (separatorAbove) {
        InsertMenuW(menu, insertPos, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        InsertMenuW(menu, insertPos + 1, MF_BYPOSITION | MF_STRING, kMenuCommandId,
                    settings.menuText.c_str());
    } else {
        InsertMenuW(menu, insertPos, MF_BYPOSITION | MF_STRING, kMenuCommandId,
                    settings.menuText.c_str());
        InsertMenuW(menu, insertPos + 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    HBITMAP menuBitmap = GetCachedMenuBitmapForTerminal(settings);
    if (menuBitmap) {
        MENUITEMINFOW itemInfo = {};
        itemInfo.cbSize = sizeof(itemInfo);
        itemInfo.fMask = MIIM_BITMAP;
        itemInfo.hbmpItem = menuBitmap;
        if (!SetMenuItemInfoW(menu, kMenuCommandId, FALSE, &itemInfo)) {
            DEBUG_LOG(settings, L"Menu icon assignment failed");
        } else {
            DEBUG_LOG(settings, L"Menu icon assigned");
        }
    } else {
        DEBUG_LOG(settings, L"Menu icon unavailable");
    }
}

static void LaunchAdminTerminal(const MenuTarget& target) {
    Settings settings = GetSettingsSnapshot();
    std::wstring command = BuildCommand(settings, target.path);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};

    BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &startupInfo, &processInfo);
    if (ok) {
        DEBUG_LOG(settings, L"Launch succeeded: target=%ls command=%ls",
                  target.path.c_str(), command.c_str());
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    } else {
        Wh_Log(L"Launch failed: error=%lu target=%ls command=%ls",
               GetLastError(), target.path.c_str(), command.c_str());
    }
}

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU menu,
                                  UINT flags,
                                  int x,
                                  int y,
                                  HWND hwnd,
                                  LPTPMPARAMS params) {
    bool injected = false;
    ClearCurrentMenuState();

    if (menu && hwnd && IsShellViewWindow(hwnd)) {
        MenuTarget target;
        Settings settings = GetSettingsSnapshot();
        if (!ResolveMenuTarget(hwnd, target)) {
            DEBUG_LOG(settings, L"Injection skipped: no eligible filesystem directory target");
        } else if (!IsTargetEnabled(settings, target.kind)) {
            DEBUG_LOG(settings,
                      L"Injection skipped: target kind disabled kind=%ls path=%ls",
                      TargetKindName(target.kind), target.path.c_str());
        } else {
            DEBUG_LOG(settings, L"Injection target: kind=%ls path=%ls",
                      TargetKindName(target.kind), target.path.c_str());
            InsertAdminTerminalMenuItem(menu, settings);
            DEBUG_LOG(settings, L"Injection inserted: position=%ls text=%ls",
                      settings.position.c_str(), settings.menuText.c_str());
            injected = true;
            g_currentMenuHwnd = hwnd;
            g_currentMenuEligible = true;
            g_currentTarget = std::move(target);
        }
    }

    BOOL result = TrackPopupMenuEx_Orig(menu, flags, x, y, hwnd, params);

    if (injected && (flags & TPM_RETURNCMD) &&
        static_cast<UINT>(result) == kMenuCommandId) {
        MenuTarget target = g_currentTarget;
        ClearCurrentMenuState();
        LaunchAdminTerminal(target);
        return 0;
    }

    return result;
}

BOOL WINAPI PostMessageW_Hook(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND &&
        LOWORD(wParam) == kMenuCommandId &&
        g_currentMenuEligible &&
        hwnd == g_currentMenuHwnd) {
        MenuTarget target = g_currentTarget;
        ClearCurrentMenuState();
        LaunchAdminTerminal(target);
        return TRUE;
    }

    return PostMessageW_Orig(hwnd, message, wParam, lParam);
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init v1.10-classic");

    g_shellIdListClipboardFormat = RegisterClipboardFormatW(L"Shell IDList Array");

    AcquireSRWLockExclusive(&g_settingsLock);
    g_settings = LoadSettings();
    DEBUG_LOG(g_settings,
              L"Settings: background=%d folder=%d drive=%d terminal=%ls position=%ls",
              static_cast<int>(g_settings.showOnFolderBackground),
              static_cast<int>(g_settings.showOnFolderItem),
              static_cast<int>(g_settings.showOnDriveItem),
              g_settings.terminalChoice.c_str(),
              g_settings.position.c_str());
    ReleaseSRWLockExclusive(&g_settingsLock);

    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(TrackPopupMenuEx),
                            reinterpret_cast<void*>(TrackPopupMenuEx_Hook),
                            reinterpret_cast<void**>(&TrackPopupMenuEx_Orig))) {
        Wh_Log(L"Failed to hook TrackPopupMenuEx");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(reinterpret_cast<void*>(PostMessageW),
                            reinterpret_cast<void*>(PostMessageW_Hook),
                            reinterpret_cast<void**>(&PostMessageW_Orig))) {
        Wh_Log(L"Failed to hook PostMessageW");
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
    Settings settings = GetSettingsSnapshot();
    DEBUG_LOG(settings, L"Uninit");
    ClearMenuBitmapCache();
}

BOOL Wh_ModSettingsChanged(BOOL* reload) {
    *reload = FALSE;

    Settings newSettings = LoadSettings();
    AcquireSRWLockExclusive(&g_settingsLock);
    g_settings = std::move(newSettings);
    ReleaseSRWLockExclusive(&g_settingsLock);
    ClearMenuBitmapCache();

    return TRUE;
}
