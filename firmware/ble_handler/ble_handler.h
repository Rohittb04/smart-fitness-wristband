#pragma once

// Call once in setup() — initialises BLE server, service, characteristic, advertising
void initBLE();

// FreeRTOS task — sends CSV payload every second while a device is connected
void taskBLEUpdate(void *pvParameters);

// FreeRTOS task — updates NTP time every second
void taskTimeUpdate(void *pvParameters);
