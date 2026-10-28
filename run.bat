@echo off
rem 启动已打包的免安装版本（含 Qt 运行库，无需安装 Qt）
start "" "%~dp0dist\EcgRpeakDetector\EcgRpeakDetector.exe" %*
