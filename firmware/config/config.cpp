#include "config.h"

// ─── Shared sensor state ──────────────────────────────────────────────────────
int   hr_max            = 0;
int   spO2              = 99;
float accel_magnitude   = 0.0f;
int   stepCount         = 0;
float current_heart_rate = 0.0f;

float input_buffer[LSTM_TIMESTEPS][LSTM_FEATURES] = {0};
int   input_buffer_idx  = 0;

volatile bool showHeart      = false;
volatile int  screenMode     = 0;
bool          deviceConnected = false;

char currentTime[11] = "0000PM";
char currentDate[20] = "Sun 01Jan 2024";

// ─── Mutexes ──────────────────────────────────────────────────────────────────
SemaphoreHandle_t xMutex        = nullptr;
SemaphoreHandle_t max30102Mutex = nullptr;
SemaphoreHandle_t stepMutex     = nullptr;

// ─── Risk classifier ─────────────────────────────────────────────────────────
const char* getRiskLevel(int hr, int spo2) {
  if (hr < 40 || hr > 120 || spo2 < 90)
    return "H";
  if ((hr >= 40 && hr < 55) || (hr > 100 && hr <= 140) || (spo2 >= 90 && spo2 < 95))
    return "M";
  return "L";
}
