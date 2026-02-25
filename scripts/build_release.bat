@REM Get args (same convention as ZeroPlus-hmi post-build script)
set IS_DEBUG=%1

if NOT "%IS_DEBUG%" == "false" (
    exit /b 0
)

set DISTDIR=%3
set PROJECTNAME=%4
set IMAGE_TYPE=%5

python ../scripts/generate_release_bin.py ^
    --elf "%DISTDIR%\%PROJECTNAME%.%IMAGE_TYPE%.elf" ^
    --release-dir "../Release" ^
    --version-file "../src/config/default/bootloader/bootloader_common.c"
