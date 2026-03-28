#pragma once

// ─── Libraries needed everywhere ─────────────────────────────────────────────
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ─── WiFi credentials ────────────────────────────────────────────────────────
#define WIFI_SSID     "Rohit"
#define WIFI_PASSWORD "12345678"

// ─── BLE UUIDs ───────────────────────────────────────────────────────────────
#define SERVICE_UUID   "12345678-1234-1234-1234-1234567890ab"
#define DATA_CHAR_UUID "00002a00-0000-1000-8000-00805f9b34fb"
#define DEVICE_NAME    "B-Fit Tracker"

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  32
#define OLED_RESET     -1
#define BUTTON_PIN     D1
#define ECG_PIN        34

// ─── LSTM model dimensions ───────────────────────────────────────────────────
#define LSTM_TIMESTEPS 10
#define LSTM_FEATURES   4   // [hr, spo2, accel_magnitude, steps]

// ─── Step detection tuning ───────────────────────────────────────────────────
#define STEP_DELAY_MS          300
#define STEP_ALPHA             0.1f
#define STEP_THRESHOLD_INIT    9.6f
#define STEP_THRESHOLD_INC     0.05f
#define STEP_THRESHOLD_DEC     0.01f
#define STEP_THRESHOLD_MIN     10.0f

// ─── Shared sensor state (defined in config.cpp) ─────────────────────────────
extern int   hr_max;
extern int   spO2;
extern float accel_magnitude;
extern int   stepCount;
extern float current_heart_rate;   // LSTM output
extern float input_buffer[LSTM_TIMESTEPS][LSTM_FEATURES];
extern int   input_buffer_idx;
extern volatile bool showHeart;
extern volatile int  screenMode;
extern bool deviceConnected;

// NTP formatted strings
extern char currentTime[11];
extern char currentDate[20];

// ─── Mutexes (defined in config.cpp) ─────────────────────────────────────────
extern SemaphoreHandle_t xMutex;        // guards LSTM buffer + current_heart_rate
extern SemaphoreHandle_t max30102Mutex; // guards hr_max, spO2
extern SemaphoreHandle_t stepMutex;     // guards stepCount

// ─── Shared utility ───────────────────────────────────────────────────────────
const char* getRiskLevel(int hr, int spo2);
