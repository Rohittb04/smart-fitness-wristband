#include "time_utils.h"
#include "config.h"

#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <time.h>

// ─── NTP internals (file-local) ───────────────────────────────────────────────
static WiFiUDP   ntpUDP;
static NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800 /*IST offset*/, 60000);

static const char* daysOfTheWeek[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char* months[]        = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};

// ─────────────────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED
                   ? "\n[WiFi] Connected!"
                   : "\n[WiFi] Failed — continuing offline");
}

void initTime() {
  timeClient.begin();
}

void showTime() {
  timeClient.update();

  int    h      = timeClient.getHours();
  int    m      = timeClient.getMinutes();
  String period = (h >= 12) ? "PM" : "AM";
  if (h > 12)  h -= 12;
  if (h == 0)  h  = 12;
  snprintf(currentTime, sizeof(currentTime), "%02d%02d%s", h, m, period.c_str());

  time_t    epoch    = timeClient.getEpochTime();
  struct tm *timeinfo = localtime(&epoch);
  snprintf(currentDate, sizeof(currentDate), "%s %02d%s%d",
           daysOfTheWeek[timeinfo->tm_wday],
           timeinfo->tm_mday,
           months[timeinfo->tm_mon],
           timeinfo->tm_year + 1900);
}
