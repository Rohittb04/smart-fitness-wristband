#include "display.h"
#include "config.h"
#include <WiFi.h>

// ─── Display object ───────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── 8×8 bitmaps (file-local) ─────────────────────────────────────────────────
static const unsigned char heartSmall[] PROGMEM = {
  0b00000000, 0b01100110, 0b11111111, 0b11111111,
  0b11111111, 0b01111110, 0b00111100, 0b00011000
};
static const unsigned char wifi_logo[] PROGMEM = {
  0b00011000, 0b00111100, 0b01111110, 0b11011011,
  0b00011000, 0b00100100, 0b01000010, 0b00000000
};
static const unsigned char bt_logo[] PROGMEM = {
  0b00011000, 0b00100100, 0b00111000, 0b01101100,
  0b00111000, 0b00100100, 0b00011000, 0b00000000
};

// ─────────────────────────────────────────────────────────────────────────────
void initDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] Init failed!");
  }
  display.clearDisplay();
}

void showLogo() {
  display.clearDisplay();
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 5);
  display.print(" B-Fit");
  display.display();
  delay(2000);
}

// ─── Screen mode helpers (file-local) ─────────────────────────────────────────
static void drawMode0() {
  // Clock + date + status icons + risk level
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
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
}

static void drawMode1() {
  // HR + SpO2 + LSTM prediction
  if (xSemaphoreTake(max30102Mutex, portMAX_DELAY)) {
    int   hr_snap   = hr_max;
    int   spo2_snap = spO2;
    xSemaphoreGive(max30102Mutex);

    float lstm_val = 0.0f;
    if (xSemaphoreTake(xMutex, 0)) {   // non-blocking — skip if busy
      lstm_val = current_heart_rate;
      xSemaphoreGive(xMutex);
    }

    display.setTextSize(1);
    display.println("HR");
    display.setTextSize(2);
    display.printf("%d", hr_snap);

    display.setCursor(30, 60);
    display.setTextSize(1);
    display.println("SpO2");
    display.setTextSize(2);
    display.printf("%d%%", spo2_snap);

    display.setCursor(0, 100);
    display.setTextSize(1);
    display.printf("AI HR:%.0f", lstm_val);
  }
}

static void drawMode2() {
  // Step counter
  if (xSemaphoreTake(stepMutex, portMAX_DELAY)) {
    int s = stepCount;
    xSemaphoreGive(stepMutex);

    display.setCursor(0, 50);
    display.setTextSize(1);
    display.println("Steps");
    display.setTextSize(2);
    display.printf("%d", s);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: OLED display — refreshes at 2 Hz
// ─────────────────────────────────────────────────────────────────────────────
void taskDisplay(void *pvParameters) {
  while (1) {
    display.clearDisplay();
    display.setRotation(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);

    switch (screenMode) {
      case 0: drawMode0(); break;
      case 1: drawMode1(); break;
      case 2: drawMode2(); break;
      default: break;
    }

    display.display();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
