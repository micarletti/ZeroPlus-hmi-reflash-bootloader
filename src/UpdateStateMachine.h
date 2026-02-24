/**
 * @file UpdateStateMachine.h
 * @brief Startup bootloader reflash + app CRC invalidation API.
 */

#ifndef __UPDATE_STATE_MACHINE_H_
#define __UPDATE_STATE_MACHINE_H_



/**
 * @brief Runs the one-shot startup reflash procedure.
 */
void UPD_CheckBootMode(void);

/**
 * @brief Initializes the startup module state.
 */
void UPD_Init(void);

#endif /*__UPDATE_STATE_MACHINE_H_ */

/* EOF */

