#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model.h"
#include <Wire.h>
#include <WiFi.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLE2902.h>

// ─── FIX 1: Removed stray '' after NULL ───────────────────────────────────────
BLEServer *pServer = NULL;

// WiFi credentials
const char* ssid     = "Rohit";
const char* password = "12345678";

bool deviceConnected    = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
// Single characteristic that carries all four values as a CSV string:
// "HR:72,LSTM:73.5,SPO2:98,STEPS:1234"
#define DATA_CHAR_UUID      "00002a00-0000-1000-8000-00805f9b34fb"

BLECharacteristic *dataCharacteristic;   // replaces all four old characteristics

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer)    { deviceConnected = true;  }
  void onDisconnect(BLEServer *pServer) { deviceConnected = false; }
};

// NTP time sync
WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

const char* daysOfTheWeek[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const char* months[]        = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
char currentTime[11] = "0000PM";
char currentDate[20] = "Sun 01Jan 2024";

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  32
#define OLED_RESET     -1
#define BUTTON_PIN     D1
#define ECG_PIN        34

volatile bool showHeart = false;

MAX30105        particleSensor;
Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── TensorFlow Lite ──────────────────────────────────────────────────────────
constexpr int kTensorArenaSize = 12 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input  = nullptr;
TfLiteTensor* output = nullptr;

// Ring buffer: 10 timesteps × 4 features [hr, spo2, accel_magnitude, steps]
#define LSTM_TIMESTEPS 10
#define LSTM_FEATURES   4
float input_buffer[LSTM_TIMESTEPS][LSTM_FEATURES] = {0};
int   input_buffer_idx = 0;   // tracks which row to overwrite next (circular)

float current_heart_rate = 0.0;  // LSTM predicted value

// ─── FIX 4: Declare AND later initialise xMutex ───────────────────────────────
SemaphoreHandle_t xMutex;

// Shared sensor variables
int   hr_max = 0;
float accel_x = 0.0, accel_y = 0.0, accel_z = 0.0;
float accel_magnitude = 0.0;  // exposed so InferenceTask can read it
int   spO2     = 99;
int   ecg_value = 0;

// Step counter
int   stepCount    = 0;
float prevMagnitude = 0.0;
bool  stepDetected  = false;
unsigned long lastStepTime = 0;
const unsigned long stepDelay = 300;

// Low-pass filter
const float alpha = 0.1f;
float filteredMagnitude = 0.0f;

// Dynamic threshold
float dynamicThreshold       = 9.6f;
const float thresholdIncrement = 0.05f;
const float thresholdDecrement = 0.01f;

volatile int screenMode = 0;

// ─── Bitmaps ──────────────────────────────────────────────────────────────────
const unsigned char heartSmall[] PROGMEM = {
  0b00000000, 0b01100110, 0b11111111, 0b11111111,
  0b11111111, 0b01111110, 0b00111100, 0b00011000
};
const unsigned char wifi_logo[] PROGMEM = {
  0b00011000, 0b00111100, 0b01111110, 0b11011011,
  0b00011000, 0b00100100, 0b01000010, 0b00000000
};
const unsigned char bt_logo[] PROGMEM = {
  0b00011000, 0b00100100, 0b00111000, 0b01101100,
  0b00111000, 0b00100100, 0b00011000, 0b00000000
};

// Mutexes
SemaphoreHandle_t max30102Mutex;
SemaphoreHandle_t stepMutex;

// ─── Forward declarations ─────────────────────────────────────────────────────
const char* getRiskLevel(int hr, int spo2);
void        showTime();

// ─── ISR ──────────────────────────────────────────────────────────────────────
void IRAM_ATTR buttonPressed() {
  screenMode = (screenMode + 1) % 3;
}

// ─── SpO2 helper ──────────────────────────────────────────────────────────────
int calculateSpO2() {
  long irValue  = particleSensor.getIR();
  long redValue = particleSensor.getRed();
  if (redValue < 28000 || irValue < 50000) return 0;
  float ratio       = (float)redValue / irValue;
  int estimatedSpO2 = 108 - (int)(25 * ratio);
  estimatedSpO2 = constrain(estimatedSpO2, 85, 100);
  return estimatedSpO2;
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: MAX30102 (heart rate + SpO2)
// ─────────────────────────────────────────────────────────────────────────────
void taskMAX30102(void *pvParameters) {
  static unsigned long lastBeat = 0;
  static int beatAvg = 0;
  long irValue;

  while (1) {
    irValue = particleSensor.getIR();

    if (irValue < 50000) {
      if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
        beatAvg = 0;
        hr_max  = 0;
        spO2    = calculateSpO2();
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

          // ─── FIX 5: Feed latest sensor values into LSTM input buffer ──────
          if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
            int steps_snapshot = 0;
            // Briefly grab steps (best-effort, non-blocking)
            if (xSemaphoreTake(stepMutex, 0)) {
              steps_snapshot = stepCount;
              xSemaphoreGive(stepMutex);
            }

            // Write into the circular buffer row
            input_buffer[input_buffer_idx][0] = (float)beatAvg;        // HR
            input_buffer[input_buffer_idx][1] = (float)spO2;           // SpO2
            input_buffer[input_buffer_idx][2] = accel_magnitude;        // accel mag
            input_buffer[input_buffer_idx][3] = (float)steps_snapshot;  // steps

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
// Task: MPU6050 (accelerometer / step counter)
// ─────────────────────────────────────────────────────────────────────────────
void taskMPU6050(void *pvParameters) {
  sensors_event_t a, g, temp;

  while (1) {
    mpu.getEvent(&a, &g, &temp);

    float magnitude = sqrtf(a.acceleration.x * a.acceleration.x +
                            a.acceleration.y * a.acceleration.y +
                            a.acceleration.z * a.acceleration.z);

    filteredMagnitude = alpha * magnitude + (1.0f - alpha) * filteredMagnitude;

    // ─── FIX 6: Update shared accel_magnitude so LSTM buffer can use it ──────
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      accel_magnitude = filteredMagnitude;
      xSemaphoreGive(xMutex);
    }

    unsigned long now = millis();

    if (filteredMagnitude > dynamicThreshold && !stepDetected &&
        (now - lastStepTime > stepDelay)) {
      stepDetected = true;
      lastStepTime = now;
      if (xSemaphoreTake(stepMutex, portMAX_DELAY)) {
        stepCount++;
        xSemaphoreGive(stepMutex);
      }
      dynamicThreshold += thresholdIncrement;
    } else {
      dynamicThreshold -= thresholdDecrement;
      if (dynamicThreshold < 10.0f) dynamicThreshold = 10.0f;
    }

    if (filteredMagnitude < dynamicThreshold - 1.5f) stepDetected = false;

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: OLED Display
// ─────────────────────────────────────────────────────────────────────────────
void taskDisplay(void *pvParameters) {
  while (1) {
    display.clearDisplay();
    display.setRotation(1);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);

    if (screenMode == 0) {
      display.setTextSize(2);
      display.setCursor(0, 0);
      display.printf("%s", currentTime);
      display.setTextSize(1);
      display.setCursor(30, 50);
      display.printf("%s", currentDate);

      if (showHeart)
        display.drawBitmap(10, 120, heartSmall, 8, 8, SSD1306_WHITE);
      if (WiFi.status() == WL_CONNECTED)
        display.drawBitmap(0, 107, wifi_logo, 8, 8, SSD1306_WHITE);
      if (deviceConnected)
        display.drawBitmap(20, 107, bt_logo, 8, 8, SSD1306_WHITE);

      if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
        const char* risk = getRiskLevel(hr_max, spO2);
        xSemaphoreGive(max30102Mutex);
        display.setCursor(0, 84);
        display.setTextSize(1);
        display.print("Risk:");
        display.setCursor(11, 94);
        display.print(risk);
      }

    } else if (screenMode == 1) {
      if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
        display.println("HR");
        display.setTextSize(2);
        display.printf("%d", hr_max);
        display.setCursor(30, 60);
        display.setTextSize(1);
        display.println("SpO2");
        display.setTextSize(2);
        display.printf("%d%%", spO2);

        // ─── Show LSTM prediction on screen too ───────────────────────────
        float lstm_val = 0.0f;
        if (xSemaphoreTake(xMutex, 0)) {
          lstm_val = current_heart_rate;
          xSemaphoreGive(xMutex);
        }
        display.setCursor(0, 100);
        display.setTextSize(1);
        display.printf("AI HR:%.0f", lstm_val);

        xSemaphoreGive(max30102Mutex);
      }

    } else if (screenMode == 2) {
      if (xSemaphoreTake(stepMutex, portMAX_DELAY)) {
        display.setCursor(0, 50);
        display.setTextSize(1);
        display.println("Steps");
        display.setTextSize(2);
        display.printf("%d", stepCount);
        xSemaphoreGive(stepMutex);
      }
    }

    display.display();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: TFLite LSTM Inference
// ─────────────────────────────────────────────────────────────────────────────
void InferenceTask(void *pvParameters) {
  while (1) {
    // ─── FIX 7: Copy circular buffer into model input in correct time order ──
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      for (int t = 0; t < LSTM_TIMESTEPS; t++) {
        // Map oldest→newest into model's sequential input
        int src = (input_buffer_idx + t) % LSTM_TIMESTEPS;
        for (int f = 0; f < LSTM_FEATURES; f++) {
          input->data.f[t * LSTM_FEATURES + f] = input_buffer[src][f];
        }
      }
      xSemaphoreGive(xMutex);
    }

    if (interpreter->Invoke() == kTfLiteOk) {
      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        current_heart_rate = output->data.f[0];
        xSemaphoreGive(xMutex);
      }
      Serial.printf("[LSTM] Predicted HR: %.2f\n", current_heart_rate);
    } else {
      Serial.println("[LSTM] Model invoke failed!");
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Run inference once per second
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: BLE notify (sends all metrics including LSTM output)
// ─────────────────────────────────────────────────────────────────────────────
void taskBLEUpdate(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(5000));

  while (1) {
    // Handle reconnection
    if (!deviceConnected && oldDeviceConnected) {
      vTaskDelay(pdMS_TO_TICKS(500));
      pServer->startAdvertising();
      Serial.println("[BLE] Re-advertising...");
      oldDeviceConnected = false;
    }
    if (deviceConnected && !oldDeviceConnected) {
      Serial.println("[BLE] Device connected");
      oldDeviceConnected = true;
    }

    if (deviceConnected) {
      // ── Read all sensor values under their respective mutexes ──────────────
      int   hr    = 0;
      int   spo2_ = 0;
      if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
        hr    = hr_max;
        spo2_ = spO2;
        xSemaphoreGive(max30102Mutex);
      }

      int steps = 0;
      if (xSemaphoreTake(stepMutex, portMAX_DELAY)) {
        steps = stepCount;
        xSemaphoreGive(stepMutex);
      }

      float lstm_hr = 0.0f;
      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        lstm_hr = current_heart_rate;
        xSemaphoreGive(xMutex);
      }

      // Format: "HR:72,LSTM:73.5,SPO2:98,STEPS:1234"
      // Android parses by splitting on ',' then ':' 
      char payload[64];
      snprintf(payload, sizeof(payload),
               "HR:%d,LSTM:%.1f,SPO2:%d,STEPS:%d",
               hr, lstm_hr, spo2_, steps);

      Serial.printf("[BLE] → %s\n", payload);

      dataCharacteristic->setValue((uint8_t*)payload, strlen(payload));
      dataCharacteristic->notify();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: NTP time update
// ─────────────────────────────────────────────────────────────────────────────
void taskTimeUpdate(void *pvParameters) {
  while (1) {
    showTime();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
void showLogo() {
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 5);
  display.print(" B-Fit");
  display.display();
  delay(2000);
}

void showTime() {
  timeClient.update();

  int    hours   = timeClient.getHours();
  int    minutes = timeClient.getMinutes();
  String period  = (hours >= 12) ? "PM" : "AM";
  if (hours > 12)  hours -= 12;
  if (hours == 0)  hours  = 12;
  snprintf(currentTime, sizeof(currentTime), "%02d%02d%s", hours, minutes, period.c_str());

  time_t    epochTime = timeClient.getEpochTime();
  struct tm *timeinfo = localtime(&epochTime);
  snprintf(currentDate, sizeof(currentDate), "%s %02d%s%d",
           daysOfTheWeek[timeinfo->tm_wday],
           timeinfo->tm_mday,
           months[timeinfo->tm_mon],
           timeinfo->tm_year + 1900);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "\nConnected!" : "\nFailed!");
}

class MySecurity : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest()                        { return 123456; }
  void     onPassKeyNotify(uint32_t pass_key)        {}
  bool     onConfirmPIN(uint32_t pass_key)           { return true;   }
  bool     onSecurityRequest()                       { return true;   }
  void     onAuthenticationComplete(esp_ble_auth_cmpl_t c) {
    Serial.println(c.success ? "✅ Bonded" : "❌ Bond failed");
  }
};

const char* getRiskLevel(int hr, int spo2) {
  if (hr < 40 || hr > 120 || spo2 < 90)                                 return "H";
  if ((hr >= 40 && hr < 55) || (hr > 100 && hr <= 140) ||
      (spo2 >= 90 && spo2 < 95))                                         return "M";
  return "L";
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  showLogo();

  connectWiFi();
  timeClient.begin();
  showTime();

  // ── BLE init ──────────────────────────────────────────────────────────────
  BLEDevice::init("B-Fit Tracker");
  BLEDevice::setSecurityCallbacks(new MySecurity());
  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setMTU(256);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Single combined characteristic — sends all values in one CSV notify
  dataCharacteristic = pService->createCharacteristic(
      DATA_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  dataCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  BLEAdvertisementData advertisementData;
  advertisementData.setName("B-Fit Tracker");
  advertisementData.setCompleteServices(BLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(advertisementData);
  BLEDevice::startAdvertising();
  Serial.println("📡 Advertising started");

  // ── Sensors ───────────────────────────────────────────────────────────────
  particleSensor.begin(Wire, I2C_SPEED_FAST);
  particleSensor.setup();
  mpu.begin();

  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  attachInterrupt(BUTTON_PIN, buttonPressed, RISING);

  // ── TFLite init ───────────────────────────────────────────────────────────
  tflite::InitializeTarget();
  const tflite::Model* model = tflite::GetModel(heart_rate_lstm_tflite);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("TFLite version mismatch"); while (1);
  }

  static tflite::MicroMutableOpResolver<4> resolver;
  resolver.AddFullyConnected();
  resolver.AddReshape();
  resolver.AddQuantize();
  resolver.AddDequantize();

  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("Tensor allocation failed"); while (1);
  }

  input  = interpreter->input(0);
  output = interpreter->output(0);

  // ─── FIX 10: Initialise ALL mutexes (xMutex was missing!) ────────────────
  xMutex       = xSemaphoreCreateMutex();
  max30102Mutex = xSemaphoreCreateMutex();
  stepMutex     = xSemaphoreCreateMutex();

  // ── Tasks ─────────────────────────────────────────────────────────────────
  xTaskCreatePinnedToCore(taskMAX30102,   "MAX30102",     4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskMPU6050,    "MPU6050",      4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskDisplay,    "Display",      4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(InferenceTask,  "InferenceTask",4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(taskBLEUpdate,  "BLEUpdate",    8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(taskTimeUpdate, "TimeUpdate",   4096, NULL, 1, NULL, 1);
}

void loop() { showTime(); }
