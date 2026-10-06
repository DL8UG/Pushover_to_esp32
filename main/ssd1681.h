#pragma once
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"

// Minimal driver for the SSD1681 e-paper controller (200x200, black/white)
// on its 4-wire SPI interface, written after the SSD1681 datasheet. It
// only uses the waveforms stored in the controller's OTP: display mode 1
// for a full refresh, mode 2 for a partial one - no custom LUTs.
//
// Frames are 200 rows of 25 bytes, MSB = leftmost pixel, bit set = white
// (the controller's own RAM format). Not thread-safe: display.cpp
// serializes all calls with its mutex.
class Ssd1681 {
public:
    static constexpr int WIDTH = 200;
    static constexpr int HEIGHT = 200;
    static constexpr int FRAME_BYTES = WIDTH / 8 * HEIGHT;

    struct Pins {
        gpio_num_t cs, dc, rst, busy, mosi, sck;
        spi_host_device_t host;
    };

    explicit Ssd1681(const Pins &pins);

    // Sets up SPI and GPIOs, resets and initialises the controller.
    void begin();

    // Full refresh (the whole panel flashes): clears ghosting. Also makes
    // `frame` the reference for following partial refreshes.
    void full_refresh(const uint8_t *frame);

    // Partial refresh: only changed pixels switch, no flashing. Leaves
    // slight ghosting over time - follow up with full_refresh() now and
    // then.
    void partial_refresh(const uint8_t *frame);

private:
    void reset();
    void init_registers();
    void wait_idle();
    void command(uint8_t cmd);
    void data(const uint8_t *bytes, int len);
    void data(uint8_t byte) { data(&byte, 1); }
    void write_ram(uint8_t ram_cmd, const uint8_t *frame);
    void update(uint8_t sequence);

    Pins m_pins;
    spi_device_handle_t m_spi = nullptr;
};
