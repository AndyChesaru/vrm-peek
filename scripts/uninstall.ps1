<#
    Removes every registry key install.ps1 creates. Leaves dist\VrmPeek on disk.
    Pass -AllUsers (elevated) to undo a machine-wide install.
#>
[CmdletBinding()]
param([switch] $AllUsers)

$ErrorActionPreference = 'Stop'

$clsid      = '{EE2F8D4B-40E1-486F-B8DF-A51B16899142}'
$previewIid = '{8895b1c6-b41f-4c1c-a562-0d564250836f}'
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

$removed = 0
$keys = @(
    "$hive\Software\Classes\CLSID\$clsid"
    "$hive\Software\Classes\$ext\ShellEx\$previewIid"
    "$hive\Software\Classes\SystemFileAssociations\$ext\ShellEx\$previewIid"
)

# Any ProgID we may have hooked at install time.
$progId = (Get-ItemProperty "Registry::HKEY_CLASSES_ROOT\$ext" -EA SilentlyContinue).'(default)'
if ($progId) { $keys += "$hive\Software\Classes\$progId\ShellEx\$previewIid" }

foreach ($k in $keys) {
    if (Test-Path $k) {
        Remove-Item -LiteralPath $k -Recurse -Force
        Write-Host "removed $k" -ForegroundColor DarkGray
        $removed++
    }
}

$handlers = "$hive\Software\Microsoft\Windows\CurrentVersion\PreviewHandlers"
if ((Get-ItemProperty $handlers -EA SilentlyContinue).PSObject.Properties.Name -contains $clsid) {
    Remove-ItemProperty -Path $handlers -Name $clsid -Force
    Write-Host "removed $handlers\$clsid" -ForegroundColor DarkGray
    $removed++
}

if (Test-Path 'HKCU:\Software\VrmPeek') {
    Remove-Item -LiteralPath 'HKCU:\Software\VrmPeek' -Recurse -Force
    Write-Host "removed HKCU:\Software\VrmPeek" -ForegroundColor DarkGray
    $removed++
}

Get-Process prevhost -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue

Add-Type -Namespace VrmPeek -Name Shell -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("shell32.dll")]
public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
'@
[VrmPeek.Shell]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)

Write-Host ""
if ($removed) { Write-Host "Uninstalled ($removed registry entries removed)." -ForegroundColor Green }
else          { Write-Host "Nothing to remove - the handler was not registered here." -ForegroundColor Yellow }
Write-Host "The WebView2 cache in %USERPROFILE%\AppData\LocalLow\VrmPeek can be deleted by hand." -ForegroundColor DarkGray
