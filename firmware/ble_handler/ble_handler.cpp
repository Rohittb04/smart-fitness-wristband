#include "ble_handler.h"
#include "config.h"
#include "time_utils.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLE2902.h>

// ─── File-local BLE handles ───────────────────────────────────────────────────
static BLEServer*         pServer            = nullptr;
static BLECharacteristic* dataCharacteristic = nullptr;
static bool               oldDeviceConnected = false;

// ─── Connection callbacks ─────────────────────────────────────────────────────
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect   (BLEServer*) override { deviceConnected = true;  }
  void onDisconnect(BLEServer*) override { deviceConnected = false; }
};

// ─── Bonding / security callbacks ────────────────────────────────────────────
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest()               override { return 123456; }
  void     onPassKeyNotify(uint32_t)        override {}
  bool     onConfirmPIN(uint32_t)           override { return true;   }
  bool     onSecurityRequest()              override { return true;   }
  void     onAuthenticationComplete(esp_ble_auth_cmpl_t c) override {
    Serial.println(c.success ? "[BLE] ✅ Bonded" : "[BLE] ❌ Bond failed");
  }
};

// ─────────────────────────────────────────────────────────────────────────────
void initBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
  BLEDevice::setMTU(256);

  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Single characteristic — carries "HR:72,LSTM:73.5,SPO2:98,STEPS:1234"
  dataCharacteristic = pService->createCharacteristic(
      DATA_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  dataCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertisementData adData;
  adData.setName(DEVICE_NAME);
  adData.setCompleteServices(BLEUUID(SERVICE_UUID));

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setAdvertisementData(adData);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising started");
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: send all sensor values as one CSV string every second
// ─────────────────────────────────────────────────────────────────────────────
void taskBLEUpdate(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(5000));  // Let sensors stabilise

  while (1) {
    // ── Handle reconnection ────────────────────────────────────────────────
    if (!deviceConnected && oldDeviceConnected) {
      vTaskDelay(pdMS_TO_TICKS(500));
      pServer->startAdvertising();
      Serial.println("[BLE] Re-advertising...");
      oldDeviceConnected = false;
    }
    if (deviceConnected && !oldDeviceConnected) {
      Serial.println("[BLE] Client connected");
      oldDeviceConnected = true;
    }

    if (deviceConnected) {
      // ── Safely snapshot all shared values ─────────────────────────────
      int   hr    = 0, spo2_ = 0;
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

      // ── Build + send CSV payload ──────────────────────────────────────
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
// Task: NTP time sync every second
// ─────────────────────────────────────────────────────────────────────────────
void taskTimeUpdate(void *pvParameters) {
  while (1) {
    showTime();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
