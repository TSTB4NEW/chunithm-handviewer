@echo off
rem ============================================================
rem  chunithm-handviewer build (Zig, no MSVC required)
rem  Usage: build.cmd
rem  Env:   set ZIG=<path to zig.exe>   (optional)
rem ============================================================
setlocal
cd /d "%~dp0"

set "ZIGEXE=%ZIG%"
if not defined ZIGEXE set "ZIGEXE=%~dp0..\_tools\ziglang\ziglang\zig.exe"
if exist "%ZIGEXE%" goto havezig
set "ZIGEXE=zig"
where zig >nul 2>nul
if errorlevel 1 (
  echo [ERROR] zig not found. Set ZIG=path\to\zig.exe
  exit /b 1
)
:havezig

set "ROOT=%~dp0.."
set "ZIG_GLOBAL_CACHE_DIR=%ROOT%\_tools\zig-global-cache"
set "ZIG_LOCAL_CACHE_DIR=%ROOT%\_tools\zig-local-cache"
set "TMP=%ROOT%\_tools\tmp"
set "TEMP=%TMP%"
if not exist "%TMP%" mkdir "%TMP%"

if not exist build mkdir build

echo == building chunithm-handviewer.exe ==
"%ZIGEXE%" c++ -target x86_64-windows-gnu -O2 -std=c++17 -municode -mwindows -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX src\main.cpp -o build\chunithm-handviewer.exe -lws2_32 -lhid -lsetupapi -lole32 -lshell32 -luser32 -lgdi32 -lwinmm
if errorlevel 1 exit /b 1

echo == copying runtime files ==
if not exist build\web mkdir build\web
copy /y web\index.html build\web\ >nul
if not exist build\handviewer.ini copy /y handviewer.ini build\handviewer.ini >nul

echo.
echo done: build\chunithm-handviewer.exe
