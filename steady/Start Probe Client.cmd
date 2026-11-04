@echo off
set "STEADY_PROBE_ENABLE=1"
set "STEADY_PROBE_TRACE=%USERPROFILE%\Downloads\steady-clock-%RANDOM%-%RANDOM%.csv"
echo Calibration trace will be saved after disconnecting: "%STEADY_PROBE_TRACE%"
echo Connect to the matching probe-enabled Sunshine host. Keyboard input remains legacy.
echo Allow at least 3 minutes, then disconnect normally to export the full inventory.
"%~dp0Moonlight.exe"
