@echo off
rem Windows MSVC build + ctest on a developer PC.
rem   tools\pc-check.cmd [nopull] [fast]
rem   Source: this checkout (or %TBCAD_SRC%); build folder: build\pc-release (or %TBCAD_OUT%).
rem   Pulls origin/main (fast-forward only) unless "nopull"; configures the build folder (Release,
rem   Ninja), builds, runs ctest -j4 (add "fast" to skip the build). Logs: pc-build.log and
rem   pc-ctest.log in the build folder; the last line of each is "done <exit code>";
rem   pc-summary.txt there holds the verdict.
setlocal
set "SRC=%TBCAD_SRC%"
if "%SRC%"=="" for %%I in ("%~dp0..") do set "SRC=%%~fI"
set "OUT=%TBCAD_OUT%"
if "%OUT%"=="" set "OUT=%SRC%\build\pc-release"
if not exist "%OUT%" mkdir "%OUT%"
set "DOPULL=1"
set "DOBUILD=1"
for %%A in (%*) do (
  if /i "%%A"=="nopull" set "DOPULL=0"
  if /i "%%A"=="fast" set "DOBUILD=0"
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%SRC%"
if "%DOPULL%"=="1" git pull --ff-only origin main
for /f %%H in ('git rev-parse --short HEAD') do set "HEAD=%%H"
if "%DOBUILD%"=="0" goto :test
cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release > "%OUT%\pc-cfg.log" 2>&1
cmake --build "%OUT%" > "%OUT%\pc-build.log" 2>&1
if errorlevel 1 goto :buildfail
echo done 0>> "%OUT%\pc-build.log"
goto :test
:buildfail
echo done 1>> "%OUT%\pc-build.log"
echo %HEAD% BUILD FAILED, see "%OUT%\pc-build.log"> "%OUT%\pc-summary.txt"
exit /b 1
:test
cd /d "%OUT%"
ctest --output-on-failure -j4 > "%OUT%\pc-ctest.log" 2>&1
set "CERR=%ERRORLEVEL%"
echo done %CERR%>> "%OUT%\pc-ctest.log"
findstr /c:"tests passed" /c:"tests failed" "%OUT%\pc-ctest.log" > "%OUT%\pc-summary.txt"
echo %HEAD% ctest exit %CERR%>> "%OUT%\pc-summary.txt"
exit /b %CERR%
