#pragma once

// Minimal DS3231 driver matching cplus_RTC's public interface closely enough
// for Bruce's generic HAS_RTC code path (src/main.cpp's init_clock() and
// src/core/settings.cpp's setClock()/time-set flow only ever call
// begin()/setWire()/GetTime()/SetTime()/GetDate()/SetDate()/getTimeStruct()
// on _rtc when neither HAS_RTC_BM8563 nor HAS_RTC_PCF85063A is defined, so
// that's all this class needs to implement -- not a full DS3231 feature
// (no alarm/SQW/temperature register support, since nothing in Bruce's
// shared code calls those on this path).
//
// Not a stock Bruce file -- added for the esp32-devkit-custom board's
// DS3231 RTC module.

#include <Wire.h>
#include <time.h>

// Reuses the same struct names as cplus_RTC.h -- only one of the two headers
// is ever included for a given build (see include/globals.h), so there's no
// redefinition conflict.
typedef struct {
    uint8_t Hours;
    uint8_t Minutes;
    uint8_t Seconds;
} RTC_TimeTypeDef;

typedef struct {
    uint8_t WeekDay;
    uint8_t Month;
    uint8_t Date;
    uint16_t Year;
} RTC_DateTypeDef;

class ds3231_RTC {
public:
    void begin(void);
    void setWire(TwoWire *obj) { wr = obj; }

    void GetTime(RTC_TimeTypeDef *RTC_TimeStruct);
    void SetTime(RTC_TimeTypeDef *RTC_TimeStruct);
    void GetDate(RTC_DateTypeDef *RTC_DateStruct);
    void SetDate(RTC_DateTypeDef *RTC_DateStruct);

    struct tm getTimeStruct();

private:
    TwoWire *wr = &Wire;
    static const uint8_t DS3231_ADDR = 0x68;

    static uint8_t bcd2bin(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
    static uint8_t bin2bcd(uint8_t v) { return (uint8_t)((((v / 10) << 4) | (v % 10))); }
};
