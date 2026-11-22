# esp32-devkit-custom — wiring reference

Plain ESP32-WROOM-32 DevKit, 4MB flash, hand-wired.

| ESP32 GPIO | Connects to                                   | Bruce field / flag                          |
|-----------:|------------------------------------------------|----------------------------------------------|
| 5          | TFT CS                                         | `TFT_CS`                                     |
| 21         | TFT DC                                         | `TFT_DC`                                     |
| 4          | TFT RST                                        | `TFT_RST`                                    |
| 22         | TFT backlight                                  | `TFT_BL`                                     |
| 18         | TFT SCK, microSD CLK, PN532 SCK                | `TFT_SCLK` / `PN532_bus.sck` / `SDCARD_bus.sck` |
| 23         | TFT MOSI, microSD MOSI, PN532 MOSI             | `TFT_MOSI` / `PN532_bus.mosi` / `SDCARD_bus.mosi` |
| 39         | TFT MISO (unused by panel), microSD MISO, PN532 MISO | `TFT_MISO` / `PN532_bus.miso` / `SDCARD_bus.miso` |
| 13         | microSD CS                                     | `SDCARD_bus.cs`                              |
| 32         | PN532 SS (set the board's mode switch/jumper to **SPI**, not I2C or HSU/UART) | `PN532_bus.cs` |
| 14         | Spare (was RC522 RST; not used by the PN532 driver — see note below) | — |
| 33         | PCF8574T SDA, RDA5807M SDA, DS3231 SDA         | `sys_i2c.sda`                                |
| 15         | PCF8574T SCL, RDA5807M SCL, DS3231 SCL         | `sys_i2c.scl`                                |
| 16         | IR transmitter (2N2222 base, 1kΩ)              | `irTx`                                       |
| 35         | TSOP1838 OUT                                   | `irRx`                                       |
| 17         | WS2812B DIN (330Ω)                             | `RGB_LED`                                    |
| 19         | Passive buzzer (100Ω)                          | `buzzer`                                     |
| 34         | Battery divider center point                   | `ANALOG_BAT_PIN`                             |
| 36         | FM audio mixer/bias node (ADC1_CH0) -- Bluetooth A2DP audio capture, see below | `FM_AUDIO_ADC_PIN` |
| 27, 26     | Unused (no microphone on this build)           | —                                             |

## PCF8574T (6 buttons: Up, Down, Left, Right, Select, Menu)

On the I2C bus above, address **0x20** (assumes A0/A1/A2 tied to GND — if not,
change `PCF8574_ADDR` in `interface.cpp`). Each button is wired from a PCF8574T
pin to GND; the pin reads LOW when pressed. P6/P7 are spare (unused).

Assumed mapping — **change `BTN_BIT_*` in `interface.cpp` if your buttons are
wired to different PCF8574T pins**:

| PCF8574T pin | Physical button | Firmware event |
|---|---|---|
| P0 | Left  | PrevPress (reused — this is what list navigation already listens for) |
| P1 | Right | NextPress (same reasoning) |
| P2 | Up    | UpPress |
| P3 | Down  | DownPress |
| P4 | Select| SelPress |
| P5 | Menu  | single press → takes you straight back to the home menu from any screen. Internally this fires a short burst of synthetic EscPress events (8, 150ms apart) rather than one — Bruce's menu stack has no direct "jump to home" call to hook into, so this walks back out through however many levels you're nested in automatically, no holding required (see the comment block above `InputHandler()` in `interface.cpp`) |

Buttons are polled by hand in `InputHandler()` because Bruce's shared button
HAL (`hal_buttons_poll_6`) only reads raw ESP32 GPIOs, not an I2C expander.

## DS3231 RTC

Shares the same I2C bus. Keeps real time through power-off (coin-cell backed),
so the clock/alarm/timer feature doesn't need WiFi to resync every boot —
only once, the first time you set it (via NTP or manually in the Time
settings), after which the DS3231 keeps ticking on its own.

Not a stock Bruce feature — Bruce ships drivers for two other RTC chips
(BM8563, used on M5Stack devices; PCF85063A) but not the DS3231, so this adds
a small custom driver (`lib/RTC/ds3231_RTC.{h,cpp}`) implementing just the
handful of calls Bruce's shared clock code (`init_clock()` in `main.cpp`,
`setClock()` in `settings.cpp`) actually needs: get/set time, get/set date,
and the `getTimeStruct()` used to feed the HUD's clock display. It plugs into
the same `HAS_RTC` code path as the other two chips via a new
`HAS_RTC_DS3231` board flag, so nothing about how the rest of the firmware
uses `_rtc` had to change.

**Not implemented** (because nothing in Bruce's shared code calls these on
this path, so there was no need to): DS3231's alarm interrupt pins (INT/SQW),
32kHz output, and temperature register. If you want the DS3231's hardware
alarm to wake the device from deep sleep later, that would need its own
small addition — flag it if you want that in a future pass.

## RDA5807M FM radio

Shares the same I2C bus. Not a stock Bruce feature — added as a small custom
module, see `src/modules/others/fm_radio.{h,cpp}` and the "FM Radio" entry
added to the Others menu.

### Wired headphone jack (3.5mm TRS, pure analog, no GPIO)

A 3.5mm female jack taps the RDA5807M's own LOUT/ROUT pins directly, for
always-available wired listening independent of Bluetooth or firmware state:

| Jack pin | Connects to |
|---|---|
| Tip    | RDA5807M LOUT, through a **1µF coupling capacitor** |
| Ring   | RDA5807M ROUT, through a **1µF coupling capacitor** |
| Sleeve | GND (common) |

The coupling caps block the chip's internal output DC bias so you don't get
a pop/hiss offset into the headphones. No firmware involvement at all —
audio is present on the jack any time the FM chip is powered and tuned,
whether or not a Bluetooth speaker is also connected.

### Bluetooth audio capture circuit (feeds GPIO36)

The same LOUT/ROUT pins are also bridged into a passive mono mixer/bias
network feeding the ESP32's ADC, so the firmware can re-encode and stream FM
audio out over Bluetooth A2DP (see "Bluetooth A2DP audio" below). Component
values and placement:

- **R_MIXL, R_MIXR: 10kΩ** — one from LOUT and one from ROUT, both meeting
  at a common "mix node". High enough impedance that it doesn't audibly load
  down the headphone jack wired to the same pins in parallel.
- **C_COUPLE: 1µF** (ceramic or film, non-polarized) — from the mix node to
  a second "bias node". Blocks DC, passes the audio AC signal through.
- **R_BIAS_HIGH: 100kΩ** — from 3.3V to the bias node.
- **R_BIAS_LOW: 100kΩ** — from the bias node to GND.
  (Together these two set the bias node's quiescent point at ~1.65V —
  centered in the ESP32 ADC's 0–3.3V range — without loading the signal
  much, since 100kΩ is high impedance.)
- **C_BIAS_FILTER: 100nF** (optional but recommended) — from the bias node
  to GND, for a little extra noise filtering on that bias point.
- **GPIO36** taps the bias node directly (ADC1_CH0, input-only pin, high
  impedance — fine to connect with no buffer).

## Bluetooth A2DP audio (classic Bluetooth, `HAS_BT_AUDIO`)

**The single riskiest part of this entire build — read this before relying
on it.** Not a stock Bruce feature; Bruce has no existing classic-Bluetooth/
A2DP code at all (confirmed by searching the whole repo), so this is new: a
from-scratch A2DP SOURCE written directly against the ESP-IDF classic-BT
headers that ship inside the arduino-esp32 core (`esp_bt.h`, `esp_bt_main.h`,
`esp_gap_bt_api.h`, `esp_a2dp_api.h`) — no new PlatformIO library fetch
needed, since those headers are already bundled with the framework.

What it does: lets Blackbird scan for, list, and connect to a Bluetooth
speaker/headset **from its own menu**, then stream audio to it — either
tuned FM audio (via the mixer/bias circuit above, captured on GPIO36) or
`.wav` files off the microSD card (the "Play Music" app). See
`src/modules/others/bt_audio_source.{h,cpp}` (the engine),
`bt_device_picker.{h,cpp}` (the shared scan/connect screen used by both FM
Radio and Play Music), and `sd_music_player.{h,cpp}`.

**Why this is risky:** a plain ESP32-WROOM-32 has one radio and ~520KB of
RAM, shared between WiFi (which this device uses heavily for its other
features), classic Bluetooth (BR/EDR, required for A2DP), and BLE (which
Bruce already uses elsewhere for scanning/spoofing features). Running all
three stacks in one firmware image is the single most memory- and
stability-sensitive thing in this whole project — more so than anything
else built so far. This has **not been tested on real hardware** (this
environment has no way to compile or flash it). If you hit boot crashes,
"Guru Meditation" errors, or brownout resets after enabling `HAS_BT_AUDIO`,
RAM exhaustion from running all three radios at once is the most likely
cause. If it proves unworkable, the fallback is a standalone Bluetooth audio
transmitter module wired to the RDA5807M's analog output instead of the
ESP32 doing Bluetooth itself — say the word and I'll wire that version in
instead.

**Known rough edges, said plainly:**
- No AVRCP — the connected speaker/headset's own volume or skip buttons do
  nothing; control only from Blackbird's own buttons.
- Pairing auto-accepts "Just Works" with no PIN-entry screen. Most modern
  speakers/headsets are fine with this; one that insists on a PIN prompt
  won't pair.
- Audio quality is "good enough to recognise a station or a song", not
  hi-fi: the resampler (16kHz from the ADC, or whatever the `.wav` file's
  own rate is, upsampled to the 44.1kHz stereo A2DP expects) is a simple
  nearest-neighbour duplication, not a proper filtered interpolator.
- Play Music only plays uncompressed PCM `.wav` files (8 or 16-bit, mono or
  stereo, any sample rate) placed in a `/music` folder on the SD card — no
  MP3/AAC/etc. decoding, since that needs a codec library this environment
  has no way to fetch.
- No pause/resume for file playback — just stop/skip/restart.

## Feature list updates (this pass)

Added and working (best-effort, uncompiled):
- **Play Music** — new Others-menu entry. Connects to a Bluetooth speaker
  (same picker screen as FM Radio) and streams `.wav` files from `/music` on
  the SD card. See `src/modules/others/sd_music_player.{h,cpp}`.
- **FM Radio → Bluetooth** — FM Radio now offers the same Bluetooth
  connect step before its tuning screen, and gained seek, signal bars, and a
  single-slot station preset (hold Up to save, hold Down to recall).
- **Alarm** — new Clock-submenu entry (`HAS_RTC` boards only). Runs as a
  background task independent of the menu system (see
  `src/modules/others/alarm.{h,cpp}`), so it rings even while you're using a
  different app, not just while sitting in the Clock screen. One real
  limitation: no on-screen pop-up when it fires (Bruce's menu system can't
  preempt whatever screen is currently blocking) — it rings the buzzer and
  is silenced by any button press from any screen, but the display keeps
  showing whatever you were already doing unless you happen to back out to
  the home menu yourself.
- **Stopwatch** — new Clock-submenu entry. Start/pause, lap recording
  (shows the 5 most recent), reset. See
  `src/modules/others/stopwatch.{h,cpp}`.
- **Clock-via-WiFi + HUD clock were already stock Bruce** — turns out no new
  code was needed for these two specifically: `setClock()`/
  `updateClockTimezone()` in `core/settings.cpp`/`core/utils.cpp` already
  sync time over WiFi (NTP) and write it through the generic `_rtc.SetTime()`
  interface, and the stock top-bar HUD (`core/display.cpp`) already prints
  the clock string. Both "just worked" once the DS3231 driver matched that
  same generic interface -- confirmed by reading the stock code rather than
  assumed.

Still backlog, not built yet — flagged here so nothing from the original
wishlist gets lost track of:
- **Notepad** (type notes via the BLE HID phone-keyboard bridge, save to SD
  card) — depends on the phone-keyboard BLE bridge, which hasn't been built
  yet (no phone-side app exists either). Comes together once that phone app
  is ready; bring it back up when you want to start on it.
- Pixel Pet companion, NeoPixel status beyond on/off, CPU underclocking,
  offline facts/math engine, Universal Remote per-device-type submenu
  polish, CC1101 Sub-GHz wiring — all previously deferred, still deferred
  unless you point back at one of them.

## Known limitations of this port

- **No wake-from-deep-sleep button.** `DEEPSLEEP_WAKEUP_PIN` isn't set because
  every button sits behind the PCF8574T, not a raw RTC-capable GPIO. The
  "Deep Sleep" menu entry will show as unavailable. A device reset or
  power-cycle is needed to wake it if you ever call `goToDeepSleep()`
  yourself from a script.
- **PN532 must be jumpered to SPI mode.** Most PN532 breakout boards have a
  3-way switch or solder-jumper pads selecting HSU (UART) / I2C / SPI — if
  left on its default (often HSU or I2C), the module simply won't respond on
  this bus. GPIO14 is left spare; wire it to the module's RSTPDN pin only if
  your specific board's RSTPDN doesn't already have its own pull-up and you
  see the reader fail to initialize.
- **No Sub-GHz (433/315MHz) support yet** — a CC1101 module is planned for a
  later pass but isn't wired in yet, so that whole menu will be present but
  non-functional for now.
- **TFT panel tab variant is a guess** (`ST7735_BLACKTAB`). If colors look
  wrong, try `ST7735_GREENTAB`, `ST7735_GREENTAB2`, `ST7735_GREENTAB3` or
  `ST7735_REDTAB` in `esp32-devkit-custom.ini` instead.
- **Battery divider ratio is assumed 2:1** (two equal resistors). If yours
  differ, recompute `ANALOG_BAT_MULTIPLIER = (R_top + R_bottom) / R_bottom`.
- **4MB flash is tight.** This config keeps FastLED and the full IR protocol
  list enabled (unlike Bruce's stock 4MB boards, which usually drop FastLED
  and trim IR protocols to fit). If `pio run` fails with a "section .flash1
  ... overflowed" error, the two things easiest to trim back are:
  - Add `-DLITE_VERSION=1` to `build_flags` (drops BadUSB/HID submenu and a
    few other features), or
  - Remove `-DHAS_RGB_LED=1` and the RGB_LED/LED_* flags (drops the WS2812B
    LED feature and its FastLED dependency) if you don't need it.
