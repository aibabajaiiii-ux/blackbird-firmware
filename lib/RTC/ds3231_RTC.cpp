#include "ds3231_RTC.h"

// ASSUMPTION: the DS3231 shares the board's existing I2C bus (sys_i2c,
// already begun elsewhere in _setup_gpio() before this -- Wire.begin() with
// the same pins a second time is harmless on ESP32). RTC_SDA/RTC_SCL are
// only used if this code ever calls Wire.begin() itself; esp32-devkit-custom
// passes -DRTC_SDA=33 -DRTC_SCL=15 to match its sys_i2c pins in case a
// future board reuses this driver with Wire.begin() actually needed here.
#ifndef RTC_SDA
#define RTC_SDA 33
#endif
#ifndef RTC_SCL
#define RTC_SCL 15
#endif

void ds3231_RTC::begin(void) {
    // Bus is already initialized (shared sys_i2c) by the time this runs;
    // nothing to do. Kept as a no-op (rather than calling wr->begin() again)
    // to avoid any chance of re-configuring pins on a bus other drivers are
    // already using.
}

void ds3231_RTC::GetTime(RTC_TimeTypeDef *RTC_TimeStruct) {
    if (!RTC_TimeStruct) return;

    wr->beginTransmission(DS3231_ADDR);
    wr->write((uint8_t)0x00); // seconds register
    wr->endTransmission(false);
    wr->requestFrom((uint8_t)DS3231_ADDR, (uint8_t)3);

    uint8_t sec = wr->available() ? wr->read() : 0;
    uint8_t min = wr->available() ? wr->read() : 0;
    uint8_t hr = wr->available() ? wr->read() : 0;

    RTC_TimeStruct->Seconds = bcd2bin(sec & 0x7F);
    RTC_TimeStruct->Minutes = bcd2bin(min & 0x7F);
    RTC_TimeStruct->Hours = bcd2bin(hr & 0x3F); // 24hr mode (bit6=0 -- set at write time)
}

void ds3231_RTC::SetTime(RTC_TimeTypeDef *RTC_TimeStruct) {
    if (!RTC_TimeStruct) return;

    wr->beginTransmission(DS3231_ADDR);
    wr->write((uint8_t)0x00); // seconds register
    wr->write(bin2bcd(RTC_TimeStruct->Seconds));
    wr->write(bin2bcd(RTC_TimeStruct->Minutes));
    wr->write(bin2bcd(RTC_TimeStruct->Hours) & 0x3F); // force 24hr mode (bit6=0)
    wr->endTransmission();
}

void ds3231_RTC::GetDate(RTC_DateTypeDef *RTC_DateStruct) {
    if (!RTC_DateStruct) return;

    wr->beginTransmission(DS3231_ADDR);
    wr->write((uint8_t)0x03); // day-of-week register
    wr->endTransmission(false);
    wr->requestFrom((uint8_t)DS3231_ADDR, (uint8_t)4);

    uint8_t wday = wr->available() ? wr->read() : 1;
    uint8_t date = wr->available() ? wr->read() : 1;
    uint8_t monthCentury = wr->available() ? wr->read() : 1;
    uint8_t year = wr->available() ? wr->read() : 0;

    RTC_DateStruct->WeekDay = bcd2bin(wday & 0x07);
    RTC_DateStruct->Date = bcd2bin(date & 0x3F);
    RTC_DateStruct->Month = bcd2bin(monthCentury & 0x1F);
    // Century bit (monthCentury bit7): 0 = 2000s, 1 = 2100s. Not expected to
    // matter for decades, but handled for correctness.
    RTC_DateStruct->Year = (monthCentury & 0x80) ? (2100 + bcd2bin(year)) : (2000 + bcd2bin(year));
}

void ds3231_RTC::SetDate(RTC_DateTypeDef *RTC_DateStruct) {
    if (!RTC_DateStruct) return;

    bool century = RTC_DateStruct->Year >= 2100;
    uint8_t yy = (uint8_t)(RTC_DateStruct->Year % 100);

    wr->beginTransmission(DS3231_ADDR);
    wr->write((uint8_t)0x03); // day-of-week register
    wr->write(bin2bcd(RTC_DateStruct->WeekDay == 0 ? 1 : RTC_DateStruct->WeekDay));
    wr->write(bin2bcd(RTC_DateStruct->Date));
    wr->write((uint8_t)(bin2bcd(RTC_DateStruct->Month) | (century ? 0x80 : 0x00)));
    wr->write(bin2bcd(yy));
    wr->endTransmission();
}

struct tm ds3231_RTC::getTimeStruct() {
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;
    GetTime(&t);
    GetDate(&d);

    struct tm timeinfo = {0};
    timeinfo.tm_sec = t.Seconds;
    timeinfo.tm_min = t.Minutes;
    timeinfo.tm_hour = t.Hours;
    timeinfo.tm_mday = d.Date;
    timeinfo.tm_mon = d.Month > 0 ? d.Month - 1 : 0;
    timeinfo.tm_year = d.Year - 1900;
    timeinfo.tm_wday = d.WeekDay;

    mktime(&timeinfo); // normalize + fill tm_yday
    return timeinfo;
}
