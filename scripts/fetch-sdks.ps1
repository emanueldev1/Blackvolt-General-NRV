<#
.SYNOPSIS
  Downloads the NVIDIA DLSS SDK (headers, NGX static libs and redistributable DLLs)
  into third_party/sdk/dlss at the pinned commit. Never committed to git.
#>
$ErrorActionPreference = 'Stop'
$Commit = '9981c6350dc58c0ce21f024fd65e31e851c31689'
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root 'third_party/sdk/dlss'

$current = if (Test-Path (Join-Path $Dest '.git')) { git -C $Dest rev-parse HEAD } else { '' }
if ($current -eq $Commit) {
    Write-Host "DLSS SDK already at $Commit"
} else {
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    git -C $Dest init -q
    git -C $Dest remote add origin https://github.com/NVIDIA/DLSS.git
    git -C $Dest sparse-checkout set --no-cone /include/ /lib/Windows_x86_64/x64/ /lib/Windows_x86_64/rel/ /LICENSE.txt
    git -C $Dest fetch -q --depth 1 --filter=blob:none origin $Commit
    git -C $Dest checkout -q FETCH_HEAD
    Write-Host "DLSS SDK ready at $Dest ($Commit)"
}
