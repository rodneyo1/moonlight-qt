$ErrorActionPreference='Stop'
$steadyFolder=$PSScriptRoot
$steadyClient=Join-Path $steadyFolder 'Moonlight.exe'
$steadyHost=Join-Path $steadyFolder 'sunshine.exe'
$steadyApp=if (Test-Path -LiteralPath $steadyClient) {$steadyClient} elseif (Test-Path -LiteralPath $steadyHost) {$steadyHost} else {throw 'Place this launcher beside Moonlight.exe or sunshine.exe'}
$steadyName=[IO.Path]::GetFileNameWithoutExtension($steadyApp)
if (Get-Process -Name $steadyName -ErrorAction SilentlyContinue) {throw "Close the running $steadyName first, then use this launcher. It must inherit the experimental settings at startup."}
$steadyTrace=Join-Path $env:USERPROFILE ('Downloads\steady-input-'+$steadyName+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'.csv')
$steadySavedEnable=$env:STEADY_INPUT_ENABLE
$steadySavedTrace=$env:STEADY_INPUT_TRACE
$steadySavedProbe=$env:STEADY_PROBE_ENABLE
try {
 $env:STEADY_INPUT_ENABLE='1'
 $env:STEADY_INPUT_TRACE=$steadyTrace
 $env:STEADY_PROBE_ENABLE='0'
 Write-Host 'Calibrated keyboard (protocol 2): do not type for the first ~35-60 s while the clock is calibrated; any key before commitment keeps this connection on ordinary input.'
 Write-Host 'Reserve: 15 ms (set STEADY_INPUT_PROFILE=standard for 25 ms). Validity is derived from the measured bounds; the stream closes at that expiry. Use a blank editor.'
 Write-Host ('Trace is saved after disconnect: '+$steadyTrace)
 if ($steadyName -eq 'Moonlight') { Start-Process -FilePath $steadyApp -WorkingDirectory $steadyFolder }
 else { Start-Process -FilePath $steadyApp -WorkingDirectory $steadyFolder -WindowStyle Hidden }
} finally {
 $env:STEADY_INPUT_ENABLE=$steadySavedEnable
 $env:STEADY_INPUT_TRACE=$steadySavedTrace
 $env:STEADY_PROBE_ENABLE=$steadySavedProbe
}
