@REM Get args (same convention as ZeroPlus-hmi post-build script)
set IS_DEBUG=%1

if NOT "%IS_DEBUG%" == "false" (
    exit /b 0
)

set MP_CC_DIR=%~2
set DISTDIR=%3
set PROJECTNAME=%4
set IMAGE_TYPE=%5

set RELEASE_PATH=..
set RELEASE_FOLDER_NAME=Release
set APP_NAME=ZP_DRFL01
set VERSION_FILE_PATH=..\src\config\default\bootloader\bootloader_common.c

@REM Generate raw binary from HEX (same as ZeroPlus-hmi v2.11 workflow)
"%MP_CC_DIR%\xc32-objcopy" -I ihex -O binary "%DISTDIR%\%PROJECTNAME%.%IMAGE_TYPE%.hex" "%DISTDIR%\%PROJECTNAME%.%IMAGE_TYPE%.bin"
if errorlevel 1 exit /b 1

@REM Remove old release folder if it exists
if exist "%RELEASE_PATH%\%RELEASE_FOLDER_NAME%" (
    rmdir /s /q "%RELEASE_PATH%\%RELEASE_FOLDER_NAME%"
)

@REM Create new release folder
mkdir "%RELEASE_PATH%\%RELEASE_FOLDER_NAME%" 2>nul

python ../scripts/generate_release_bin.py ^
    --input-bin "%DISTDIR%/%PROJECTNAME%.%IMAGE_TYPE%.bin" ^
    --output-prefix "%RELEASE_PATH%/%RELEASE_FOLDER_NAME%/%APP_NAME%" ^
    --version-file "%VERSION_FILE_PATH%"
if errorlevel 1 exit /b 1

@REM Keep fixed reflasher filename
for %%f in ("%RELEASE_PATH%\%RELEASE_FOLDER_NAME%\%APP_NAME%*.bin") do move /Y "%%f" "%RELEASE_PATH%\%RELEASE_FOLDER_NAME%\%APP_NAME%.bin" >nul
if errorlevel 1 exit /b 1
