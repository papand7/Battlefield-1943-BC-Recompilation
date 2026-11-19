@echo off
setlocal EnableDelayedExpansion
rem Builds the dedicated server (no launcher) of Battlefield 1943 or, with --bfbc1, Battlefield:
rem Bad Company, from your own game files, on Windows. Run it once per game; then start_server.bat.
rem
rem   build_server.bat --game <game package | disc image .iso | default.xex | game folder> [--bfbc1]
rem
rem Needs, installed: the Visual Studio C++ build tools, LLVM (clang-cl), CMake, Ninja, Python 3 and the
rem Vulkan SDK (winget ids: Microsoft.VisualStudio.2022.BuildTools with the "Desktop development with
rem C++" workload, LLVM.LLVM, Kitware.CMake, Ninja-build.Ninja, Python.Python.3.12, KhronosGroup.VulkanSDK).
rem It is the build the launcher does, step by step. The game files go to bf1943-recomp\original\v106
rem (Bad Company: bfbc1-recomp\original\bfbc1). The server is the game's exe run with --dedicated.
set "HERE=%~dp0"
set "GAME="
set "BC=0"
:args
if "%~1"=="" goto argsdone
if /i "%~1"=="--game" (set "GAME=%~2" & shift & shift & goto args)
if /i "%~1"=="--bfbc1" (set "BC=1" & shift & goto args)
echo error: unknown option %~1
exit /b 1
:argsdone
if "!GAME!"=="" (echo error: --game is required, see the top of this file & exit /b 1)
if not exist "!GAME!" (echo error: no such file or folder: !GAME! & exit /b 1)

set "SRC=%HERE%bf1943-recomp"
if "%BC%"=="1" (
  set "TREE=%HERE%bfbc1-recomp"
  set "DATA=%HERE%bfbc1-recomp\original\bfbc1"
  set "UNPACK=xiso_extract.py"
  set "CFG=bfbc1_config"
  set "EXE=bfbc1.exe"
) else (
  set "TREE=%HERE%bf1943-recomp"
  set "DATA=%HERE%bf1943-recomp\original\v106"
  set "UNPACK=stfs_extract.py"
  set "CFG=bf1943_config"
  set "EXE=bf1943.exe"
)

rem 1. the toolchain
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSW!" (echo error: the Visual Studio C++ build tools are not installed & exit /b 1)
set "VSDIR="
for /f "usebackq delims=" %%i in (`""!VSW!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`) do set "VSDIR=%%i"
if "!VSDIR!"=="" (echo error: the Visual Studio C++ build tools are missing & exit /b 1)
call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\LLVM\bin;%ProgramFiles%\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
for %%t in (clang-cl cmake ninja python) do (
  where %%t >nul 2>&1 || (echo error: %%t was not found in PATH & exit /b 1)
)
if "%VULKAN_SDK%"=="" for /d %%d in (C:\VulkanSDK\*) do set "VULKAN_SDK=%%d"
if "%VULKAN_SDK%"=="" (echo error: the Vulkan SDK is not installed & exit /b 1)

rem 2. the game files: a folder or default.xex is copied, a package or disc image unpacked
if exist "!DATA!\default.xex" goto havegame
if exist "!GAME!\*" (
  echo Copying !GAME!
  xcopy /e /i /q /y "!GAME!" "!DATA!" >nul || exit /b 1
  goto havegame
)
for %%f in ("!GAME!") do set "GNAME=%%~nxf" & set "GDIR=%%~dpf"
if /i "!GNAME!"=="default.xex" (
  xcopy /e /i /q /y "!GDIR!." "!DATA!" >nul || exit /b 1
  goto havegame
)
echo Unpacking !GAME!
python -I "%SRC%\tools\!UNPACK!" "!GAME!" "!DATA!" || exit /b 1
:havegame
if not exist "!DATA!\default.xex" (echo error: no default.xex found in !DATA! & exit /b 1)


rem 3. the recompiler tools (XenonRecomp, XenonAnalyse, xex_dump) and the GPU shaders
set "RT=%SRC%\build\recomp-tools"
if not exist "!RT!\XenonRecomp\XenonRecomp\XenonRecomp.exe" (
  echo Building XenonRecomp...
  cmake -S "%SRC%\tools\xex_dump" -B "!RT!" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
  cmake --build "!RT!" --target XenonAnalyse XenonRecomp xex_dump || exit /b 1
)
if not exist "%SRC%\build\xenia_shaders" (
  echo Compiling the GPU shaders...
  python -I "%SRC%\tools\build_xenia_shaders.py" "%SRC%\thirdparty\xenia\src" "%SRC%\build\xenia_shaders" || exit /b 1
)

rem 4. the recompiler config and the game's code (generated/ is not in the package: made from your default.xex)
set "GEN=%TREE%\generated"
set "CTX=%SRC%\thirdparty\XenonRecomp\XenonUtils\ppc_context.h"
if not exist "!GEN!" mkdir "!GEN!"
if exist "!GEN!\!CFG!.toml" goto haveconfig
echo Making the recompiler config from your default.xex, this takes a few minutes...
set "TITLE=bf1943"
if "%BC%"=="1" set "TITLE=bfbc1"
python -I "%SRC%\tools\make_recomp_config.py" --title !TITLE! --xex "!DATA!\default.xex" --gen "!GEN!" --analyse "!RT!\XenonRecomp\XenonAnalyse\XenonAnalyse.exe" --recomp "!RT!\XenonRecomp\XenonRecomp\XenonRecomp.exe" --xex-dump "!RT!\xex_dump.exe" --ppc-context "!CTX!" || (del "!GEN!\!CFG!.toml" 2>nul & echo error: making the recompiler config failed & exit /b 1)
goto recompiled
:haveconfig
rem a config that is already there: with this PC's path to default.xex
python -I "%SRC%\tools\server_config.py" "!GEN!\!CFG!.toml" "!GEN!\!CFG!.local.toml" "!DATA!\default.xex" || exit /b 1
if exist "!GEN!\ppc\ppc_recomp_shared.h" goto recompiled
echo Recompiling the game code, this takes a few minutes...
if not exist "!GEN!\ppc" mkdir "!GEN!\ppc"
pushd "!GEN!"
"!RT!\XenonRecomp\XenonRecomp\XenonRecomp.exe" !CFG!.local.toml "!CTX:\=/!"
if errorlevel 1 (popd & echo error: XenonRecomp failed & exit /b 1)
popd
:recompiled


rem 5. build the program
set "ROOTARG="
if "%BC%"=="1" set "ROOTARG=-DROOT=%SRC:\=/%"
cmake -S "%TREE%\game" -B "%TREE%\build\game" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl !ROOTARG! || exit /b 1
cmake --build "%TREE%\build\game" || exit /b 1
if not exist "%TREE%\build\game\!EXE!" (echo error: the build made no !EXE! & exit /b 1)
echo.
echo Done. Start the server with: %TREE%\start_server.bat
