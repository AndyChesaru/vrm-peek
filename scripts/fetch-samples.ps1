<#
    Downloads the sample avatars used for manual testing into samples\.

    These are third-party models and are not redistributed in this repository.
    They come from the three-vrm and VRM specification sample sets and are used
    here only to exercise the viewer.

    samples\not-a-model.vrm and samples\empty-gltf.vrm are deliberately
    malformed fixtures kept in the repo; this script does not touch them.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'samples'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$models = @(
    @{ Name = 'three-vrm-girl-VRM0.vrm'
       Url  = 'https://raw.githubusercontent.com/pixiv/three-vrm/v0.6.11/packages/three-vrm/examples/models/three-vrm-girl.vrm'
       Note = 'VRM 0.x, pixiv Inc.' }
    @{ Name = 'VRM1_Constraint_Twist_Sample.vrm'
       Url  = 'https://raw.githubusercontent.com/pixiv/three-vrm/dev/packages/three-vrm/examples/models/VRM1_Constraint_Twist_Sample.vrm'
       Note = 'VRM 1.0, pixiv Inc.' }
    @{ Name = 'Seed-san.vrm'
       Url  = 'https://raw.githubusercontent.com/vrm-c/vrm-specification/master/samples/Seed-san/vrm/Seed-san.vrm'
       Note = 'VRM 1.0, VirtualCast Inc.' }
)

foreach ($m in $models) {
    $path = Join-Path $dest $m.Name
    if (Test-Path $path) {
        Write-Host ("skip  {0,-34} already present" -f $m.Name) -ForegroundColor DarkGray
        continue
    }
    Write-Host ("get   {0,-34} {1}" -f $m.Name, $m.Note) -ForegroundColor Cyan
    Invoke-WebRequest $m.Url -OutFile $path -TimeoutSec 300
}

Write-Host ""
Get-ChildItem $dest -Filter *.vrm | ForEach-Object {
    "  {0,-36} {1,8:N0} KB" -f $_.Name, ($_.Length / 1KB)
}
