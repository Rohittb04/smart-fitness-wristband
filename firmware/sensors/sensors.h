#pragma once

#include "MAX30105.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// Sensor object handles — extern so display/inference can read raw data if needed
extern MAX30105         particleSensor;
extern Adafruit_MPU6050 mpu;

// Call once in setup() to initialise both sensors
void initSensors();

// FreeRTOS task functions — pass to xTaskCreatePinnedToCore()
void taskMAX30102(void *pvParameters);
void taskMPU6050 (void *pvParameters);
