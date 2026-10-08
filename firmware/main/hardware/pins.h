#pragma once
// Yoto Player Gen 2 ("V2") pin map, from the stock firmware's hardware config.
// See docs/hardware.md. Unverified on hardware until bring-up.

// I2C: ES8388 codec, PI4IOE5V6416 IO expander, LIS2DH12 accelerometer
#define PIN_I2C_SDA 26
#define PIN_I2C_SCL 27
#define I2C_ADDR_IOX    0x20
#define I2C_ADDR_ES8388 0x10
#define I2C_ADDR_ACCEL  0x18  // LIS2DH12 (the stock firmware reads it here)

// I2S to the ES8388 (the ESP32 is master and supplies MCLK)
#define PIN_I2S_MCLK 0
#define PIN_I2S_BCLK 25
#define PIN_I2S_WS   32
#define PIN_I2S_DOUT 33

// Shared SPI bus: 4 x HT16D35x display drivers and the CR95HF NFC reader.
// Their chip selects are on the IO expander.
#define PIN_SPI_MOSI 18
#define PIN_SPI_MISO 21
#define PIN_SPI_SCLK 2

// SD card, on its own SPI bus
#define PIN_SD_MOSI 22
#define PIN_SD_MISO 34
#define PIN_SD_SCLK 14
#define PIN_SD_CS   15

// Rotary encoders (push buttons are on the IO expander)
#define PIN_ENC1_A 5
#define PIN_ENC1_B 13
#define PIN_ENC2_A 4
#define PIN_ENC2_B 35

// Nightlight RGB. GPIO 12 is the flash-voltage strapping pin: it must be low at reset.
#define PIN_NL_RED   12
#define PIN_NL_GREEN 23
#define PIN_NL_BLUE  19

// ADC1 channels
#define ADC_CH_LIGHT   0  // GPIO 36
#define ADC_CH_BATTERY 3  // GPIO 39, battery mV = pin mV * 82 / 39

// IO expander pins, as (port << 3) | bit
#define IOX(port, bit) (((port) << 3) | (bit))
#define IOX_DISP_CS3     IOX(0, 0)  // rows 12-15
#define IOX_DISP_CS2     IOX(0, 1)  // rows 8-11
#define IOX_DISP_CS1     IOX(0, 2)  // rows 4-7
#define IOX_DISP_CS0     IOX(0, 3)  // rows 0-3
#define IOX_ENC1_PUSH    IOX(0, 4)  // in, low = pressed (assumed, as the power button)
#define IOX_ENC2_PUSH    IOX(0, 5)  // in
#define IOX_AMP_EN       IOX(0, 6)  // out, high = speaker amplifier on
#define IOX_NFC_IRQ_IN   IOX(0, 7)  // out, pulse low to wake the CR95HF
#define IOX_NFC_IRQ_OUT  IOX(1, 0)  // in, low = response ready
#define IOX_HP_DETECT    IOX(1, 1)  // in, low = headphones inserted
#define IOX_TILT         IOX(1, 2)  // in, accelerometer interrupt
#define IOX_POWER_BUTTON IOX(1, 3)  // in, low = pressed
#define IOX_NFC_CS       IOX(1, 4)  // out, low = selected
#define IOX_PLUG_STATUS  IOX(1, 5)  // in, charger plugged (polarity unknown)
#define IOX_VIN_HOLD     IOX(1, 6)  // out, high = stay powered
#define IOX_CHG_STATUS   IOX(1, 7)  // in, charging (polarity unknown)

// Direction registers (1 = input) and safe initial output levels: chip selects high,
// NFC wake line high, power latch high, amplifier off.
#define IOX_DIR_P0 0x30
#define IOX_DIR_P1 0xAF
#define IOX_OUT_P0 0xBF
#define IOX_OUT_P1 0xFF
// Inputs with the expander's internal pull-up: knob buttons; power button, plug and
// charge status (the stock config's p0PullEn / p1PullEn).
#define IOX_PULL_P0 0x30
#define IOX_PULL_P1 0xA8
