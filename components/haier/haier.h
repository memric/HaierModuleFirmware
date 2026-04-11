#ifndef HAIER_H_
#define HAIER_H_

#include "freertos/FreeRTOS.h"
#include <stdint.h>

#define HAIER_ERR_OK      0
#define HAIER_ERR_PARSING 1

#define HAIER_CMD_STATE   0x01
#define HAIER_CMD_ON      0x02
#define HAIER_CMD_OFF     0x03

typedef enum
{
    HAIER_STATE = HAIER_CMD_STATE,
    HAIER_OFF   = HAIER_CMD_OFF,
    HAIER_ON    = HAIER_CMD_ON,
    HAIER_TEMP,
} cmd_t;

typedef struct
{
    uint8_t temp_curr; /*!< Current temp */
    uint8_t temp_set;  /*!< Set temperature */
    uint8_t mode;
    uint8_t fan_speed;
    uint8_t en; /*!< Enabled */
} haier_state_t;

uint32_t Haier_Init(void);
uint32_t Haier_Cmd(uint8_t c);
uint32_t Haier_GetState(haier_state_t *state, TickType_t timeout);

#endif /*HAIER_H_*/