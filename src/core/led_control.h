#ifndef __LED_CONTROL_H__
#define __LED_CONTROL_H__
#include <globals.h>

#ifdef HAS_RGB_LED
#include <Arduino.h>
// globals.h (included above) drags in TFT_eSPI's ST7735_Defines.h, which
// #defines plain RED/BLACK as 16-bit color constants (0xF800/0x0000).
// FastLED 3.10.x's fl/rbtree.h declares `enum Color { RED, BLACK };` as
// unqualified identifiers for its red-black tree node color, so without
// undefining these first, the preprocessor mangles that enum into garbage
// and the whole FastLED map/set template cascade fails to parse. Nothing
// in this header (or led_control.cpp) uses the TFT RED/BLACK color
// macros, so it's safe to drop them here, right before FastLED.h.
#undef RED
#undef BLACK
#include <FastLED.h>

#define LED_EFFECT_SOLID 0
#define LED_COLOR_BREATHE 1
#define LED_EFFECT_COLOR_CYCLE 2
#define LED_EFFECT_COLOR_WHEEL 3
#define LED_EFFECT_CHASE 4
#define LED_EFFECT_CHASE_TAIL 5
#define LED_EFFECT_RAINBOW_CHASE 6
#define LED_EFFECT_RAINBOW_BREATHE 7
#define LED_EFFECT_DISCO 8
#define LED_EFFECT_FIRE 9

CRGB hsvToRgb(uint16_t h, uint8_t s, uint8_t v);
uint32_t alterOneColorChannel(uint32_t color, uint16_t newR, uint16_t newG, uint16_t newB);

void beginLed();
void blinkLed(int blinkTime = 50);

void setLedColor(CRGB color);
void setLedEffect(int effect);
void setLedColorConfig();
void setCustomColorMenu();
void setCustomColorSettingMenuR();
void setCustomColorSettingMenuG();
void setCustomColorSettingMenuB();
void setLedEffectConfig();
void setLedEffectSpeedConfig();
void setLedEffectDirectionConfig();
void ledSetup();
void ledEffects(bool enable);
void ledPreviewMode(bool enable);
void setLedBrightness(int value);
void setLedBrightnessConfig();

#else
inline void blinkLed(int blinkTime = 50) {};
#endif

#endif
