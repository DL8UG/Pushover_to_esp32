#include "ssd1681.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char *TAG = "ssd1681";

// SSD1681 commands (datasheet, "Command Table")
constexpr uint8_t DRIVER_OUTPUT_CONTROL = 0x01;
constexpr uint8_t DATA_ENTRY_MODE = 0x11;
constexpr uint8_t SW_RESET = 0x12;
constexpr uint8_t TEMPERATURE_SENSOR = 0x18;
constexpr uint8_t MASTER_ACTIVATION = 0x20;
constexpr uint8_t UPDATE_CONTROL_2 = 0x22;
constexpr uint8_t WRITE_RAM_BW = 0x24;  // new image
constexpr uint8_t WRITE_RAM_RED = 0x26; // previous image, for mode 2
constexpr uint8_t BORDER_WAVEFORM = 0x3C;
constexpr uint8_t RAM_X_RANGE = 0x44;
constexpr uint8_t RAM_Y_RANGE = 0x45;
constexpr uint8_t RAM_X_COUNTER = 0x4E;
constexpr uint8_t RAM_Y_COUNTER = 0x4F;

// Display update sequences for UPDATE_CONTROL_2: clock + analog on, load
// temperature and the OTP waveform, display, analog + clock off.
constexpr uint8_t SEQ_FULL = 0xF7;    // display mode 1
constexpr uint8_t SEQ_PARTIAL = 0xFF; // display mode 2

constexpr uint8_t BORDER_WHITE = 0x05;  // full refresh: border follows white LUT
constexpr uint8_t BORDER_KEEP = 0x80;   // partial: border held at VSS, no flicker

// A full refresh takes ~2 s; a stuck BUSY line must not hang the caller.
constexpr int64_t BUSY_TIMEOUT_US = 10 * 1000000LL;

// SPI DMA needs internal RAM; frames passed in may live anywhere.
DMA_ATTR uint8_t s_dma_frame[Ssd1681::FRAME_BYTES];

} // namespace

Ssd1681::Ssd1681(const Pins &pins) : m_pins(pins) {}

void Ssd1681::begin()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = m_pins.mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = m_pins.sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = FRAME_BYTES;
    ESP_ERROR_CHECK(spi_bus_initialize(m_pins.host, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 20 * 1000 * 1000; // SSD1681 write cycle: 50 ns min
    dev.mode = 0;
    dev.spics_io_num = m_pins.cs;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(m_pins.host, &dev, &m_spi));

    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask = (1ULL << m_pins.dc) | (1ULL << m_pins.rst);
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {};
    in.mode = GPIO_MODE_INPUT;
    in.pin_bit_mask = 1ULL << m_pins.busy;
    ESP_ERROR_CHECK(gpio_config(&in));

    reset();
    init_registers();
}

void Ssd1681::reset()
{
    gpio_set_level(m_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(m_pins.rst, 0); // hardware reset: low for >= 10 ms
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(m_pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    wait_idle();
    command(SW_RESET);
    wait_idle();
}

void Ssd1681::init_registers()
{
    // 200 gate lines (MUX = 199), default scan direction
    const uint8_t driver_output[] = {(HEIGHT - 1) & 0xFF, (HEIGHT - 1) >> 8, 0x00};
    command(DRIVER_OUTPUT_CONTROL);
    data(driver_output, sizeof(driver_output));

    command(DATA_ENTRY_MODE);
    data(0x03); // X and Y increment, X first: plain row-major frames

    const uint8_t x_range[] = {0, WIDTH / 8 - 1};
    command(RAM_X_RANGE);
    data(x_range, sizeof(x_range));
    const uint8_t y_range[] = {0, 0, (HEIGHT - 1) & 0xFF, (HEIGHT - 1) >> 8};
    command(RAM_Y_RANGE);
    data(y_range, sizeof(y_range));

    command(BORDER_WAVEFORM);
    data(BORDER_WHITE);
    command(TEMPERATURE_SENSOR);
    data(0x80); // internal sensor, selects the OTP waveform by temperature
}

void Ssd1681::wait_idle()
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(m_pins.busy) == 1) {
        if (esp_timer_get_time() - start > BUSY_TIMEOUT_US) {
            ESP_LOGW(TAG, "BUSY still high after %lld ms, giving up",
                     (long long)(BUSY_TIMEOUT_US / 1000));
            return;
        }
        vTaskDelay(1);
    }
}

void Ssd1681::command(uint8_t cmd)
{
    gpio_set_level(m_pins.dc, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &cmd;
    ESP_ERROR_CHECK(spi_device_polling_transmit(m_spi, &t));
}

void Ssd1681::data(const uint8_t *bytes, int len)
{
    gpio_set_level(m_pins.dc, 1);
    spi_transaction_t t = {};
    t.length = 8 * len;
    t.tx_buffer = bytes;
    ESP_ERROR_CHECK(spi_device_polling_transmit(m_spi, &t));
}

void Ssd1681::write_ram(uint8_t ram_cmd, const uint8_t *frame)
{
    command(RAM_X_COUNTER);
    data(0);
    const uint8_t y0[] = {0, 0};
    command(RAM_Y_COUNTER);
    data(y0, sizeof(y0));

    memcpy(s_dma_frame, frame, FRAME_BYTES);
    command(ram_cmd);
    data(s_dma_frame, FRAME_BYTES);
}

void Ssd1681::update(uint8_t sequence)
{
    command(UPDATE_CONTROL_2);
    data(sequence);
    command(MASTER_ACTIVATION);
    wait_idle();
}

void Ssd1681::full_refresh(const uint8_t *frame)
{
    command(BORDER_WAVEFORM);
    data(BORDER_WHITE);
    write_ram(WRITE_RAM_BW, frame);
    write_ram(WRITE_RAM_RED, frame); // reference for the next partial refresh
    update(SEQ_FULL);
}

void Ssd1681::partial_refresh(const uint8_t *frame)
{
    // Mode 2 drives each pixel by its old (RED RAM) and new (B/W RAM)
    // value, so only changed pixels switch.
    command(BORDER_WAVEFORM);
    data(BORDER_KEEP);
    write_ram(WRITE_RAM_BW, frame);
    update(SEQ_PARTIAL);
    write_ram(WRITE_RAM_RED, frame);
}
