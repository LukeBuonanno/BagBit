# BagBit

A bag charm with customizable animations, info, and games. A compact ESP32-S3-powered device featuring a 0.96" 80x160 RGB TFT display, onboard battery, and a single-button interface. It supports animations, a clock, weather display, and simple apps.

# Hardware Components:
ESP32-S3 Dev Module
0.96" IPS TFT Display (80x160, SPI, ST7735 driver)
103450 3.7V 2000mAh LiPo Battery
TP4056 5V 1A Lithium Battery Charger Module
Buck-Boost Converter (3.7V → 5V or 3.3V depending on your setup)
1x Mechanical Keyboard Switch (main input)
1x Small Power Switch
Custom 3D Printed Chassis

# Wiring Guide

# TFT Display → ESP32-S3 (SPI)
TFT Pin	ESP32 Pin
   VCC	 3.3V
   GND	  GND
   SCL	  GPIO 36
   SDA	  GPIO 35
   RES	  GPIO 12
   DC	  GPIO 10
   CS	  GPIO 11
 
Button (Mechanical Switch)
Switch Pin	ESP32
 One side	   GPIO 45
  Other side	GND


# Battery + Power System
Power Flow:
LiPo Battery (+)
   ↓
Power Switch
   ↓
TP4056 Charger Module (B+ / B-)
   ↓
OUT+ / OUT-
   ↓
Buck-Boost Converter
   ↓
ESP32 Power Input (5V or 3.3V)

# Notes:
The small switch is wired inline with the positive battery lead
TP4056 handles charging via USB
Buck-boost ensures stable voltage to ESP32

# Physical Assembly
ESP32 is nserted into the bottom chassis
with Pins facing upward
Internal Layout on top of ESP32 (Front → Back)
Display
Mounted inside the front of the bottom chassis
Buck-Boost Converter
Positioned behind the display
Battery (LiPo)
Placed behind the converter
TP4056 Charger Module
Located at the rear
USB port aligned with chassis opening
Mechanical switch:
Insert through front chassis hole
Feed wires through then solder connections
before pressing into place

# After wiring:

Secure all components inside bottom chassis
Attach display to front interior
Press top chassis onto bottom to close enclosure

# Software Setup

# 1. Install Arduino IDE Libraries

Required libraries:

Adafruit GFX
Adafruit ST7735
ArduinoJson
Preferences (built-in)
WiFi (built-in)

# 2. Board Configuration
Board: ESP32S3 Dev Module
Flash Size: 8MB (or your board spec)
Partition Scheme: Default / Large App

# 3. Upload Code
Connect ESP32 via USB
Select correct COM port
Upload the provided .ino file

# 4. WiFi and Weather setup

Edit in code:

const char *WIFI_SSID = "your_wifi";
const char *WIFI_PASS = "your_password";

#define WX_LAT   "your_lat"
#define WX_LON   "your_long"

// Change this to -4*3600 for EDT or -5*3600 for EST as appropriate.
#define GMT_OFFSET   (-4 * 3600)   // UTC-4  (EDT)
