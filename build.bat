@echo off
rem Mindriver build script - Release mode

if "%1"=="clean" goto :do_clean
if "%1"=="gcc" goto :build_gcc
if "%1"=="linux" goto :build_gcc

:build_msvc
echo [build] compiling mindriver with msvc (Release)...
if not exist "bin\out\windows" mkdir "bin\out\windows"
if not exist "bin\00Temp" mkdir "bin\00Temp"

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if exist %VCVARS% call %VCVARS%

set RCEXE="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe"
if not exist %RCEXE% set RCEXE=rc.exe


%RCEXE% /fo bin\00Temp\mindriver.res windows\mindriver.rc
cl.exe /nologo /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /W3 /MD /I. /Iwindows /Fobin\00Temp\ main.c windows\initcore.c windows\resolver.c windows\servicefunc.c bin\00Temp\mindriver.res /link /OUT:bin\out\windows\mindriver.exe ole32.lib user32.lib mfplat.lib mfreadwrite.lib mfuuid.lib avrt.lib

if errorlevel 1 goto :build_failed
echo [build] SUCCESS: bin\out\windows\mindriver.exe
goto :end

:build_gcc
echo [build] compiling mindriver with gcc (Release)...
if not exist "bin\out\windows" mkdir "bin\out\windows"
if not exist "bin\00Temp" mkdir "bin\00Temp"

set GCCEXE="C:\Dev-Cpp\bin\gcc.exe"
if not exist %GCCEXE% set GCCEXE=gcc

:: gcc objects redirect to  bin\00Temp
%GCCEXE% -O2 -DNDEBUG -Wall -I. -Iwindows -c main.c -o bin\00Temp\main.o
%GCCEXE% -O2 -DNDEBUG -Wall -I. -Iwindows -c windows\initcore.c -o bin\00Temp\initcore.o
%GCCEXE% -O2 -DNDEBUG -Wall -I. -Iwindows -c windows\resolver.c -o bin\00Temp\resolver.o
%GCCEXE% -O2 -DNDEBUG -Wall -I. -Iwindows -c windows\servicefunc.c -o bin\00Temp\servicefunc.o

%GCCEXE% bin\00Temp\main.o bin\00Temp\initcore.o bin\00Temp\resolver.o bin\00Temp\servicefunc.o -o bin\out\windows\mindriver.exe -lole32 -luser32 -lavrt

if errorlevel 1 goto :build_failed
echo [build] SUCCESS: bin\out\windows\mindriver.exe
goto :end

:do_clean
if exist "bin\out\windows\mindriver.exe" del /Q "bin\out\windows\mindriver.exe"
if exist "bin\00Temp" rmdir /S /Q "bin\00Temp"
echo [clean] done
goto :end

:build_failed
echo [build] FAILED with errorlevel %errorlevel%
exit /b 1

:end