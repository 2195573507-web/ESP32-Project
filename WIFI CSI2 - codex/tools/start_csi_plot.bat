@echo off
cd /d "%~dp0\.."
if exist "D:\ESP\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" (
    "D:\ESP\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" tools\csi_web_plot.py --baud 115200
) else (
    python tools\csi_web_plot.py --baud 115200
)
pause
