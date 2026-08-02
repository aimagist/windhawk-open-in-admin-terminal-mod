$compiler = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
$testSource = Join-Path $PSScriptRoot "mod_tests.cpp"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) "open-in-admin-terminal-tests-$PID"
$testExecutable = Join-Path $tempRoot "mod_tests.exe"
$compilerRoot = Split-Path (Split-Path $compiler)
$runtimeDirectory = Join-Path $compilerRoot "i686-w64-mingw32\bin"
$env:PATH = "$(Split-Path $compiler);$runtimeDirectory;$env:PATH"

New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    & $compiler -std=c++23 -static $testSource -o $testExecutable `
        -lole32 -loleaut32 -luuid -lshlwapi -lshell32 -lgdi32
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    & $testExecutable
    exit $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
