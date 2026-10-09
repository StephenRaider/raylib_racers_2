@echo off
rem General Championship: add teams and build everything. Run this after dropping zips in GC\submissions
rem (or folders in GC\teams). Needs cmake and a C/C++ compiler (Visual Studio Build Tools or MinGW).
cd /d "%~dp0"
set B=build\cmake
cmake -S .. -B "%B%" -DCMAKE_BUILD_TYPE=Release -DRR_BUILD_BOTS=OFF || goto fail
cmake --build "%B%" --config Release --target rr_gc || goto fail
rr_gc.exe import
rr_gc.exe plan
cmake -S .. -B "%B%" || goto fail
cmake --build "%B%" --config Release --target rr_gc_host || goto fail
echo.
rr_gc.exe check --built
rr_gc.exe smoke
echo.
echo Done. Start GC_Race_viewer.exe.
pause
exit /b 0
:fail
echo.
echo BUILD FAILED. Scroll up for the first error.
pause
exit /b 1
