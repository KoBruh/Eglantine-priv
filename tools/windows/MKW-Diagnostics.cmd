@echo off
setlocal
set EGLANTINE_MKW_1X=1
set EDEN_NX2_GPU_PROFILE=1
start "" /D "%~dp0" "%~dp0eglantine.exe" %*
