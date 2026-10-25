@echo off
rem Windows MSVC build + ctest on the owner's PC (DESKTOP-TN34IH3). For threads running there.
rem   tools\pc-check.cmd [nopull] [fast]
rem   Pulls origin/main (fast-forward only) unless "nopull"; configures D:\tb (Release, Ninja),
rem   builds, runs ctest -j4 (add "fast" to skip the build). Logs: D:\tb\pc-build.log, D:\tb\pc-ctest.log.
rem   Last line of each log is "done <exit code>"; D:\tb\pc-summary.txt holds the verdict.
setlocal
set "SRC=D:\TCAD"
set "OUT=D:\tb"
set "DOPULL=1"
set "DOBUILD=1"
for %%A in (%*) do (
  if /i "%%A"=="nopull" set "DOPULL=0"
  if /i "%%A"=="fast" set "DOBUILD=0"
)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %SRC%
if "%DOPULL%"=="1" git pull --ff-only origin main
for /f %%H in ('git rev-parse --short HEAD') do set "HEAD=%%H"
if "%DOBUILD%"=="0" goto :test
cmake -S . -B %OUT% -G Ninja -DCMAKE_BUILD_TYPE=Release > %OUT%\pc-cfg.log 2>&1
cmake --build %OUT% > %OUT%\pc-build.log 2>&1
if errorlevel 1 goto :buildfail
echo done 0>> %OUT%\pc-build.log
goto :test
:buildfail
echo done 1>> %OUT%\pc-build.log
echo %HEAD% BUILD FAILED, see %OUT%\pc-build.log> %OUT%\pc-summary.txt
exit /b 1
:test
cd /d %OUT%
ctest --output-on-failure -j4 > %OUT%\pc-ctest.log 2>&1
set "CERR=%ERRORLEVEL%"
echo done %CERR%>> %OUT%\pc-ctest.log
findstr /c:"tests passed" /c:"tests failed" %OUT%\pc-ctest.log > %OUT%\pc-summary.txt
echo %HEAD% ctest exit %CERR%>> %OUT%\pc-summary.txt
exit /b %CERR%
