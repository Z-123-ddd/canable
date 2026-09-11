@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
cl /nologo /O2 /utf-8 /Fe:nsi_dfu_flash.exe nsi_dfu_flash.c ole32.lib
