<#
    Registers the VRM preview handler with Explorer.

    Per-user by default (no elevation, writes only to HKEY_CURRENT_USER).
    Pass -AllUsers to register for every account (must run elevated).

    The DLL is registered where it sits - do not move dist\VrmPeek afterwards.
    Run uninstall.ps1 first if you want to relocate it.
#>
[CmdletBinding()]
param(
    [string] $Path,
    [switch] $AllUsers,
    [switch] $EnableLogging
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Two layouts are supported: the repo, where this script lives in scripts\ and
# registers dist\VrmPeek, and the released zip, where it sits next to the DLL.
if (-not $Path) {
    $Path = if (Test-Path (Join-Path $PSScriptRoot 'VrmPeek.dll')) { $PSScriptRoot }
            else { Join-Path $root 'dist\VrmPeek' }
}
$Path   = (Resolve-Path -LiteralPath $Path).Path
$dll    = Join-Path $Path 'VrmPeek.dll'
$viewer = Join-Path $Path 'web\viewer.html'

if (-not (Test-Path $dll)) {
    throw "VrmPeek.dll not found in '$Path'. Build it with scripts\build.ps1, or pass -Path <folder holding VrmPeek.dll>."
}
if (-not (Test-Path $viewer)) {
    throw "web\viewer.html not found in '$Path'. The web folder must sit next to VrmPeek.dll."
}

$clsid      = '{EE2F8D4B-40E1-486F-B8DF-A51B16899142}'
$previewIid = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'   # IPreviewHandler
$surrogate  = '{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}'   # 64-bit prevhost.exe
$friendly   = 'VrmPeek'
$ext        = '.vrm'

if ($AllUsers) {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "-AllUsers requires an elevated PowerShell session."
    }
    $hive = 'HKLM:'
} else {
    $hive = 'HKCU:'
}

# ------------------------------------------------------------- prerequisites
$wv2 = @(
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
    'HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
    'HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}'
) | ForEach-Object { (Get-ItemProperty $_ -EA SilentlyContinue).pv } | Where-Object { $_ } | Select-Object -First 1

if (-not $wv2) {
    Write-Warning "The WebView2 Runtime was not detected. The preview pane will stay blank until it is installed:"
    Write-Warning "  https://developer.microsoft.com/microsoft-edge/webview2/"
} else {
    Write-Host "WebView2 Runtime $wv2" -ForegroundColor DarkGray
}

function Set-Key([string] $key, [string] $name, [string] $value) {
    if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
    New-ItemProperty -Path $key -Name $name -Value $value -PropertyType String -Force | Out-Null
}

# ------------------------------------------------------------------ register
$clsKey = "$hive\Software\Classes\CLSID\$clsid"
Set-Key $clsKey             '(default)'      $friendly
Set-Key $clsKey             'AppID'          $surrogate
Set-Key $clsKey             'DisplayName'    $friendly
Set-Key "$clsKey\InprocServer32" '(default)' $dll
Set-Key "$clsKey\InprocServer32" 'ThreadingModel' 'Apartment'

# Hook the extension directly and via SystemFileAssociations, so the handler
# survives whatever ProgID another application claims for .vrm.
Set-Key "$hive\Software\Classes\$ext\ShellEx\$previewIid" '(default)' $clsid
Set-Key "$hive\Software\Classes\SystemFileAssociations\$ext\ShellEx\$previewIid" '(default)' $clsid

# If something already owns .vrm, register under that ProgID too.
$progId = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\$ext" -EA SilentlyContinue).'(default)'
if ($progId -and (Test-Path "Registry::HKEY_CLASSES_ROOT\$progId")) {
    Set-Key "$hive\Software\Classes\$progId\ShellEx\$previewIid" '(default)' $clsid
    Write-Host "Also registered under ProgID '$progId'" -ForegroundColor DarkGray
}

# Explorer's approved-handler list.
Set-Key "$hive\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers" $clsid $friendly

# Opt-in tracing, written to %USERPROFILE%\AppData\LocalLow\VrmPeek\vrmpeek.log
if (-not (Test-Path 'HKCU:\Software\VrmPeek')) { New-Item -Path 'HKCU:\Software\VrmPeek' -Force | Out-Null }
New-ItemProperty -Path 'HKCU:\Software\VrmPeek' -Name 'Debug' `
    -Value $(if ($EnableLogging) { 1 } else { 0 }) -PropertyType DWord -Force | Out-Null
if ($EnableLogging) {
    Write-Host "Logging on -> $env:USERPROFILE\AppData\LocalLow\VrmPeek\vrmpeek.log" -ForegroundColor DarkGray
}

# ------------------------------------------------------------------- refresh
Get-Process prevhost -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

Add-Type -Namespace VrmPeek -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
'@
[VrmPeek.Shell]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED

Write-Host ""
Write-Host "Installed ($(if ($AllUsers) {'all users'} else {'current user'}))." -ForegroundColor Green
Write-Host "  Handler : $dll"
Write-Host "  Viewer  : $viewer"
Write-Host ""
Write-Host "In File Explorer turn on View > Show > Preview pane (Alt+P) and select a .vrm file." -ForegroundColor DarkGray
Write-Host "Close and reopen any folder window that was already open - Explorer caches its" -ForegroundColor DarkGray
Write-Host "preview host per window and will not pick up the change otherwise." -ForegroundColor DarkGray
