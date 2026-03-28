#pragma once

#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

// Shared display object — used by display.cpp; extern for any future module that needs it
extern Adafruit_SSD1306 display;

// Call once in setup() after Wire.begin()
void initDisplay();

// Splash screen on boot
void showLogo();

// FreeRTOS task — refreshes OLED every 500 ms
void taskDisplay(void *pvParameters);
