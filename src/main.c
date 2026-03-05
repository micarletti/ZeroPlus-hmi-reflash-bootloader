/*******************************************************************************
  Main Source File

  Company:
    Microchip Technology Inc.

  File Name:
    main.c

  Summary:
    This file contains the "main" function for a project.

  Description:
    This file contains the "main" function for a project.  The
    "main" function calls the "SYS_Initialize" function to initialize the state
    machines of all modules in the system
 *******************************************************************************/

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************

#include <stddef.h>      // Defines NULL
#include <stdbool.h>     // Defines true
#include <stdint.h>
#include <stdlib.h>      // Defines EXIT_FAILURE
#include "definitions.h" // SYS function prototypes
#include "UpdateStateMachine.h"

#define APP_START_GUARD_DELAY_MS    (5U)
#define CORE_TIMER_TICKS_PER_MS     (CPU_CLOCK_FREQUENCY / 2000U)

static void APP_StartGuardDelay(void)
{
    const uint32_t ticks = APP_START_GUARD_DELAY_MS * CORE_TIMER_TICKS_PER_MS;
    const uint32_t start = _CP0_GET_COUNT();

    while ((uint32_t)(_CP0_GET_COUNT() - start) < ticks)
    {
        /* allow EOF ACK from old bootloader to finish before UART re-init */
    }
}

// *****************************************************************************
// *****************************************************************************
// Section: Main Entry Point
// *****************************************************************************
// *****************************************************************************

int main(void)
{
    APP_StartGuardDelay();

    /* Initialize all modules */
    SYS_Initialize(NULL);

    /* Initialize startup reflash logic */
    UPD_Init();

    /* Perform one-shot bootloader rewrite, app CRC invalidation and reset */
    UPD_CheckBootMode();

    while (true)
    {
        SYS_Tasks();
        WDT_Clear();
    }

    /* Execution should not come here during normal operation */
    return (EXIT_FAILURE);
}

/*******************************************************************************
 End of File
*/
