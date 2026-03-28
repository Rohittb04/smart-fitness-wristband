// ─── B-Fit Tracker — main entry point ────────────────────────────────────────
// All logic lives in the modules below; setup() just wires them together.
//
// Module map:
//   config.h/cpp       — shared globals, mutexes, getRiskLevel()
//   time_utils.h/cpp   — WiFi + NTP
//   sensors.h/cpp      — MAX30102 (HR/SpO2) + MPU6050 (steps)
//   inference.h/cpp    — TFLite LSTM
//   display.h/cpp      — OLED (3 screen modes)
//   ble_handler.h/cpp  — BLE server + CSV notify task

#include "config.h"
#include "time_utils.h"
#include "sensors.h"
#include "inference.h"
#include "display.h"
#include "ble_handler.h"

// ─── Button ISR ───────────────────────────────────────────────────────────────
void IRAM_ATTR buttonPressed() {
  screenMode = (screenMode + 1) % 3;
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // 1. Display — first so we can show the logo immediately
  initDisplay();
  showLogo();

  // 2. Network + time
  connectWiFi();
  initTime();
  showTime();

  // 3. BLE
  initBLE();

  // 4. Sensors
  initSensors();

  // 5. Button
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  attachInterrupt(BUTTON_PIN, buttonPressed, RISING);

  // 6. TFLite LSTM
  initInference();

  // 7. Mutexes — must be created before any task starts
  xMutex        = xSemaphoreCreateMutex();
  max30102Mutex = xSemaphoreCreateMutex();
  stepMutex     = xSemaphoreCreateMutex();

  // 8. FreeRTOS tasks
  //                          function        name             stack  param  pri  handle  core
  xTaskCreatePinnedToCore(taskMAX30102,   "MAX30102",      4096,  NULL,  1,   NULL,   0);
  xTaskCreatePinnedToCore(taskMPU6050,    "MPU6050",       4096,  NULL,  1,   NULL,   0);
  xTaskCreatePinnedToCore(taskDisplay,    "Display",       4096,  NULL,  1,   NULL,   0);
  xTaskCreatePinnedToCore(InferenceTask,  "InferenceTask", 4096,  NULL,  1,   NULL,   1);
  xTaskCreatePinnedToCore(taskBLEUpdate,  "BLEUpdate",     8192,  NULL,  2,   NULL,   0);
  xTaskCreatePinnedToCore(taskTimeUpdate, "TimeUpdate",    4096,  NULL,  1,   NULL,   1);

  Serial.println("[Main] All tasks started");
}

void loop() {
  // FreeRTOS handles everything — loop() is intentionally idle
  vTaskDelay(pdMS_TO_TICKS(1000));
}
