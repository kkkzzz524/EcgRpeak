@echo off
rem ================================================================
rem  一键构建脚本：按需修改下面的 Qt / 编译器路径
rem ================================================================
setlocal

set QT_DIR=C:\Qt\6.12.0\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1310_64
set CMAKE_EXE=C:\Qt\Tools\CMake_64\bin\cmake.exe
set NINJA_DIR=C:\Qt\Tools\Ninja

set PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%NINJA_DIR%;%PATH%

cd /d "%~dp0"

if not exist "%CMAKE_EXE%" (
    echo [错误] 找不到 cmake：%CMAKE_EXE%，请修改脚本中的路径。
    pause
    exit /b 1
)

"%CMAKE_EXE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH=%QT_DIR% ^
    -DCMAKE_CXX_COMPILER=%MINGW_DIR%\bin\g++.exe
if errorlevel 1 goto :fail

"%CMAKE_EXE%" --build build
if errorlevel 1 goto :fail

echo.
echo 构建完成： build\EcgRpeakDetector.exe
exit /b 0

:fail
echo.
echo [失败] 构建未通过，请检查上面的错误信息。
pause
exit /b 1
