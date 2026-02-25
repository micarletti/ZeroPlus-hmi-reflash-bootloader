/**
 * @file UpdateStateMachine.c
 * @brief Startup bootloader reflash + app CRC invalidation + MCU reset.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "UpdateStateMachine.h"
#include "definitions.h"
#include "generated_bootloader_image.h"

#define BOOTLOADER_START_ADDRESS         (0x9FC01000U)
#define BOOTLOADER_MAX_SIZE_BYTES        (0x4000U)
#define BOOTLOADER_MIN_SIZE_BYTES        (1024U)
#define BOOTLOADER_QUADWORD_SIZE_BYTES   (16U)
#define BOOTLOADER_VERIFY_CHUNK_BYTES    (256U)
#define APP_HEADER_CRC_ADDRESS           (0x9D0FFFF8U)
#define CRC_INVALID_VALUE                (0x00000000U)

static bool g_reflashAttempted = false;

int __pic32_software_reset(void);

static bool UPD_IsBootloaderImageValid(void);
static bool UPD_ReflashBootloader(void);
static void UPD_DisableBootFlashProtection(void);
static bool UPD_EraseBootFlash(uint32_t address, uint32_t size);
static bool UPD_WriteBootFlash(uint32_t address, const uint8_t *data, uint32_t size);
static bool UPD_VerifyBootFlash(uint32_t address, const uint8_t *data, uint32_t size);
static bool UPD_InvalidateAppHeaderCrc(void);
static uint32_t UPD_AlignDown(uint32_t value, uint32_t alignment);
static uint32_t UPD_AlignUp(uint32_t value, uint32_t alignment);

void UPD_Init(void)
{
}

void UPD_CheckBootMode(void)
{
    if (!g_reflashAttempted)
    {
        g_reflashAttempted = true;

        (void)UPD_ReflashBootloader();
        (void)UPD_InvalidateAppHeaderCrc();

        __pic32_software_reset();
    }
}

static bool UPD_ReflashBootloader(void)
{
    uint32_t imageSize = (uint32_t)sizeof(g_bootloaderImage);

    if (!UPD_IsBootloaderImageValid())
    {
        return false;
    }

    UPD_DisableBootFlashProtection();

    if (!UPD_EraseBootFlash(BOOTLOADER_START_ADDRESS, imageSize))
    {
        return false;
    }

    if (!UPD_WriteBootFlash(BOOTLOADER_START_ADDRESS, g_bootloaderImage, imageSize))
    {
        return false;
    }

    return UPD_VerifyBootFlash(BOOTLOADER_START_ADDRESS, g_bootloaderImage, imageSize);
}

static bool UPD_IsBootloaderImageValid(void)
{
    bool hasProgramData = false;
    uint32_t imageSize = (uint32_t)sizeof(g_bootloaderImage);

    if ((imageSize < BOOTLOADER_MIN_SIZE_BYTES) || (imageSize > BOOTLOADER_MAX_SIZE_BYTES))
    {
        return false;
    }

    for (uint32_t idx = 0U; idx < imageSize; idx++)
    {
        if (g_bootloaderImage[idx] != 0xFFU)
        {
            hasProgramData = true;
            break;
        }
    }

    return hasProgramData;
}

static void UPD_DisableBootFlashProtection(void)
{
    uint32_t allBootPages =
        NVM_UPPER_BOOT_WRITE_PROTECT_0 |
        NVM_UPPER_BOOT_WRITE_PROTECT_1 |
        NVM_UPPER_BOOT_WRITE_PROTECT_2 |
        NVM_UPPER_BOOT_WRITE_PROTECT_3 |
        NVM_UPPER_BOOT_WRITE_PROTECT_4 |
        NVM_LOWER_BOOT_WRITE_PROTECT_0 |
        NVM_LOWER_BOOT_WRITE_PROTECT_1 |
        NVM_LOWER_BOOT_WRITE_PROTECT_2 |
        NVM_LOWER_BOOT_WRITE_PROTECT_3 |
        NVM_LOWER_BOOT_WRITE_PROTECT_4;

    NVM_BootFlashWriteProtectDisable((NVM_BOOT_FLASH_WRITE_PROTECT)allBootPages);
}

static bool UPD_EraseBootFlash(uint32_t address, uint32_t size)
{
    uint32_t eraseStart = UPD_AlignDown(address, NVM_FLASH_PAGESIZE);
    uint32_t eraseEnd = UPD_AlignUp(address + size, NVM_FLASH_PAGESIZE);

    for (uint32_t eraseAddress = eraseStart; eraseAddress < eraseEnd; eraseAddress += NVM_FLASH_PAGESIZE)
    {
        if (!NVM_PageErase(eraseAddress))
        {
            return false;
        }

        while (NVM_IsBusy())
        {
        }

        if (NVM_ErrorGet() != NVM_ERROR_NONE)
        {
            return false;
        }
    }

    return true;
}

static bool UPD_WriteBootFlash(uint32_t address, const uint8_t *data, uint32_t size)
{
    uint32_t offset = 0U;

    while (offset < size)
    {
        uint32_t quadWordData[4] = {0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU};
        uint32_t remaining = size - offset;
        uint32_t copySize = (remaining < BOOTLOADER_QUADWORD_SIZE_BYTES) ? remaining : BOOTLOADER_QUADWORD_SIZE_BYTES;

        (void)memcpy((void *)quadWordData, (const void *)(data + offset), copySize);

        if (!NVM_QuadWordWrite(quadWordData, address + offset))
        {
            return false;
        }

        while (NVM_IsBusy())
        {
        }

        if (NVM_ErrorGet() != NVM_ERROR_NONE)
        {
            return false;
        }

        offset += BOOTLOADER_QUADWORD_SIZE_BYTES;
    }

    return true;
}

static bool UPD_VerifyBootFlash(uint32_t address, const uint8_t *data, uint32_t size)
{
    uint8_t verifyBuffer[BOOTLOADER_VERIFY_CHUNK_BYTES] = {0};
    uint32_t offset = 0U;

    while (offset < size)
    {
        uint32_t remaining = size - offset;
        uint32_t chunkSize = (remaining < BOOTLOADER_VERIFY_CHUNK_BYTES) ? remaining : BOOTLOADER_VERIFY_CHUNK_BYTES;

        (void)memset(verifyBuffer, 0, chunkSize);

        if (!NVM_Read((uint32_t *)verifyBuffer, chunkSize, address + offset))
        {
            return false;
        }

        if (memcmp((const void *)verifyBuffer, (const void *)(data + offset), chunkSize) != 0)
        {
            return false;
        }

        offset += chunkSize;
    }

    return true;
}

static bool UPD_InvalidateAppHeaderCrc(void)
{
    uint32_t crcValue = CRC_INVALID_VALUE;
    uint32_t readBack = 0xFFFFFFFFU;

    if (!NVM_WordWrite(crcValue, APP_HEADER_CRC_ADDRESS))
    {
        return false;
    }

    while (NVM_IsBusy())
    {
    }

    if (NVM_ErrorGet() != NVM_ERROR_NONE)
    {
        return false;
    }

    if (!NVM_Read(&readBack, sizeof(readBack), APP_HEADER_CRC_ADDRESS))
    {
        return false;
    }

    return (readBack == crcValue);
}

static uint32_t UPD_AlignDown(uint32_t value, uint32_t alignment)
{
    return (value & ~(alignment - 1U));
}

static uint32_t UPD_AlignUp(uint32_t value, uint32_t alignment)
{
    return ((value + alignment - 1U) & ~(alignment - 1U));
}
