@echo off
REM Build JessicaSAPI.dll (32-bit SAPI5 engine, static CRT) into the repo root.
REM Run from PowerShell:
REM     & build.bat

setlocal
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat" >nul

set "ROOT=%~dp0"
set "OBJ=%ROOT%src\obj"
if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%"

cl.exe /nologo /W3 /O2 /EHsc /MT /DUNICODE /D_UNICODE /I"%ROOT%src" ^
    /Fo"%OBJ%\\" /LD "%ROOT%src\JessicaSAPI.cpp" "%ROOT%src\cybit.c" ^
    /Fe"%ROOT%JessicaSAPI.dll" ^
    /link /DEF:"%ROOT%src\JessicaSAPI.def" /MACHINE:X86 /IMPLIB:"%OBJ%\JessicaSAPI.lib" ^
    ole32.lib oleaut32.lib uuid.lib advapi32.lib kernel32.lib user32.lib ^
    > "%OBJ%\build.log" 2>&1
set RC=%ERRORLEVEL%
type "%OBJ%\build.log"
rmdir /s /q "%OBJ%"

if %RC% equ 0 (
    for %%F in ("%ROOT%JessicaSAPI.dll") do echo Built: %%~fF ^(%%~zF bytes^)
) else (
    echo BUILD FAILED
)
echo Exit: %RC%
endlocal & exit /b %RC%
