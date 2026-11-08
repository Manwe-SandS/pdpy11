@echo off
rem Build with MSVC (Developer Command Prompt). Produces pdpy11.exe
cl /nologo /O2 /std:c11 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:pdpy11.exe src\*.c shell32.lib winmm.lib
if "%1"=="test" (
  echo Run tests\run_golden.sh under a POSIX shell, or tools\difftest.py with Python.
)
