#ifndef HAIER_H_
#define HAIER_H_

#include <stdint.h>

typedef enum
{
    HAIER_OFF = 0,
    HAIER_ON,
    HAIER_STATE,
    HAIER_TEMP,
} cmd_t;

typedef struct
{
    uint8_t temp_curr; /*!< Current temp */
    uint8_t temp_set;  /*!< Set temperature */
    uint8_t mode;
    uint8_t fan_speed;
} state_queue_t;

uint32_t Haier_Init(void);
uint32_t Haier_Cmd(uint8_t c);

#endif /*HAIER_H_*/