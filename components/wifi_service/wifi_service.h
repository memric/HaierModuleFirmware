#ifndef WIFI_SERVICE_H_
#define WIFI_SERVICE_H_

#include <stdint.h>
#include "esp_err.h"

esp_err_t WifiServiceInit(void);
bool WifiServiceIsConnected(void);
uint32_t WifiServiceGetReconnectCounter(void);
void WifiServiceGetIpAddress(char *buffer, size_t buffer_size);
#endif /* WIFI_SERVICE_H_ */
