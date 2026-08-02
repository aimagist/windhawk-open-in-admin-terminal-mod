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
    settings.terminalDisplayCommand = L"pwsh.exe";
    settings.keepOpenAfterScript = true;
    settings.scriptExecutionPolicyBypass = true;

    LaunchSpec spec =
        BuildScriptLaunchSpec(settings, L"C:\\Scripts\\My Script.ps1");

    return Check(spec.executable == L"pwsh.exe",
                 "PowerShell script should use the selected interpreter") &&
           Check(spec.parameters ==
                     L"-NoExit -ExecutionPolicy Bypass -File "
                     L"\"C:\\Scripts\\My Script.ps1\"",
                 "PowerShell script should honor execution settings and quote its path") &&
           Check(spec.workingDirectory == L"C:\\Scripts",
                 "PowerShell script should use its parent directory");
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

    std::cout << "PASS: 4 tests\n";
    return 0;
}
