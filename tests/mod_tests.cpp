#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

static std::vector<std::wstring> g_logFormats;
static std::vector<void*> g_hookTargets;

extern "C" PCWSTR Wh_GetStringSetting(PCWSTR) {
    return L"";
}

extern "C" void Wh_FreeStringSetting(PCWSTR) {}

extern "C" int Wh_GetIntSetting(PCWSTR) {
    return 0;
}

extern "C" void Wh_Log(PCWSTR format, ...) {
    g_logFormats.emplace_back(format ? format : L"");
}

extern "C" BOOL Wh_SetFunctionHook(void* targetFunction,
                                     void*,
                                     void** originalFunction) {
    g_hookTargets.push_back(targetFunction);
    *originalFunction = targetFunction;
    return TRUE;
}

#include "../open-in-admin-terminal.wh.cpp"

static bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

static bool IsAbsolutePath(const std::wstring& path) {
    return !PathIsRelativeW(path.c_str());
}

static bool TestInitializationLogIsVersionNeutral() {
    g_logFormats.clear();
    g_hookTargets.clear();

    if (!Check(Wh_ModInit() == TRUE, "Wh_ModInit should succeed")) {
        return false;
    }

    bool passed = Check(!g_logFormats.empty(), "Wh_ModInit should log initialization") &&
                  Check(g_logFormats.front() == L"Init",
                        "initialization log should be version-neutral") &&
                  Check(g_hookTargets.size() == 1,
                        "initialization should hook only TrackPopupMenuEx");
    Wh_ModUninit();
    return passed;
}

static bool TestCustomCommandReceivesSelectedFolder() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"custom";
    settings.customTerminalCommand =
        L"\"C:\\Tools\\terminal.exe\" --cwd \"%V\" --target \"%1\"";

    LaunchSpec spec = BuildLaunchSpec(settings, L"C:\\Folder With Spaces");

    return Check(spec.executable == L"C:\\Tools\\terminal.exe",
                 "custom command should preserve its quoted executable") &&
           Check(spec.parameters ==
                     L"--cwd \"C:\\Folder With Spaces\" --target "
                     L"\"C:\\Folder With Spaces\"",
                 "custom command should expand %V and %1") &&
           Check(spec.workingDirectory == L"C:\\Folder With Spaces",
                 "custom command should use the selected folder as working directory");
}

static bool TestCustomPlaceholderExpansionIsSinglePass() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"custom";
    settings.customTerminalCommand =
        L"\"C:\\Tools\\terminal.exe\" --cwd \"%V\" --target %1";

    LaunchSpec nestedPlaceholder =
        BuildLaunchSpec(settings, L"C:\\Folder %1");

    settings.customTerminalCommand =
        L"\"C:\\Tools\\terminal.exe\" --cwd \"%V\"";
    LaunchSpec driveRoot = BuildLaunchSpec(settings, L"C:\\");

    return Check(nestedPlaceholder.parameters ==
                     L"--cwd \"C:\\Folder %1\" --target \"C:\\Folder %1\"",
                 "expanded target text should not be scanned for placeholders") &&
           Check(driveRoot.parameters == L"--cwd C:\\",
                 "quoted drive-root placeholders should not escape the closing quote");
}

static bool TestRelativeCustomCommandResolvesExecutable() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"custom";
    settings.customTerminalCommand = L"cmd.exe /d /c echo %V %1";

    LaunchSpec spec = BuildLaunchSpec(settings, L"C:\\Selected Folder");

    return Check(!spec.executable.empty(),
                 "relative custom executable should resolve") &&
           Check(IsAbsolutePath(spec.executable),
                 "relative custom executable should become an absolute path") &&
           Check(spec.parameters ==
                     L"/d /c echo \"C:\\Selected Folder\" "
                     L"\"C:\\Selected Folder\"",
                 "resolved custom command should preserve and expand arguments");
}

static bool TestUnresolvedCustomCommandFailsClosed() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"custom";
    settings.customTerminalCommand =
        L"open-in-admin-terminal-missing-test-executable.exe %V";

    LaunchSpec spec = BuildLaunchSpec(settings, L"C:\\Selected Folder");

    return Check(spec.executable.empty(),
                 "unresolved custom executable should fail closed");
}

static bool TestEmptyDisplayCommandUsesSystemCmd() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"cmd";

    LaunchSpec spec = BuildLaunchSpec(settings, L"C:\\Selected Folder");
    std::wstring cmdPath;
    if (!Check(ResolveSystemExecutablePath(L"cmd.exe", cmdPath),
               "cmd.exe should resolve from the system directory")) {
        return false;
    }

    return Check(spec.executable == cmdPath,
                 "empty display command should use the trusted system cmd path");
}

static bool TestWindowsTerminalQuotesSelectedFolder() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"wt";
    settings.terminalDisplayCommand = L"wt.exe";

    LaunchSpec spec = BuildLaunchSpec(settings, L"C:\\Folder With Spaces");

    return Check(spec.executable == L"wt.exe",
                 "Windows Terminal should use the resolved executable") &&
           Check(spec.parameters == L"-d \"C:\\Folder With Spaces\"",
                 "Windows Terminal should quote the selected folder") &&
           Check(spec.workingDirectory == L"C:\\Folder With Spaces",
                 "Windows Terminal should use the selected folder as working directory");
}

static bool TestPowerShellScriptHonorsExecutionSettings() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"pwsh";
    settings.terminalDisplayCommand = L"C:\\Program Files\\PowerShell\\7\\pwsh.exe";
    settings.keepOpenAfterScript = true;
    settings.scriptExecutionPolicyBypass = true;

    LaunchSpec spec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\My Script.ps1");

    return Check(spec.executable ==
                     L"C:\\Program Files\\PowerShell\\7\\pwsh.exe",
                 "PowerShell script should use the selected interpreter") &&
           Check(spec.parameters ==
                     L"-NoExit -ExecutionPolicy Bypass -File "
                     L"\"C:\\Scripts\\My Script.ps1\"",
                 "PowerShell script should honor execution settings and quote its path") &&
           Check(spec.workingDirectory == L"C:\\Scripts",
                 "PowerShell script should use its parent directory");
}

static bool TestScriptInterpretersUseTrustedAbsolutePaths() {
    Settings settings{};
    settings.keepOpenAfterScript = true;

    LaunchSpec cmdSpec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\Run Me.cmd");
    LaunchSpec scriptHostSpec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\Run Me.vbs");

    std::wstring cmdPath;
    std::wstring cscriptPath;
    if (!Check(ResolveSystemExecutablePath(L"cmd.exe", cmdPath),
               "cmd.exe should resolve from the system directory") ||
        !Check(ResolveSystemExecutablePath(L"cscript.exe", cscriptPath),
               "cscript.exe should resolve from the system directory")) {
        return false;
    }

    return Check(cmdSpec.executable == cmdPath,
                 "cmd.exe launch should use the trusted system path") &&
           Check(IsAbsolutePath(cscriptPath),
                 "cscript.exe should use an absolute path") &&
           Check(scriptHostSpec.parameters.find(
                     QuoteCommandLineArgument(cscriptPath)) != std::wstring::npos,
                 "nested cscript.exe command should quote its absolute path");
}

static bool TestPowerShellFallbackIsAbsolute() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"cmd";

    LaunchSpec spec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\Run Me.ps1");

    return Check(!spec.executable.empty(),
                 "PowerShell fallback should resolve an interpreter") &&
           Check(IsAbsolutePath(spec.executable),
                 "PowerShell fallback should never use a bare executable name");
}

static bool TestScriptExtensionSettingFiltersSupportedTypes() {
    Settings settings{};
    settings.scriptExtensions = L".PS1;.Js;.py";

    return Check(IsScriptExtension(L"C:\\Scripts\\test.ps1", settings),
                 "supported extension filtering should be case-insensitive") &&
           Check(IsScriptExtension(L"C:\\Scripts\\test.JS", settings),
                 "supported mixed-case extensions should remain enabled") &&
           Check(!IsScriptExtension(L"C:\\Scripts\\test.py", settings),
                 "unsupported extensions should not be added by the filter") &&
           Check(!IsScriptExtension(L"C:\\Scripts\\test.cmd", settings),
                 "supported extensions omitted from the filter should be disabled");
}

static bool TestScriptHostLabelIgnoresKeepOpenWrapper() {
    Settings settings{};
    settings.keepOpenAfterScript = true;

    return Check(GetScriptTerminalDisplayName(
                     settings, L"C:\\Scripts\\test.vbs") ==
                     L"Windows Script Host",
                 "VBS label should describe Windows Script Host") &&
           Check(GetScriptTerminalDisplayName(
                     settings, L"C:\\Scripts\\test.js") ==
                     L"Windows Script Host",
                 "JS label should describe Windows Script Host");
}

static bool TestPowerShellBypassCanBeDisabled() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"powershell";
    settings.terminalDisplayCommand =
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    settings.scriptExecutionPolicyBypass = false;

    LaunchSpec spec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\Run Me.ps1");

    return Check(spec.parameters.find(L"ExecutionPolicy") == std::wstring::npos,
                 "disabled execution-policy bypass should add no bypass arguments");
}

static bool TestCmdPathsAreAlwaysQuoted() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"cmd";
    settings.terminalDisplayCommand = L"C:\\Windows\\System32\\cmd.exe";
    settings.keepOpenAfterScript = true;

    LaunchSpec terminalSpec = BuildLaunchSpec(settings, L"C:\\R&D");
    LaunchSpec batchSpec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\build&deploy.bat");
    LaunchSpec programFilesBatchSpec = BuildScriptLaunchSpec(
        settings, L"C:\\Program Files (x86)\\App\\install.bat");
    LaunchSpec scriptHostSpec =
        BuildScriptLaunchSpec(settings, L"C:\\t\\a.vbs");
    settings.keepOpenAfterScript = false;
    LaunchSpec closedBatchSpec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\build&deploy.bat");

    std::wstring cscriptPath;
    if (!Check(ResolveSystemExecutablePath(L"cscript.exe", cscriptPath),
               "cscript.exe should resolve from the system directory")) {
        return false;
    }

    return Check(terminalSpec.parameters ==
                     L"/s /k \"cd /d \"C:\\R&D\"\"",
                 "cmd terminal command should preserve quoted metacharacters") &&
           Check(batchSpec.parameters ==
                     L"/s /k \"\"C:\\Scripts\\build&deploy.bat\"\"",
                 "batch metacharacters should remain inside inner quotes") &&
           Check(programFilesBatchSpec.parameters ==
                     L"/s /k \"\"C:\\Program Files (x86)\\App\\install.bat\"\"",
                 "batch paths with spaces and parentheses should remain quoted") &&
           Check(closedBatchSpec.parameters ==
                     L"/s /c \"\"C:\\Scripts\\build&deploy.bat\"\"",
                 "non-keep-open batch commands should preserve inner quotes") &&
           Check(scriptHostSpec.parameters ==
                     L"/s /k \"\"" + cscriptPath +
                         L"\" //nologo \"C:\\t\\a.vbs\"\"",
                 "keep-open script host command should preserve both inner paths");
}

static bool TestScriptHostWithoutKeepOpenBypassesCmd() {
    Settings settings{};
    settings.keepOpenAfterScript = false;

    LaunchSpec spec = BuildScriptLaunchSpec(settings, L"C:\\t\\a.vbs");
    std::wstring cscriptPath;
    if (!Check(ResolveSystemExecutablePath(L"cscript.exe", cscriptPath),
               "cscript.exe should resolve from the system directory")) {
        return false;
    }

    return Check(spec.executable == cscriptPath,
                 "script host without keep-open should launch cscript directly") &&
           Check(spec.parameters == L"//nologo C:\\t\\a.vbs",
                 "direct cscript arguments should not use cmd wrapping");
}

static bool TestCmdWrappedScriptsBypassTerminalHosts() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"wt";
    settings.terminalDisplayCommand = L"wt.exe";
    settings.keepOpenAfterScript = true;

    LaunchSpec batchSpec =
        BuildScriptLaunchSpec(settings, L"C:\\My Scripts\\Run Me.cmd");
    LaunchSpec scriptHostSpec =
        BuildScriptLaunchSpec(settings, L"C:\\My Scripts\\Run Me.vbs");
    settings.keepOpenAfterScript = false;
    LaunchSpec directScriptHostSpec =
        BuildScriptLaunchSpec(settings, L"C:\\My Scripts\\Run Me.vbs");

    std::wstring cmdPath;
    std::wstring cscriptPath;
    if (!Check(ResolveSystemExecutablePath(L"cmd.exe", cmdPath),
               "cmd.exe should resolve from the system directory") ||
        !Check(ResolveSystemExecutablePath(L"cscript.exe", cscriptPath),
               "cscript.exe should resolve from the system directory")) {
        return false;
    }

    return Check(batchSpec.executable == cmdPath,
                 "batch scripts should bypass terminal hosts") &&
           Check(batchSpec.parameters ==
                     L"/s /k \"\"C:\\My Scripts\\Run Me.cmd\"\"",
                 "direct batch launch should preserve cmd quoting") &&
           Check(scriptHostSpec.executable == cmdPath,
                 "keep-open script-host actions should bypass terminal hosts") &&
           Check(directScriptHostSpec.executable == L"wt.exe",
                 "plain argv-style script-host actions may use terminal hosts") &&
           Check(directScriptHostSpec.parameters.find(cscriptPath) !=
                     std::wstring::npos,
                 "terminal-hosted script actions should retain trusted cscript path");
}

static bool TestCmdWrappedScriptPresentationUsesInterpreter() {
    Settings settings{};
    settings.terminalEffectiveChoice = L"wt";
    settings.terminalDisplayCommand = L"wt.exe";
    settings.keepOpenAfterScript = true;

    std::wstring cmdPath;
    if (!Check(ResolveSystemExecutablePath(L"cmd.exe", cmdPath),
               "cmd.exe should resolve from the system directory")) {
        return false;
    }

    Settings batchIconSettings =
        GetScriptIconSettings(settings, L"C:\\Scripts\\Run.cmd");
    Settings scriptHostIconSettings =
        GetScriptIconSettings(settings, L"C:\\Scripts\\Run.vbs");
    return Check(GetScriptTerminalDisplayName(
                     settings, L"C:\\Scripts\\Run.cmd") == L"Command Prompt",
                 "batch labels should describe Command Prompt") &&
           Check(GetScriptTerminalDisplayName(
                     settings, L"C:\\Scripts\\Run.vbs") == L"Windows Script Host",
                 "keep-open script-host labels should describe Windows Script Host") &&
           Check(batchIconSettings.terminalDisplayCommand == cmdPath,
                 "batch icons should use the direct cmd executable") &&
           Check(scriptHostIconSettings.terminalDisplayCommand == cmdPath,
                 "keep-open script-host icons should use the direct cmd executable");
}

static bool TestNavigationPaneSelectionFallbackRules() {
    return Check(ShouldUseFocusedNavigationPaneFallback(false, true, false,
                                                        false),
                 "lost tracking points should fall back to focused navigation") &&
           Check(!ShouldUseFocusedNavigationPaneFallback(true, true, false,
                                                         false),
                 "navigation-pane points should remain hit-test-only") &&
           Check(!ShouldUseFocusedNavigationPaneFallback(false, true, true,
                                                         false),
                 "shell-view menus should never use navigation selection") &&
           Check(!ShouldUseFocusedNavigationPaneFallback(false, false, false,
                                                         false),
                 "fallback should require navigation-pane focus") &&
           Check(!ShouldUseFocusedNavigationPaneFallback(false, true, false,
                                                         true),
                 "Explorer chrome menus should not use navigation selection");
}

static bool TestKeyboardContextMenuSentinel() {
    return Check(IsKeyboardContextMenuPoint({-1, -1}),
                 "(-1, -1) should identify keyboard context-menu invocation") &&
           Check(!IsKeyboardContextMenuPoint({-1, 10}),
                 "only the complete keyboard sentinel should be accepted") &&
           Check(!IsKeyboardContextMenuPoint({10, -1}),
                 "ordinary screen points should not use selection fallback");
}

static bool TestNavigationPaneWindowUsesControlBeforeFallback() {
    HWND control = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
    HWND fallback = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));

    return Check(SelectNavigationPaneWindow(control, fallback) == control,
                 "the navigation control should provide its own coordinate window") &&
           Check(SelectNavigationPaneWindow(nullptr, fallback) == fallback,
                 "FCW_TREE should remain a fallback for older Explorer versions");
}

static bool TestMenuCommandIdsAvoidExistingEntries() {
    HMENU menu = CreatePopupMenu();
    if (!Check(menu != nullptr, "a popup menu should be created")) {
        return false;
    }

    InsertMenuW(menu, 0, MF_BYPOSITION | MF_STRING,
                kPreferredAdminMenuCommandId, L"existing");
    UINT admin = ReserveMenuCommandId(menu, kPreferredAdminMenuCommandId);
    UINT nonElevated = ReserveMenuCommandId(
        menu, kPreferredNonElevatedMenuCommandId, admin);
    DestroyMenu(menu);

    return Check(kPreferredAdminMenuCommandId != 0xBF31,
                 "command IDs should not overlap the catalog custom-items mod") &&
           Check(admin == kPreferredAdminMenuCommandId + 1,
                 "an occupied preferred ID should be skipped") &&
           Check(nonElevated != admin && nonElevated != 0,
                 "both inserted actions should have distinct available IDs");
}

static bool TestExplorerContextMatchingUsesTabSpecificWindows() {
    HWND view1 = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
    HWND view2 = reinterpret_cast<HWND>(static_cast<UINT_PTR>(2));
    HWND tab1 = reinterpret_cast<HWND>(static_cast<UINT_PTR>(3));
    HWND tab2 = reinterpret_cast<HWND>(static_cast<UINT_PTR>(4));

    return Check(IsExplorerContextMatch(view1, tab1, view1, tab1),
                 "the requested shell view should match itself") &&
           Check(!IsExplorerContextMatch(view1, tab1, view2, tab1),
                 "a different shell view in the same frame should be rejected") &&
           Check(IsExplorerContextMatch(nullptr, tab1, nullptr, tab1),
                 "navigation context should match the tab derived from the active view") &&
           Check(!IsExplorerContextMatch(nullptr, tab1, nullptr, tab2),
                 "navigation context should reject a tab derived from another view") &&
           Check(IsExplorerContextMatch(nullptr, nullptr, nullptr, nullptr),
                 "systems without tab discriminators should retain frame matching");
}

static bool TestMenuInjectionRequiresReturnCommand() {
    return Check(ShouldInjectForMenuFlags(TPM_RETURNCMD),
                 "return-command menus should allow injection") &&
           Check(ShouldInjectForMenuFlags(TPM_RETURNCMD | TPM_RIGHTBUTTON),
                 "other flags should preserve return-command injection") &&
           Check(!ShouldInjectForMenuFlags(0),
                 "menus without TPM_RETURNCMD should fail closed") &&
           Check(!ShouldInjectForMenuFlags(TPM_RIGHTBUTTON),
                 "unrelated flags should not enable injection");
}

static bool TestDesktopFolderFallbackRules() {
    return Check(ShouldUseDesktopFolderFallback(false, false, true),
                 "unresolved desktop background should use the desktop folder") &&
           Check(!ShouldUseDesktopFolderFallback(false, true, true),
                 "unresolved desktop selection should not use the desktop folder") &&
           Check(!ShouldUseDesktopFolderFallback(true, false, true),
                 "resolved desktop target should not use the desktop fallback") &&
           Check(!ShouldUseDesktopFolderFallback(false, false, false),
                 "unresolved non-desktop target should not use the desktop fallback");
}

int main() {
    if (!TestInitializationLogIsVersionNeutral()) {
        return 1;
    }
    if (!TestCustomCommandReceivesSelectedFolder()) {
        return 1;
    }
    if (!TestCustomPlaceholderExpansionIsSinglePass()) {
        return 1;
    }
    if (!TestRelativeCustomCommandResolvesExecutable()) {
        return 1;
    }
    if (!TestUnresolvedCustomCommandFailsClosed()) {
        return 1;
    }
    if (!TestEmptyDisplayCommandUsesSystemCmd()) {
        return 1;
    }
    if (!TestWindowsTerminalQuotesSelectedFolder()) {
        return 1;
    }
    if (!TestPowerShellScriptHonorsExecutionSettings()) {
        return 1;
    }
    if (!TestScriptInterpretersUseTrustedAbsolutePaths()) {
        return 1;
    }
    if (!TestPowerShellFallbackIsAbsolute()) {
        return 1;
    }
    if (!TestScriptExtensionSettingFiltersSupportedTypes()) {
        return 1;
    }
    if (!TestScriptHostLabelIgnoresKeepOpenWrapper()) {
        return 1;
    }
    if (!TestPowerShellBypassCanBeDisabled()) {
        return 1;
    }
    if (!TestCmdPathsAreAlwaysQuoted()) {
        return 1;
    }
    if (!TestScriptHostWithoutKeepOpenBypassesCmd()) {
        return 1;
    }
    if (!TestCmdWrappedScriptsBypassTerminalHosts()) {
        return 1;
    }
    if (!TestCmdWrappedScriptPresentationUsesInterpreter()) {
        return 1;
    }
    if (!TestNavigationPaneSelectionFallbackRules()) {
        return 1;
    }
    if (!TestKeyboardContextMenuSentinel()) {
        return 1;
    }
    if (!TestNavigationPaneWindowUsesControlBeforeFallback()) {
        return 1;
    }
    if (!TestMenuCommandIdsAvoidExistingEntries()) {
        return 1;
    }
    if (!TestExplorerContextMatchingUsesTabSpecificWindows()) {
        return 1;
    }
    if (!TestMenuInjectionRequiresReturnCommand()) {
        return 1;
    }
    if (!TestDesktopFolderFallbackRules()) {
        return 1;
    }

    std::cout << "PASS: 24 tests\n";
    return 0;
}
