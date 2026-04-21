/*
 */

#ifndef MQTT_SERVICE_H_
#define MQTT_SERVICE_H_

#include <stdint.h>

#define MQTT_SERVICE_OK  0
#define MQTT_SERVICE_ERR 1

uint32_t MQTT_ServiceInit(void);
uint32_t MQTT_ServiceDeinit(void);
uint32_t MQTT_Send(const char *subt, const char *msg);

#endif /* MQTT_SERVICE_H_ */
