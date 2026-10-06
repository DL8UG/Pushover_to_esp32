#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// Board: Waveshare ESP32-S3-ePaper-1.54 (V1 and V2 use the same pins).

// e-paper (SSD1681, 200x200) on SPI
#define EPD_SPI_NUM   SPI2_HOST
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8

// Switchable supply rails (low = on) and the battery latch
#define EPD_PWR_PIN   GPIO_NUM_6
#define AUDIO_PWR_PIN GPIO_NUM_42
#define VBAT_PWR_PIN  GPIO_NUM_17

// Buttons (low = pressed)
#define BOOT_BUTTON_PIN GPIO_NUM_0
#define PWR_BUTTON_PIN  GPIO_NUM_18

// Time zone for the clock and message times on the display (POSIX TZ
// string; default Central Europe / Berlin with daylight saving time)
#define LOCAL_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// Messages not acknowledged within this many hours expire: they disappear
// from the display and are acknowledged on Pushover, so the store doesn't
// fill up with stale alerts after a long time offline. Counted from the
// time the message was sent; needs the clock (SNTP). 0 = never expire.
#define MESSAGE_EXPIRY_HOURS 24

// NTP server for the clock
#define NTP_SERVER "pool.ntp.org"
