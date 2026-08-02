#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

static std::vector<std::wstring> g_logFormats;

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

static bool TestInitializationLogIsVersionNeutral() {
    g_logFormats.clear();

    if (!Check(Wh_ModInit() == TRUE, "Wh_ModInit should succeed")) {
        return false;
    }

    bool passed = Check(!g_logFormats.empty(), "Wh_ModInit should log initialization") &&
                  Check(g_logFormats.front() == L"Init",
                        "initialization log should be version-neutral");
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

static bool IsAbsolutePath(const std::wstring& path) {
    return !PathIsRelativeW(path.c_str());
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
    LaunchSpec scriptHostSpec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\build&deploy.vbs");

    std::wstring cscriptPath;
    if (!Check(ResolveSystemExecutablePath(L"cscript.exe", cscriptPath),
               "cscript.exe should resolve from the system directory")) {
        return false;
    }

    return Check(terminalSpec.parameters == L"/k cd /d \"C:\\R&D\"",
                 "cmd terminal target should always be quoted") &&
           Check(batchSpec.parameters ==
                     L"/k \"C:\\Scripts\\build&deploy.bat\"",
                 "batch script path should always be quoted") &&
           Check(scriptHostSpec.parameters.find(QuoteCmdPath(cscriptPath)) !=
                     std::wstring::npos,
                 "nested cscript path should always be quoted") &&
           Check(scriptHostSpec.parameters.find(
                     L"\"C:\\Scripts\\build&deploy.vbs\"") !=
                     std::wstring::npos,
                 "nested script path should always be quoted");
}

static bool TestKeyboardContextMenuSentinel() {
    return Check(IsKeyboardContextMenuPoint({-1, -1}),
                 "(-1, -1) should identify keyboard context-menu invocation") &&
           Check(!IsKeyboardContextMenuPoint({-1, 10}),
                 "only the complete keyboard sentinel should be accepted") &&
           Check(!IsKeyboardContextMenuPoint({10, -1}),
                 "ordinary screen points should not use selection fallback");
}

static bool TestDelayedMenuCommandKeepsState() {
    return Check(!ShouldClearMenuStateAfterTracking(0, TRUE),
                 "successful posted-command menus should retain target state") &&
           Check(ShouldClearMenuStateAfterTracking(0, FALSE),
                 "canceled posted-command menus should clear target state") &&
           Check(ShouldClearMenuStateAfterTracking(TPM_RETURNCMD, TRUE),
                 "return-command menus should clear target state immediately");
}

int main() {
    if (!TestInitializationLogIsVersionNeutral()) {
        return 1;
    }
    if (!TestCustomCommandReceivesSelectedFolder()) {
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
    if (!TestKeyboardContextMenuSentinel()) {
        return 1;
    }
    if (!TestDelayedMenuCommandKeepsState()) {
        return 1;
    }

    std::cout << "PASS: 12 tests\n";
    return 0;
}
