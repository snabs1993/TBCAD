@echo off
rem Opens a DXF drawing: converts it to <name>.tbcad next to it, then opens the result in TBCAD.
rem Installed next to tbcad.exe; the "Open with TBCAD" entry of .dxf files runs it.
setlocal
if "%~1"=="" (
  echo Usage: tbcad-open-dxf.cmd file.dxf
  exit /b 2
)
"%~dp0tbcad-convert.exe" "%~1" -o "%~dpn1.tbcad"
if errorlevel 2 (
  echo.
  echo The DXF file could not be converted.
  pause
  exit /b 1
)
start "" "%~dp0tbcad.exe" "%~dpn1.tbcad"
