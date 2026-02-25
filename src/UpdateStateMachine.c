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

#define BOOTLOADER_START_ADDRESS         (0x9FC00000U)
#define BOOTLOADER_MAX_SIZE_BYTES        (0x8000U)
#define BOOTLOADER_MIN_SIZE_BYTES        (1024U)
#define BOOTLOADER_VECTOR_REGION_BYTES   (0x1000U)
#define BOOTLOADER_QUADWORD_SIZE_BYTES   (16U)
#define BOOTLOADER_VERIFY_CHUNK_BYTES    (256U)
#define APP_HEADER_CRC_ADDRESS           (0x9D0FFFF8U)
#define CRC_INVALID_VALUE                (0x00000000U)
#define CORE_TIMER_TICKS_PER_MS          (CPU_CLOCK_FREQUENCY / 2000U)

#define LED_1_PIN                        GPIO_PIN_RF13
#define LED_2_PIN                        GPIO_PIN_RC3
#define LED_3_PIN                        GPIO_PIN_RC2

#define MODBUS_FN_READ_HOLDING           (0x03U)
#define MODBUS_CHECK_REGISTER            (0x0080U)
#define MODBUS_CHECK_QUANTITY            (0x0001U)
#define MODBUS_CHECK_REQ_SIZE            (8U)
#define MODBUS_CHECK_RESP_SIZE           (7U)
#define MODBUS_RESPONSE_VALUE            (0x0000U)
#define CHECK_RESPONSE_WAIT_TIMEOUT_MS   (15000U)

#define ADD_1_Get()                      ((PORTJ >> 11) & 0x1U)
#define ADD_2_Get()                      ((PORTH >> 8) & 0x1U)

#define PRE_FLASH_BLINK_COUNT            (10U)
#define PRE_FLASH_BLINK_MS               (200U)
#define POST_FLASH_BLINK_COUNT           (20U)
#define POST_FLASH_BLINK_MS              (100U)
#define ERROR_BLINK_ON_MS                (120U)
#define ERROR_BLINK_OFF_MS               (120U)
#define ERROR_PATTERN_GAP_MS             (900U)

typedef enum
{
    UPD_ERROR_NONE = 0,
    UPD_ERROR_IMAGE_INVALID = 1,
    UPD_ERROR_BOOT_ERASE_FAIL = 2,
    UPD_ERROR_BOOT_WRITE_FAIL = 3,
    UPD_ERROR_BOOT_VERIFY_FAIL = 4,
    UPD_ERROR_APP_CRC_INVALIDATE_FAIL = 5,
    UPD_ERROR_CHECK_RESPONSE_TX_FAIL = 6,
    UPD_ERROR_CHECK_NOT_OBSERVED = 7,
    UPD_ERROR_UNKNOWN = 8
} UPD_ErrorCode_t;

static bool g_reflashAttempted = false;
static bool g_checkUpdateReady = false;
static uint8_t g_displayAddress = 1U;
static uint8_t g_modbusReqFrame[MODBUS_CHECK_REQ_SIZE] = {0};
static uint8_t g_modbusReqIndex = 0U;
static uint32_t g_checkResponsesSent = 0U;
static UPD_ErrorCode_t g_lastError = UPD_ERROR_NONE;

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
static void UPD_LedsInit(void);
static void UPD_SetAllLeds(bool on);
static void UPD_DelayMs(uint32_t delayMs);
static void UPD_BlinkAllLeds(uint32_t blinkCount, uint32_t intervalMs);
static void UPD_DelayMsRaw(uint32_t delayMs);
static void UPD_InitDisplayAddress(void);
static void UPD_ServiceModbusCheckRequest(void);
static void UPD_ProcessModbusRxByte(uint8_t rxByte);
static void UPD_ModbusParserResetWithByte(uint8_t rxByte);
static bool UPD_SendModbusCheckResponse(uint16_t value);
static void UPD_ComputeModbusCrc(const uint8_t *data, uint32_t len, uint16_t *crc);
static uint32_t UPD_GetElapsedMs(uint32_t startCount);
static void UPD_SetError(UPD_ErrorCode_t errorCode);
static bool UPD_WaitForMainBootloaderCheck(void);
static void UPD_EnterErrorLoop(UPD_ErrorCode_t errorCode);

void UPD_Init(void)
{
    UPD_LedsInit();
    UPD_InitDisplayAddress();
    uBUS_EN_Clear();
}

void UPD_CheckBootMode(void)
{
    if (!g_reflashAttempted)
    {
        bool reflashed = false;
        bool crcInvalidated = false;

        g_reflashAttempted = true;
        /* Keep check-update responder active immediately, even during pre-flash LED pattern. */
        g_checkUpdateReady = true;
        g_modbusReqIndex = 0U;
        g_checkResponsesSent = 0U;
        g_lastError = UPD_ERROR_NONE;

        UPD_BlinkAllLeds(PRE_FLASH_BLINK_COUNT, PRE_FLASH_BLINK_MS);

        reflashed = UPD_ReflashBootloader();
        if (reflashed)
        {
            crcInvalidated = UPD_InvalidateAppHeaderCrc();
        }

        if (!reflashed)
        {
            UPD_EnterErrorLoop(g_lastError);
        }

        if (!crcInvalidated)
        {
            if (g_lastError == UPD_ERROR_NONE)
            {
                UPD_SetError(UPD_ERROR_APP_CRC_INVALIDATE_FAIL);
            }
            UPD_EnterErrorLoop(g_lastError);
        }

        if (!UPD_WaitForMainBootloaderCheck())
        {
            if (g_lastError == UPD_ERROR_NONE)
            {
                UPD_SetError(UPD_ERROR_CHECK_NOT_OBSERVED);
            }
            UPD_EnterErrorLoop(g_lastError);
        }

        if (reflashed && crcInvalidated)
        {
            UPD_BlinkAllLeds(POST_FLASH_BLINK_COUNT, POST_FLASH_BLINK_MS);
            __pic32_software_reset();
        }
    }
}

static bool UPD_ReflashBootloader(void)
{
    uint32_t imageSize = (uint32_t)sizeof(g_bootloaderImage);

    UPD_ServiceModbusCheckRequest();

    if (!UPD_IsBootloaderImageValid())
    {
        UPD_SetError(UPD_ERROR_IMAGE_INVALID);
        return false;
    }

    UPD_DisableBootFlashProtection();

    if (!UPD_EraseBootFlash(BOOTLOADER_START_ADDRESS, imageSize))
    {
        UPD_SetError(UPD_ERROR_BOOT_ERASE_FAIL);
        return false;
    }

    if (!UPD_WriteBootFlash(BOOTLOADER_START_ADDRESS, g_bootloaderImage, imageSize))
    {
        UPD_SetError(UPD_ERROR_BOOT_WRITE_FAIL);
        return false;
    }

    if (!UPD_VerifyBootFlash(BOOTLOADER_START_ADDRESS, g_bootloaderImage, imageSize))
    {
        UPD_SetError(UPD_ERROR_BOOT_VERIFY_FAIL);
        return false;
    }

    return true;
}

static bool UPD_IsBootloaderImageValid(void)
{
    bool hasVectorData = false;
    bool hasProgramData = false;
    uint32_t imageSize = (uint32_t)sizeof(g_bootloaderImage);

    if ((imageSize < BOOTLOADER_MIN_SIZE_BYTES) || (imageSize > BOOTLOADER_MAX_SIZE_BYTES))
    {
        return false;
    }

    if (imageSize <= BOOTLOADER_VECTOR_REGION_BYTES)
    {
        return false;
    }

    // check the reset vectoor is actually there
    for (uint32_t idx = 0U; idx < BOOTLOADER_VECTOR_REGION_BYTES; idx++)
    {
        if (g_bootloaderImage[idx] != 0xFFU)
        {
            hasVectorData = true;
            break;
        }
    }

    if (!hasVectorData)
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
            UPD_ServiceModbusCheckRequest();
            WDT_Clear();
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
            UPD_ServiceModbusCheckRequest();
            WDT_Clear();
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
        UPD_SetError(UPD_ERROR_APP_CRC_INVALIDATE_FAIL);
        return false;
    }

    while (NVM_IsBusy())
    {
        UPD_ServiceModbusCheckRequest();
        WDT_Clear();
    }

    if (NVM_ErrorGet() != NVM_ERROR_NONE)
    {
        UPD_SetError(UPD_ERROR_APP_CRC_INVALIDATE_FAIL);
        return false;
    }

    if (!NVM_Read(&readBack, sizeof(readBack), APP_HEADER_CRC_ADDRESS))
    {
        UPD_SetError(UPD_ERROR_APP_CRC_INVALIDATE_FAIL);
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

static void UPD_LedsInit(void)
{
    GPIO_PinOutputEnable(LED_1_PIN);
    GPIO_PinOutputEnable(LED_2_PIN);
    GPIO_PinOutputEnable(LED_3_PIN);
    UPD_SetAllLeds(false);
}

static void UPD_SetAllLeds(bool on)
{
    if (on)
    {
        GPIO_PinSet(LED_1_PIN);
        GPIO_PinSet(LED_2_PIN);
        GPIO_PinSet(LED_3_PIN);
    }
    else
    {
        GPIO_PinClear(LED_1_PIN);
        GPIO_PinClear(LED_2_PIN);
        GPIO_PinClear(LED_3_PIN);
    }
}

static void UPD_DelayMs(uint32_t delayMs)
{
    uint32_t ticks = delayMs * CORE_TIMER_TICKS_PER_MS;
    uint32_t start = _CP0_GET_COUNT();

    while ((uint32_t)(_CP0_GET_COUNT() - start) < ticks)
    {
        UPD_ServiceModbusCheckRequest();
        WDT_Clear();
    }
}

static void UPD_BlinkAllLeds(uint32_t blinkCount, uint32_t intervalMs)
{
    for (uint32_t i = 0U; i < blinkCount; i++)
    {
        UPD_SetAllLeds(true);
        UPD_DelayMs(intervalMs);
        UPD_SetAllLeds(false);
        UPD_DelayMs(intervalMs);
    }
}

static void UPD_DelayMsRaw(uint32_t delayMs)
{
    uint32_t ticks = delayMs * CORE_TIMER_TICKS_PER_MS;
    uint32_t start = _CP0_GET_COUNT();

    while ((uint32_t)(_CP0_GET_COUNT() - start) < ticks)
    {
        WDT_Clear();
    }
}

static void UPD_InitDisplayAddress(void)
{
    if (!ADD_1_Get() && !ADD_2_Get())
    {
        g_displayAddress = 2U;
    }
    else if (!ADD_1_Get() && ADD_2_Get())
    {
        g_displayAddress = 3U;
    }
    else if (ADD_1_Get() && ADD_2_Get())
    {
        g_displayAddress = 4U;
    }
    else
    {
        g_displayAddress = 1U;
    }
}

static void UPD_ServiceModbusCheckRequest(void)
{
    if ((U2STA & _U2STA_OERR_MASK) != 0U)
    {
        U2STACLR = _U2STA_OERR_MASK;
    }

    while ((U2STA & _U2STA_URXDA_MASK) != 0U)
    {
        uint8_t rxByte = (uint8_t)U2RXREG;
        UPD_ProcessModbusRxByte(rxByte);
    }
}

static void UPD_ProcessModbusRxByte(uint8_t rxByte)
{
    uint16_t crc = 0U;
    uint16_t requestCrc = 0U;

    if (g_modbusReqIndex == 0U)
    {
        if (rxByte == g_displayAddress)
        {
            g_modbusReqFrame[0] = rxByte;
            g_modbusReqIndex = 1U;
        }
        return;
    }

    g_modbusReqFrame[g_modbusReqIndex++] = rxByte;

    if ((g_modbusReqIndex == 2U) && (g_modbusReqFrame[1] != MODBUS_FN_READ_HOLDING))
    {
        UPD_ModbusParserResetWithByte(rxByte);
        return;
    }

    if ((g_modbusReqIndex == 3U) && (g_modbusReqFrame[2] != (uint8_t)(MODBUS_CHECK_REGISTER >> 8)))
    {
        UPD_ModbusParserResetWithByte(rxByte);
        return;
    }

    if ((g_modbusReqIndex == 4U) && (g_modbusReqFrame[3] != (uint8_t)(MODBUS_CHECK_REGISTER & 0xFFU)))
    {
        UPD_ModbusParserResetWithByte(rxByte);
        return;
    }

    if ((g_modbusReqIndex == 5U) && (g_modbusReqFrame[4] != (uint8_t)(MODBUS_CHECK_QUANTITY >> 8)))
    {
        UPD_ModbusParserResetWithByte(rxByte);
        return;
    }

    if ((g_modbusReqIndex == 6U) && (g_modbusReqFrame[5] != (uint8_t)(MODBUS_CHECK_QUANTITY & 0xFFU)))
    {
        UPD_ModbusParserResetWithByte(rxByte);
        return;
    }

    if (g_modbusReqIndex < MODBUS_CHECK_REQ_SIZE)
    {
        return;
    }

    UPD_ComputeModbusCrc(g_modbusReqFrame, MODBUS_CHECK_REQ_SIZE - 2U, &crc);
    requestCrc = (uint16_t)g_modbusReqFrame[MODBUS_CHECK_REQ_SIZE - 2U] |
                 ((uint16_t)g_modbusReqFrame[MODBUS_CHECK_REQ_SIZE - 1U] << 8);

    g_modbusReqIndex = 0U;

    if (crc != requestCrc)
    {
        return;
    }

    if (!g_checkUpdateReady)
    {
        return;
    }

    if (UPD_SendModbusCheckResponse(MODBUS_RESPONSE_VALUE))
    {
        g_checkResponsesSent++;
    }
}

static void UPD_ModbusParserResetWithByte(uint8_t rxByte)
{
    if (rxByte == g_displayAddress)
    {
        g_modbusReqFrame[0] = rxByte;
        g_modbusReqIndex = 1U;
    }
    else
    {
        g_modbusReqIndex = 0U;
    }
}

static bool UPD_SendModbusCheckResponse(uint16_t value)
{
    uint8_t response[MODBUS_CHECK_RESP_SIZE] = {0};
    uint16_t crc = 0U;

    response[0] = g_displayAddress;
    response[1] = MODBUS_FN_READ_HOLDING;
    response[2] = 0x02U;
    response[3] = (uint8_t)(value >> 8);
    response[4] = (uint8_t)(value & 0xFFU);

    UPD_ComputeModbusCrc(response, MODBUS_CHECK_RESP_SIZE - 2U, &crc);
    response[5] = (uint8_t)(crc & 0xFFU);
    response[6] = (uint8_t)(crc >> 8);

    uBUS_EN_Set();

    if (!UART2_Write(response, MODBUS_CHECK_RESP_SIZE))
    {
        UPD_SetError(UPD_ERROR_CHECK_RESPONSE_TX_FAIL);
        uBUS_EN_Clear();
        return false;
    }

    while (UART2_WriteIsBusy())
    {
        WDT_Clear();
    }

    while (!UART2_TransmitComplete())
    {
        WDT_Clear();
    }

    uBUS_EN_Clear();
    return true;
}

static void UPD_ComputeModbusCrc(const uint8_t *data, uint32_t len, uint16_t *crc)
{
    if ((data == NULL) || (crc == NULL))
    {
        return;
    }

    *crc = 0xFFFFU;

    for (uint32_t pos = 0U; pos < len; pos++)
    {
        *crc ^= (uint16_t)data[pos];

        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            if ((*crc & 0x0001U) != 0U)
            {
                *crc >>= 1U;
                *crc ^= 0xA001U;
            }
            else
            {
                *crc >>= 1U;
            }
        }
    }
}

static uint32_t UPD_GetElapsedMs(uint32_t startCount)
{
    uint32_t elapsedTicks = (uint32_t)(_CP0_GET_COUNT() - startCount);
    return (elapsedTicks / CORE_TIMER_TICKS_PER_MS);
}

static void UPD_SetError(UPD_ErrorCode_t errorCode)
{
    if (g_lastError == UPD_ERROR_NONE)
    {
        g_lastError = errorCode;
    }
}

static bool UPD_WaitForMainBootloaderCheck(void)
{
    uint32_t startCount = _CP0_GET_COUNT();

    while (UPD_GetElapsedMs(startCount) < CHECK_RESPONSE_WAIT_TIMEOUT_MS)
    {
        if (g_checkResponsesSent > 0U)
        {
            return true;
        }

        UPD_ServiceModbusCheckRequest();
        WDT_Clear();
    }

    return (g_checkResponsesSent > 0U);
}

static void UPD_EnterErrorLoop(UPD_ErrorCode_t errorCode)
{
    uint32_t flashes = (uint32_t)errorCode;

    if (flashes == 0U)
    {
        flashes = (uint32_t)UPD_ERROR_UNKNOWN;
    }

    uBUS_EN_Clear();

    while (true)
    {
        for (uint32_t i = 0U; i < flashes; i++)
        {
            UPD_SetAllLeds(true);
            UPD_DelayMsRaw(ERROR_BLINK_ON_MS);
            UPD_SetAllLeds(false);
            UPD_DelayMsRaw(ERROR_BLINK_OFF_MS);
        }

        UPD_DelayMsRaw(ERROR_PATTERN_GAP_MS);
    }
}
