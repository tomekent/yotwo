# Hardware: Yoto Player Gen 2

What is on the board, how it is wired, and what to watch out for. The pin map
and chip list were learned from the original firmware's own hardware
description and then confirmed one by one as yotwo's drivers brought each part
up on real players. Anything not yet confirmed on a player says so.

This is the full-size **Gen 2** player with the 16 × 16 LED-matrix display, two
knobs and the pogo-pin charging dock. It is not the Mini, and not the Gen 3.

## SoC, flash and memory

| Item | Value |
|---|---|
| SoC | ESP32-D0WD, dual core, 240 MHz, 40 MHz crystal. Seen as silicon revision **v1.0** on one board and **v3.0** on another: both run the same yotwo build |
| Flash | 8 MB, 3.3 V, DIO. Seen from two makers: Boya (`0x68`, device `0x4017`) and GigaDevice (`0xc8`) |
| PSRAM | 8 MB fitted; the ESP32 maps 4 MB of it |
| Security | Secure boot and flash encryption **off** on both boards checked, so the flash can be read, backed up and rewritten over serial |
| Console / flashing | UART0, TX GPIO 1, RX GPIO 3, **115200 baud** |

Revision v1.0 silicon (the original ESP32, pre-ECO3) needs the PSRAM cache
workaround, which ESP-IDF applies by default below revision v3.0. It also has a 20 KB
corner of instruction RAM that the heap hands out but that only allows aligned
32-bit access. yotwo reserves it at start-up so nothing copies bytes into it
(see `reserve_iram_heap` in `main.c`).

## ESP32 GPIO map

| GPIO | Function | Notes |
|---|---|---|
| 0 | I2S MCLK | Strapping pin: **low at reset = bootloader mode** |
| 1 / 3 | UART0 TX / RX | Console and flashing |
| 2 | Shared SPI SCLK | Strapping pin |
| 4 | Encoder 2, pin A | |
| 5 | Encoder 1, pin A | Strapping pin |
| 12 | Nightlight (LED ring) red | Strapping pin (flash voltage); see cautions |
| 13 | Encoder 1, pin B | |
| 14 | SD SPI SCLK | |
| 15 | SD SPI CS | Strapping pin |
| 18 | Shared SPI MOSI | |
| 19 | Nightlight (LED ring) blue | |
| 21 | Shared SPI MISO | |
| 22 | SD SPI MOSI | |
| 23 | Nightlight (LED ring) green | |
| 25 | I2S BCLK | |
| 26 | I2C SDA | |
| 27 | I2C SCL | |
| 32 | I2S LRCLK | |
| 33 | I2S data out | |
| 34 | SD SPI MISO | Input-only pin |
| 35 | Encoder 2, pin B | Input-only pin, no internal pull |
| 36 | Light sensor, ADC1 channel 0 | Input-only pin. Not used by yotwo yet |
| 39 | Battery voltage, ADC1 channel 3 | Input-only pin. Battery mV = pin mV × 82 / 39 |
| 16, 17 | PSRAM | |

Encoder 1 is the **left** knob. Each knob's push button is on the IO expander
(below), not on a GPIO.

## IO expander: PI4IOE5V6416 (16-bit, I2C `0x20`)

Pin directions: port 0 `0x30`, port 1 `0xAF` (1 = input). Pull-ups enabled on
port 0 `0x30` and port 1 `0xA8`. Outputs start high.

| Pin | Dir | Pull-up | Function |
|---|---|---|---|
| 0.0 | out | | Display chip select 3 (rows 12–15), low = selected |
| 0.1 | out | | Display chip select 2 (rows 8–11) |
| 0.2 | out | | Display chip select 1 (rows 4–7) |
| 0.3 | out | | Display chip select 0 (rows 0–3) |
| 0.4 | in | yes | Encoder 1 (left) push button, low = pressed |
| 0.5 | in | yes | Encoder 2 (right) push button, low = pressed |
| 0.6 | out | | Speaker amplifier enable, high = on |
| 0.7 | out | | NFC `IRQ_IN` (wake pulse to the CR95HF) |
| 1.0 | in | | NFC `IRQ_OUT` (data ready from the CR95HF) |
| 1.1 | in | | Headphone detect, low = plugged in |
| 1.2 | in | | Accelerometer interrupt |
| 1.3 | in | yes | Power button, low = pressed |
| 1.4 | out | | NFC SPI chip select |
| 1.5 | in | yes | Charger: external power present, low = present |
| 1.6 | out | | **`vinhold` power latch, high = stay on** |
| 1.7 | in | yes | Charger: charging, low = charging |

- **The pull-ups matter.** Without them the knob buttons float and produce
  random presses. They are enabled through registers `0x46`/`0x47` (enable)
  and `0x48`/`0x49` (1 = pull-up).
- The expander's interrupt line is **not** wired to the ESP32, so every input
  above is polled over I2C (yotwo polls every 20 ms).

## Buses and devices

| Bus | Pins | Devices |
|---|---|---|
| I2C | SDA 26, SCL 27 | ES8388 codec (`0x10`), PI4IOE5V6416 expander (`0x20`), LIS2DH12 accelerometer (`0x18`) |
| Shared SPI | MOSI 18, MISO 21, SCLK 2 | 4 × HT16D35x display drivers, CR95HF NFC reader. All chip selects are on the expander |
| SD SPI | MOSI 22, MISO 34, SCLK 14, CS 15 | microSD card (SPI mode, its own bus) |
| I2S | MCLK 0, BCLK 25, LRCLK 32, DOUT 33 | ES8388 (playback only; no data-in pin) |
| LEDC PWM | 12 / 23 / 19 | LED ring, red / green / blue |

## Chips

| Chip | Role | Notes |
|---|---|---|
| ES8388 | Audio codec: speaker and headphones | ESP32 is I2S master with MCLK on GPIO 0. Both output pairs are driven. Volume through the output volume registers |
| HT16D35x × 4 | LED-matrix drivers | 16 × 16 RGB, origin top-left. Each chip drives 4 rows × 16 pixels with 6-bit levels per colour. SPI at 4 MHz |
| ST CR95HF | NFC reader, ISO 14443-A, SPI at 2 MHz | Reads card UIDs (4 or 7 bytes). See cautions about audio |
| PI4IOE5V6416 | 16-bit IO expander | Above |
| LIS2DH12 | Accelerometer | Only Z is needed: about +12000 raw (±2 g range) = lying on its front (face down) |
| ETA6003 | Battery charger | No bus; two status lines on the expander |
| 18650 Li-ion cell | Battery | 2600 mAh |
| LED ring | Nightlight around the back | One colour for the whole ring (three PWM channels), **not** individually addressable |

There is no fuel gauge, RTC, wireless-charging receiver or USB-PD controller
on this board. yotwo gets the time from the network.

## Power

- **Soft power latch.** The power button wakes the board. The firmware must
  then drive expander pin 1.6 (`vinhold`) **high** early in boot, or the
  player switches off on battery as soon as the button is released. Driving it
  low switches the player off. yotwo writes safe output levels (latch high,
  amp off, chip selects high) *before* turning the pins into outputs.
- **Battery reading:** ADC1 channel 3 with 11 dB attenuation, battery mV =
  ADC mV × 82 / 39. The ADC reference is in eFuse (about 1100 mV).
- **Charging input:** the original charging dock feeds 5 V through the pogo
  pads on the base. Some docks were recalled, so a common modification is a
  USB-C socket wired to those pads:
  - **5 V only.** There is no regulator or USB-PD controller behind the pads.
  - A simple USB-C socket needs **5.1 kΩ resistors from CC1 and CC2 to
    ground**, or a USB-C charger will not switch 5 V on. A 4-wire
    pigtail usually already has them (only VBUS and GND are wired to the
    player).
  - Use a supply good for at least 2 A. Wi-Fi start-up plus the amplifier
    draws current in spikes, and a weak supply or weak cell can brown the
    board out.
  - A wireless-charging receiver would need to deliver 5 V at 2 A (10 W).

## Cautions

- **GPIO 12 is the flash-voltage strapping pin** and also drives the ring's
  red channel. If it is pulled high at reset the ESP32 selects 1.8 V flash and
  will not boot (eFuse `XPD_SDIO_FORCE` is not set, so the pin decides).
  Don't attach anything to it.
- **GPIO 0 and GPIO 2** are boot-mode strapping pins as well as MCLK and the
  SPI clock. GPIO 0 is the one you ground to enter the bootloader.
- **The NFC reader's field gets into the audio.** With the reader's RF field
  left on, the speaker crackles. yotwo switches the field on only for the few
  milliseconds of each card check.
- **The serial link is noisy.** Both boards tried were only reliable at
  115200 baud, and even then a long read occasionally drops bytes (one board
  far more than the other). Use 115200, and expect to retry long reads; see
  `tools/backup.sh`.
- **No auto-reset.** The adapter's DTR/RTS lines are not wired to EN/GPIO 0,
  so esptool cannot reset the chip into the bootloader. Use buttons or
  jumpers ([getting-started.md](getting-started.md)), and run esptool with
  `--before no_reset --after no_reset`.
- **Water damage.** A player that has been wet can lose single colours on
  individual pixels (corroded LED pads). yotwo's display test (web page,
  Settings → Display → Test the display) shows each colour full-screen, then
  sweeps rows and columns, which makes such pixels easy to spot.

## How the Gen 2 differs from the Yoto Mini

For anyone coming from the Mini projects ([open-yoto](https://github.com/tensorfish/open-yoto),
[Adafruit's guide](https://learn.adafruit.com/hacking-the-yoto-music-players)):

| Area | Gen 2 | Mini |
|---|---|---|
| Display | HT16D35x LED matrix, 4 chips | GC9306 colour TFT |
| Audio | ES8388, I2C `0x10` | AW88194 amp + ES8156 DAC |
| NFC | CR95HF over SPI | CR95HF over UART |
| SD | SPI | SDMMC 4-bit |
| Battery | ADC reading, ETA6003 | CW2215B fuel gauge, SGM41513 |
| I2C pins | SDA 26, SCL 27 | SDA 21, SCL 25 |
| I2S pins | 0 / 25 / 32 / 33 | 0 / 5 / 18 / 19 |
| IO expander | PI4IOE5V6416, no interrupt line | ET6416, interrupt on GPIO 34 |
| RTC, Qi, temperature sensor | none | present |
