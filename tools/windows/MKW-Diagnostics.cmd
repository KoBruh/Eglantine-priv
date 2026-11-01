@echo off
setlocal
set EDEN_NX2_GPU_PROFILE=1
start "" /D "%~dp0" "%~dp0eglantine.exe" %*
