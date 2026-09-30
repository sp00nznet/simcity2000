@echo off
rem Configure and build sc2k with x86 clang-cl. Needs Visual Studio 2022 (or
rem Build Tools) with the x86 libraries, LLVM, CMake and Ninja.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem ProgramFiles(x86) is dropped when cmd is started from a POSIX shell.
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
cd /d "%~dp0.."
rem clang-cl targets x64 whatever vcvars says; the target has to be named.
if not exist build\build.ninja cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_C_COMPILER_TARGET=i686-pc-windows-msvc -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_CXX_COMPILER_TARGET=i686-pc-windows-msvc -DCMAKE_BUILD_TYPE=Release || exit /b 1
rem PCRECOMP names the pcrecomp checkout to build against (default ..\tools).
if defined PCRECOMP cmake -B build "-DPCRECOMP=%PCRECOMP%" >nul || exit /b 1
cmake --build build %*
