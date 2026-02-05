@echo off
setlocal

set "ESP_IDF_PATH=%~1"
set "ESP_IDF_CURRENT_SETUP=%~2"
set "BUILD_DIR=%~3"
set "BOARD=%~4"
set "ACTION=%~5"
set "PORT=%~6"

if "%ESP_IDF_PATH%"=="" set "ESP_IDF_PATH=%ESP_IDF_CURRENT_SETUP%"
if "%ESP_IDF_PATH%"=="" set "ESP_IDF_PATH=%IDF_PATH%"

if "%BUILD_DIR%"=="" goto :usage
if "%BOARD%"=="" goto :usage
if "%ACTION%"=="" goto :usage

if "%ESP_IDF_PATH%"=="" (
  echo ESP-IDF path not set. Configure idf.espIdfPath/idf.currentSetup or IDF_PATH.
  exit /b 1
)

if not exist "%ESP_IDF_PATH%\export.bat" (
  echo Cannot find "%ESP_IDF_PATH%\export.bat"
  exit /b 1
)

call "%ESP_IDF_PATH%\export.bat" >nul
if errorlevel 1 exit /b %errorlevel%

if /I "%ACTION%"=="flash" (
  if "%PORT%"=="" (
    echo Serial port not set. Configure idf.portWin or pass a port.
    exit /b 1
  )
)

if /I "%ACTION%"=="monitor" (
  if "%PORT%"=="" (
    echo Serial port not set. Configure idf.portWin or pass a port.
    exit /b 1
  )
)

if "%PORT%"=="" (
  idf.py -B "%BUILD_DIR%" -DBOARD=%BOARD% %ACTION%
) else (
  idf.py -B "%BUILD_DIR%" -DBOARD=%BOARD% -p "%PORT%" %ACTION%
)

exit /b %errorlevel%

:usage
echo Usage: run_idf.cmd ^<esp_idf_path^> ^<esp_idf_current_setup^> ^<build_dir^> ^<board^> ^<action^> [port]
exit /b 2
