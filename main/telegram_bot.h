/*
 * telegram_bot.h
 *
 *  Created on: 26 авг. 2022 г.
 *      Author: chudnikov
 */

#ifndef MAIN_TELEGRAM_BOT_H_
#define MAIN_TELEGRAM_BOT_H_

#include "haier.h"

void TeleBot_Init(void);
void TeleBot_SetState(state_queue_t state);

#endif /* MAIN_TELEGRAM_BOT_H_ */
