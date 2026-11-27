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

# ---------------------------------------------------------------------------
# DLSS NR runtimes (community builds, bundled like Veyra does). Each archive and
# the DLL inside are verified against pinned SHA-256 values before use.
# ---------------------------------------------------------------------------
$Community = Join-Path $Root 'third_party/sdk/community'
$NrBuilds = @(
    @{ Url = 'https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.Lecram/nvngx_dlssnr_310.8.Lecram.zip'
       ZipSha = 'ddb64e5545ba3f217c32555d2eb8fdfe76b9510d2256f13e8a5ecb1a9650ca54'
       DllSha = 'f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc'
       Target = '' },
    @{ Url = 'https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF-v2/nvngx_dlssnr_310.8.SF-v2.zip'
       ZipSha = '1da35941894994eb087e017577829e492454e9bae3a6a9397027069ceb74955c'
       DllSha = '6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927'
       Target = 'legacy' }
)

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

foreach ($b in $NrBuilds) {
    $dir = if ($b.Target) { Join-Path $Community $b.Target } else { $Community }
    $dll = Join-Path $dir 'nvngx_dlssnr.dll'
    if ((Test-Path $dll) -and (Get-Sha256 $dll) -eq $b.DllSha) { Write-Host "NR runtime ok: $dll"; continue }

    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $zip = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetFileName($b.Url))
    Invoke-WebRequest -Uri $b.Url -OutFile $zip -UseBasicParsing
    if ((Get-Sha256 $zip) -ne $b.ZipSha) { Remove-Item $zip; throw "Hash mismatch for $($b.Url)" }
    Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force
    Remove-Item $zip
    if ((Get-Sha256 $dll) -ne $b.DllSha) { Remove-Item $dll; throw "DLL hash mismatch for $($b.Url)" }
    Write-Host "NR runtime ready: $dll"
}
