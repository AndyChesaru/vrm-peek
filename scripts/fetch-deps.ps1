<#
    Downloads the WebView2 SDK (headers + static loader) into external\webview2.
    Only needed once, or after changing $Version. Requires internet access.
#>
[CmdletBinding()]
param([string] $Version = '1.0.4129.50')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'external\webview2'
$marker = Join-Path $dest 'build\native\include\WebView2.h'

if (Test-Path $marker) {
    Write-Host "WebView2 SDK already present in external\webview2" -ForegroundColor DarkGray
    Write-Host "Delete that folder to re-download." -ForegroundColor DarkGray
    return
}

New-Item -ItemType Directory -Force -Path (Join-Path $root 'external') | Out-Null
$nupkg = Join-Path $root "external\webview2-$Version.nupkg"
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"

Write-Host "Downloading WebView2 SDK $Version..." -ForegroundColor Cyan
Invoke-WebRequest $url -OutFile $nupkg -TimeoutSec 300

Expand-Archive $nupkg -DestinationPath $dest -Force
Remove-Item $nupkg -Force

if (-not (Test-Path $marker)) { throw "Extraction failed - WebView2.h not found under $dest" }
Write-Host "WebView2 SDK $Version -> $dest" -ForegroundColor Green
