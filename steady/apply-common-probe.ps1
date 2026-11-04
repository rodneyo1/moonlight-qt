$ErrorActionPreference = 'Stop'
$probeRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$probeCommon = Join-Path $probeRoot 'moonlight-common-c/moonlight-common-c'
$probeBase = & git -C $probeCommon rev-parse HEAD
if ($LASTEXITCODE -or $probeBase -ne '04053843c90c4268583f2496b997d54d59f03957') { throw 'Unexpected common-c base; re-audit before applying the probe patch' }
$probePatch = Join-Path $PSScriptRoot 'common-c-probe.patch'
& git -C $probeCommon apply --check $probePatch
if ($LASTEXITCODE) { throw 'Probe patch does not apply cleanly' }
& git -C $probeCommon apply $probePatch
if ($LASTEXITCODE) { throw 'Probe patch failed' }
foreach ($probeName in @('SteadyProbe.h','SteadyProbeClient.inc')) {
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot $probeName) -Destination (Join-Path $probeCommon 'src')
}
$probeBuild = Join-Path $probeRoot 'build-baseline'
New-Item -ItemType Directory -Path $probeBuild -Force | Out-Null
[ordered]@{scope='Opt-in control-v2 calibration only; keyboard input remains legacy';base=$probeBase;patch_sha256=(Get-FileHash -LiteralPath $probePatch).Hash;header_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'SteadyProbe.h')).Hash;client_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'SteadyProbeClient.inc')).Hash} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $probeBuild 'steady-probe-sources.json') -Encoding UTF8
