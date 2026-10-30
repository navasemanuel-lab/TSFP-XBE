@echo off
rem Runs the game logging every change of the controllers' input to out\build\release\logs\tsfp.log
set XBE_TRACE_INPUT=1
start "" /d "%~dp0..\out\build\release" "%~dp0..\out\build\release\TSFP.exe"
