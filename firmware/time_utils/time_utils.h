#pragma once

// Connect to WiFi using credentials from config.h
void connectWiFi();

// Sync NTP client and update currentTime / currentDate strings
void showTime();

// Start the NTP client — call once in setup()
void initTime();
