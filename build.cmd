@echo off
cd /d "%~dp0"
if not exist bin mkdir bin
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
rc /nologo size.rc || exit /b 1
cl /nologo /std:c++17 /EHsc /utf-8 /O2 main.cpp size.res /Fe:bin\StickyNotes.exe /link /subsystem:windows user32.lib gdi32.lib comdlg32.lib


