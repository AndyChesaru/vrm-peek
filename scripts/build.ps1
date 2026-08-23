<#
    Builds VrmPeek.dll (x64) and the bundled web viewer into dist\VrmPeek.

    Requires: Visual Studio 2022 Build Tools with the C++ workload, and Node.js.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $SkipWeb,
    [switch] $Tools
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$native   = Join-Path $root 'src\native'
$web      = Join-Path $root 'src\web'
$sdk      = Join-Path $root 'external\webview2\build\native'
$objDir   = Join-Path $root "build\obj\$Configuration"
$outDir   = Join-Path $root 'dist\VrmPeek'
$dllPath  = Join-Path $outDir 'VrmPeek.dll'

# ---------------------------------------------------------------- toolchain
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools." }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ toolset was found." }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

if (-not (Test-Path (Join-Path $sdk 'include\WebView2.h'))) {
    throw "WebView2 SDK missing. Run scripts\fetch-deps.ps1 first."
}

New-Item -ItemType Directory -Force -Path $objDir, $outDir | Out-Null

# -------------------------------------------------------------------- web
if (-not $SkipWeb) {
    Write-Host 'Building web viewer...' -ForegroundColor Cyan
    if (-not (Test-Path (Join-Path $web 'node_modules'))) {
        Push-Location $web
        try { & npm install --silent; if ($LASTEXITCODE) { throw 'npm install failed' } }
        finally { Pop-Location }
    }
    Push-Location $web
    try { & node build.mjs; if ($LASTEXITCODE) { throw 'viewer bundle failed' } }
    finally { Pop-Location }
}

# ----------------------------------------------------------------- native
Write-Host "Building native handler ($Configuration, x64)..." -ForegroundColor Cyan

$cflags = @(
    '/nologo', '/c', '/EHsc', '/std:c++17', '/W4', '/permissive-',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
    '/D_CRT_SECURE_NO_WARNINGS',
    "/I`"$sdk\include`""
)
if ($Configuration -eq 'Release') { $cflags += @('/O2', '/MT', '/GL', '/DNDEBUG', '/GS') }
else                              { $cflags += @('/Od', '/MTd', '/Zi', '/D_DEBUG') }

$lflags = @(
    '/nologo', '/DLL', '/MACHINE:X64', '/DYNAMICBASE', '/NXCOMPAT',
    "/DEF:`"$native\VrmPeek.def`"",
    "/OUT:`"$dllPath`"",
    "/IMPLIB:`"$objDir\VrmPeek.lib`""
)
if ($Configuration -eq 'Release') { $lflags += @('/LTCG', '/OPT:REF', '/OPT:ICF', '/RELEASE') }
else                              { $lflags += @('/DEBUG', "/PDB:`"$outDir\VrmPeek.pdb`"") }

$libs = @(
    "`"$sdk\x64\WebView2LoaderStatic.lib`"",
    'shlwapi.lib', 'pathcch.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib',
    'user32.lib', 'gdi32.lib', 'advapi32.lib', 'version.lib', 'uuid.lib'
)

$sources = @('dllmain.cpp', 'PreviewHandler.cpp') | ForEach-Object { "`"$native\$_`"" }

# A running preview holds the DLL open in prevhost.exe and the link would fail.
Get-Process prevhost -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

# Stale objects from an interrupted run would be linked in blindly.
Get-ChildItem $objDir -Filter *.obj -EA SilentlyContinue | Remove-Item -Force

$bat = Join-Path $objDir 'build.bat'
@(
    '@echo off'
    "call `"$vcvars`" >nul || exit /b 1"
    "rc.exe /nologo /fo `"$objDir\VrmPeek.res`" `"$native\VrmPeek.rc`" || exit /b 1"
    "cl.exe $($cflags -join ' ') /Fo`"$objDir\\`" /Fd`"$objDir\vc.pdb`" $($sources -join ' ') || exit /b 1"
    "link.exe $($lflags -join ' ') `"$objDir\*.obj`" `"$objDir\VrmPeek.res`" $($libs -join ' ') || exit /b 1"
) | Set-Content -LiteralPath $bat -Encoding ASCII

& cmd.exe /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "Native build failed (exit $LASTEXITCODE)." }

$size = [math]::Round((Get-Item $dllPath).Length / 1KB)
Write-Host ""
Write-Host "  $dllPath  ($size KB)" -ForegroundColor Green

# ------------------------------------------------------------------- tools
if ($Tools) {
    Write-Host 'Building PreviewTest.exe...' -ForegroundColor Cyan
    $toolsOut = Join-Path $root 'build'
    $toolBat  = Join-Path $objDir 'build-tools.bat'
    @(
        '@echo off'
        "call `"$vcvars`" >nul || exit /b 1"
        ("cl.exe /nologo /EHsc /std:c++17 /W4 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN " +
         "/Fo`"$objDir\tools\\`" /Fe`"$toolsOut\PreviewTest.exe`" " +
         "`"$native\tools\PreviewTest.cpp`" /link /SUBSYSTEM:CONSOLE " +
         "shlwapi.lib ole32.lib oleaut32.lib user32.lib gdi32.lib uuid.lib || exit /b 1")
    ) | Set-Content -LiteralPath $toolBat -Encoding ASCII
    New-Item -ItemType Directory -Force -Path (Join-Path $objDir 'tools') | Out-Null
    & cmd.exe /c "`"$toolBat`""
    if ($LASTEXITCODE -ne 0) { throw "Tool build failed (exit $LASTEXITCODE)." }
    Write-Host "  $toolsOut\PreviewTest.exe" -ForegroundColor Green
}

Write-Host "  Next: .\scripts\install.ps1" -ForegroundColor DarkGray
