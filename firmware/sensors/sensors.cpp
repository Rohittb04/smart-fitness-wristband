#include "sensors.h"
#include "config.h"
#include "heartRate.h"   // checkForBeat()

// ─── Sensor objects ────────────────────────────────────────────────────────────
MAX30105         particleSensor;
Adafruit_MPU6050 mpu;

// ─── File-local step-detection state ──────────────────────────────────────────
static float         filteredMagnitude = 0.0f;
static float         dynamicThreshold  = STEP_THRESHOLD_INIT;
static bool          stepDetected      = false;
static unsigned long lastStepTime      = 0;

// ─────────────────────────────────────────────────────────────────────────────
void initSensors() {
  particleSensor.begin(Wire, I2C_SPEED_FAST);
  particleSensor.setup();
  if (!mpu.begin()) {
    Serial.println("[MPU6050] Init failed!");
  }
  Serial.println("[Sensors] Initialised");
}

// ─── SpO2 estimate (file-local helper) ───────────────────────────────────────
static int calculateSpO2() {
  long ir  = particleSensor.getIR();
  long red = particleSensor.getRed();
  if (red < 28000 || ir < 50000) return 0;
  float ratio = (float)red / ir;
  return constrain(108 - (int)(25 * ratio), 85, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: MAX30102 — heart rate + SpO2
// ─────────────────────────────────────────────────────────────────────────────
void taskMAX30102(void *pvParameters) {
  static unsigned long lastBeat = 0;
  static int           beatAvg  = 0;

  while (1) {
    long irValue = particleSensor.getIR();

    if (irValue < 50000) {
      // Finger lifted — reset readings
      if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
        beatAvg = 0;
        hr_max  = 0;
        spO2    = 0;
        xSemaphoreGive(max30102Mutex);
      }

    } else if (checkForBeat(irValue)) {
      unsigned long delta = millis() - lastBeat;
      lastBeat  = millis();
      showHeart = true;

      if (delta > 0) {
        float bpm = 60000.0f / delta;
        if (bpm >= 20 && bpm <= 255) {
          beatAvg = (int)(beatAvg * 0.83f + bpm * 0.17f);

          if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
            hr_max = beatAvg;
            spO2   = calculateSpO2();
            xSemaphoreGive(max30102Mutex);
          }

          // ── Feed new row into LSTM circular buffer ────────────────────────
          if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            int steps_snap = 0;
            if (xSemaphoreTake(stepMutex, 0)) {   // non-blocking attempt
              steps_snap = stepCount;
              xSemaphoreGive(stepMutex);
            }
            input_buffer[input_buffer_idx][0] = (float)beatAvg;
            input_buffer[input_buffer_idx][1] = (float)spO2;
            input_buffer[input_buffer_idx][2] = accel_magnitude;
            input_buffer[input_buffer_idx][3] = (float)steps_snap;
            input_buffer_idx = (input_buffer_idx + 1) % LSTM_TIMESTEPS;
            xSemaphoreGive(xMutex);
          }
        }
      }
    }

    if (millis() - lastBeat > 200) showHeart = false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: MPU6050 — accelerometer + adaptive step counter
// ─────────────────────────────────────────────────────────────────────────────
void taskMPU6050(void *pvParameters) {
  sensors_event_t a, g, temp;

  while (1) {
    mpu.getEvent(&a, &g, &temp);

    float raw = sqrtf(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);

    filteredMagnitude = STEP_ALPHA * raw + (1.0f - STEP_ALPHA) * filteredMagnitude;

    // Publish filtered magnitude for LSTM input buffer
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      accel_magnitude = filteredMagnitude;
      xSemaphoreGive(xMutex);
    }

    unsigned long now = millis();

    if (filteredMagnitude > dynamicThreshold &&
        !stepDetected &&
        (now - lastStepTime > STEP_DELAY_MS)) {
      stepDetected = true;
      lastStepTime = now;
      if (xSemaphoreTake(stepMutex, portMAX_DELAY)) {
        stepCount++;
        xSemaphoreGive(stepMutex);
      }
      dynamicThreshold += STEP_THRESHOLD_INC;
    } else {
      dynamicThreshold -= STEP_THRESHOLD_DEC;
      if (dynamicThreshold < STEP_THRESHOLD_MIN)
        dynamicThreshold = STEP_THRESHOLD_MIN;
    }

    if (filteredMagnitude < dynamicThreshold - 1.5f)
      stepDetected = false;

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
