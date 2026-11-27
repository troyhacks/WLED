/*
    WLED-MM-P4 - HUB75 panel driver.

    The peripheral half of the panel driver: the bus, the frame buffer, and the
    continuous scan.

    Ported from MoonLight (https://github.com/MoonModules/MoonLight), file
    src/platform/esp32/platform_esp32_hub75.cpp, which is GPL-3.0 like WLED.
    See hub75/README.md for the mapping.

    @author Ewowi (MoonLight)
    @license GNU GENERAL PUBLIC LICENSE Version 3

    @technical
    ///
    /// ## The output is continuous, not one-shot
    ///
    /// A strand latches and holds a frame; a panel lights only while clocked, unlike every other output seam. So the start arms one endless scan of the buffer, the driver writes each frame into it between scans, and no call transmits per frame.
    ///
    /// ## Keeping a discrete-frame interface scanning
    ///
    /// The SDK's transactions are discrete frames, so a refill task queues the same buffer again as each scan completes, and the peripheral's own interrupt starts it, the producer and consumer way.
    ///
    /// Nothing re-arms from the interrupt: the completion callback runs before its slot is recycled, so a transmit there blocks on the empty pool, and a blocking wait in an interrupt is a watchdog panic, verified on every board preset.
    ///
    /// ## One scan in flight, which is not a choice
    ///
    /// Each transmit blocks in task context until its scan finishes, then queues the buffer again, and the interrupt starts it at once since the completion is still pending.
    ///
    /// The gap is one task wake, microseconds against a scan of a millisecond or more, and uniform: a fraction of a percent of brightness, not a flicker.
    ///
    /// A depth of two does not exist: the bus owns one descriptor list, and a second queued scan mounts it while the first still owns it, which fails outright.
    ///
    /// Right after start that failure can appear once or twice while the engine hands the descriptors back; the interrupt then restarts the list, which already holds this frame, so the panel loses nothing.
    ///
    /// The refill task runs on the core that allocated the LCD interrupt, since a wake from the other core cost 17% of the refresh rate, measured on a Waveshare board; it outranks the encode task, since a refill that lost the processor for a scan leaves the panel dark that long, and it stays alive until teardown deletes it by its handle.
    ///
    /// ## Both control lines must be real pins
    ///
    /// A panel has no command phase, yet the layer validates the data-command line first and rejects the whole bus without it, with a message that reads like a pin conflict and is not one.
    ///
    /// So it points at the write strobe, free since the command phase never asserts; the unused data lanes, which the layer also rejects unconnected, park on the strobe too, toggling with no panel line attached.
    ///
    /// ## The frame must be internal memory
    ///
    /// The peripheral keeps clocking while the transfer engine lags on external reads, signals completion with descriptors it still owns, and every later mount fails; one chip's external region cannot transfer at all.
    ///
    /// So a frame that does not fit internal memory is refused with the depth to lower: a panel that says why rather than one that scans torn.
*/

#include "Hub75Panel.h"
#include "Hub75Bus.h"   // the wire format, for the frame size: one home for it, shared with the encoder

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define HUB75_LCDCAM  SOC_LCDCAM_I80_LCD_SUPPORTED
#define HUB75_PARLIO  (SOC_PARLIO_SUPPORTED && SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH >= 16)

#if HUB75_LCDCAM || HUB75_PARLIO

#if HUB75_LCDCAM
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "hal/lcd_periph.h"      // soc_lcd_i80_signals: the OE lane's GPIO-matrix signal
#endif
#if HUB75_PARLIO
#include "driver/parlio_tx.h"
#include "hal/parlio_periph.h"   // soc_parlio_signals: the same, for Parlio
#endif
#include "esp_rom_gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace hub75 {

namespace {

// The parallel path's proven shift clock, an exact divide of the bus resolution: the component silently rounds an inexact rate into a wrong waveform.
constexpr uint32_t kPclkHz = 20000000;

// Parlio's ceiling, PER_FRAME's 0x7FFFF bits, is 65,535 bytes at any bus width, which makes a large wall an LCD_CAM job.
constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;

const char* g_lastError = nullptr;

// The OE bit means lit and the panel's OE is active low, so the pin is re-routed inverted and an idle bus is dark (Hub75Bus.h, "The bus word").
constexpr uint8_t kOeLane = Hub75Layout{}.oe;
// The bus lanes the layout uses, bit 0 (R1) to bit 14 (OE); the 16-bit bus's bit 15 is a hole.
constexpr size_t kBusLanes = 15;
void invertOe(gpio_num_t oe, int signal) { esp_rom_gpio_connect_out_signal(oe, signal, true, false); }

struct PanelState {
    Backend backend = Backend::LcdCam;
#if HUB75_LCDCAM
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    TaskHandle_t refill = nullptr;         // keeps the transaction queue full, see hub75RefillTask
    std::atomic<bool> refillParked{false}; // the refill task has stopped touching this state
#endif
#if HUB75_PARLIO
    parlio_tx_unit_handle_t parlio = nullptr;
#endif
    uint8_t* frame = nullptr;
    size_t   frameBytes = 0;
    // ATOMIC: the ISR callbacks read this to decide whether to touch `frame`, and teardown clears it from a task. A plain bool orders nothing between the two, so a callback could pass the check and then read a freed frame.
    std::atomic<bool> running{false};
    // Scans counted by the completion callback, so refresh is measured rather than calculated; atomic, as it crosses from the interrupt to the status tick.
    std::atomic<uint32_t> scans{0};
    uint32_t windowStartUs = 0;
    std::atomic<uint16_t> refreshHz{0};
};

// A panel is lit only while clocked: Parlio re-sends on its own, so its callback only counts, and LCD_CAM has a task queue each next scan.
#if HUB75_LCDCAM
bool IRAM_ATTR hub75DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
    auto* st = static_cast<PanelState*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// One scan in flight at a time: @xref{one-scan-in-flight-which-is-not-a-choice|why a second cannot be queued}.
constexpr size_t kLcdQueueDepth = 1;

void hub75RefillTask(void* arg) {
    auto* st = static_cast<PanelState*>(arg);
    while (st->running.load(std::memory_order_acquire)) {
        if (esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) != ESP_OK) break;
    }
    st->refillParked.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}
#endif

#if HUB75_PARLIO
// Parlio counts only: the unit loops the buffer on its own, so re-arming here would queue a second transmission against a transfer that never ends.
bool IRAM_ATTR hub75ParlioDoneCb(parlio_tx_unit_handle_t,
                                 const parlio_tx_done_event_data_t*, void* ctx) {
    auto* st = static_cast<PanelState*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}
#endif

void destroyState(PanelState* st) {
    if (!st) return;
    // Order matters: clear the flag, stop the peripheral (which disables its callback and waits for an in-flight one), and only then free the frame the callback reads.
    st->running.store(false, std::memory_order_release);
#if HUB75_LCDCAM
    if (st->refill) {
        // The refill task parks after the current scan and must be gone before the delete; the wait is bounded, since a leaked handle beats a hung teardown.
        bool parked = false;
        for (int i = 0; i < 100 && !parked; i++) {
            parked = st->refillParked.load(std::memory_order_acquire);
            if (!parked) vTaskDelay(1);
        }
        vTaskDelete(st->refill);
        st->refill = nullptr;
        if (!parked) st->io = nullptr;
    }
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
#endif
#if HUB75_PARLIO
    if (st->parlio) {
        parlio_tx_unit_disable(st->parlio);
        parlio_del_tx_unit(st->parlio);
    }
#endif
    if (st->frame) free(st->frame);
    delete st;
}

/// Every panel line in the bit order the layout declares; false when one is unset, such a port being dark rather than degraded.
bool buildPinOrder(const Pins& p, uint8_t scanRate, gpio_num_t* out) {
    // Bits 0-5: color. Bits 8-12: address. 13: latch. 14: OE. The gap at 6-7 is deliberate, because it matches Hub75Layout, which the encoder reads, so encoder and bus agree on every bit.
    const int16_t order[kBusLanes] = {
        p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
        -1, -1,                 // 6, 7: unused by the default layout
        p.a, p.b, p.c, p.d, p.e,
        p.lat, p.oe,
    };
    // How many address lines this panel's scan rate actually uses: a 1/8 panel leaves D and E unwired, and demanding them would refuse a perfectly good port.
    const uint8_t addrBits = (scanRate > 16) ? 5 : (scanRate > 8) ? 4 : 3;

    for (size_t bit = 0; bit < kBusLanes; bit++) {
        const bool isAddr = (bit >= 8 && bit <= 12);
        const bool needed = !isAddr || (bit - 8) < addrBits;
        if (!needed || order[bit] < 0) {
            // An unneeded or unwired line still occupies its bus position: the encoder puts data at fixed bit offsets, so a hole cannot shift the lines above it. NC parks it.
            out[bit] = GPIO_NUM_NC;
            continue;
        }
        out[bit] = static_cast<gpio_num_t>(order[bit]);
    }

    // The lines without which nothing lights.
    const int16_t required[] = {p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
                                p.a, p.b, p.c, p.clk, p.lat, p.oe};
    for (int16_t pin : required) {
        if (pin < 0) return false;
    }
    if (addrBits >= 4 && p.d < 0) return false;
    if (addrBits >= 5 && p.e < 0) return false;
    return true;
}

}  // namespace

const char* backendLabel(Backend backend) {
    switch (backend) {
        case Backend::Parlio: return "PARLIO";
        case Backend::LcdCam:
        default:               return "LCD_CAM";
    }
}

const char* lastError() { return g_lastError; }

bool backendAvailable(Backend backend, size_t frameBytes) {
    if (backend == Backend::LcdCam) {
#if HUB75_LCDCAM
        // LCD_CAM has no transfer-size cap of its own: the frame size is a memory question (internal DRAM first, see the allocation in init).
        (void)frameBytes;
        return true;
#else
        return false;
#endif
    }
#if HUB75_PARLIO
    // Parlio's single-shot transfer caps at 65,535 bytes, width-invariant. Offering it for a frame that cannot fit would be a choice that fails at init, so the ceiling is part of "available".
    return frameBytes <= kParlioMaxTransferBytes;
#else
    (void)frameBytes;
    return false;
#endif
}

bool init(Hub75Handle& handle, Backend backend, const Pins& pins, uint16_t width, uint16_t height,
          uint8_t scanRate, uint8_t bitDepth) {
    deinit(handle);
    g_lastError = nullptr;

    // The port's own rule: a real panel is 1/8, 1/16 or 1/32, and buildPinOrder derives the address width from it, so a config file's 3 would ask for an address no panel has. The rest of the geometry is `valid()`'s, below.
    if (scanRate != 8 && scanRate != 16 && scanRate != 32) {
        g_lastError = "scan rate must be 1/8, 1/16 or 1/32";
        return false;
    }

    // The size comes from the encoder, which is the one home for the wire format. Recomputing it here is how the two silently disagreed: this must be the exact buffer hub75Encode writes, or a correct encode overruns the DMA frame.
    Hub75Geometry geo;
    geo.width = width; geo.height = height; geo.scanRate = scanRate; geo.bitDepth = bitDepth;
    if (bitDepth > kHub75PortMaxBitDepth || !geo.valid()) {
        g_lastError = "the panel size, scan rate or bit depth is outside what the port encodes";
        return false;
    }

    gpio_num_t busPins[kBusLanes] = {};
    if (!buildPinOrder(pins, scanRate, busPins)) {
        g_lastError = "a HUB75 line is unset: every color, address and control pin is required";
        return false;
    }

    const size_t frameBytesPre = geo.frameBytes();
    if (!backendAvailable(backend, frameBytesPre)) {
        // Named, never substituted: a user keeping LCD_CAM free for strips must hear that the panel does not fit Parlio.
        g_lastError = (backend == Backend::Parlio)
            ? "this panel is too big for PARLIO (65,535 byte limit): use LCD_CAM or lower the depth"
            : "this chip has no LCD_CAM";
        return false;
    }

    auto* st = new (std::nothrow) PanelState();
    if (!st) {
        g_lastError = "out of memory";
        return false;
    }
    st->backend = backend;

    if (backend == Backend::Parlio) {
#if HUB75_PARLIO
        parlio_tx_unit_config_t cfg = {};
        cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;
        cfg.data_width = 16;              // the layout puts OE at bit 14
        cfg.clk_in_gpio_num = GPIO_NUM_NC;
        cfg.valid_gpio_num = GPIO_NUM_NC;
        cfg.clk_out_gpio_num = static_cast<gpio_num_t>(pins.clk);
        // Parlio ACCEPTS an NC data lane where the i80 layer rejects one, so the layout's holes (bits 6, 7 and 15) stay unparked here. The i80 branch below has to park them on WR instead.
        for (size_t i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++) {
            cfg.data_gpio_nums[i] = (i < kBusLanes) ? busPins[i] : GPIO_NUM_NC;
        }
        cfg.output_clk_freq_hz = kPclkHz;
        cfg.trans_queue_depth = 1;
        cfg.max_transfer_size = frameBytesPre;
        // The edge the panel's chips sample on: rising for most, falling for some, and the wrong one puts every pixel one column over.
        cfg.shift_edge = pins.clkFalling ? PARLIO_SHIFT_EDGE_NEG : PARLIO_SHIFT_EDGE_POS;
        if (parlio_new_tx_unit(&cfg, &st->parlio) != ESP_OK) {
            g_lastError = "could not claim the PARLIO unit: is another driver using it?";
            destroyState(st);
            return false;
        }
        invertOe(busPins[kOeLane], soc_parlio_signals[0].tx_units[0].data_sigs[kOeLane]);
        parlio_tx_event_callbacks_t cbs = {};
        cbs.on_trans_done = hub75ParlioDoneCb;
        parlio_tx_unit_register_event_callbacks(st->parlio, &cbs, st);
        parlio_tx_unit_enable(st->parlio);
#else
        g_lastError = "this chip has no PARLIO";
        destroyState(st);
        return false;
#endif
    } else {
#if HUB75_LCDCAM
    // The bus is 16 bits wide because the layout puts OE at bit 14: the color and control lines do not fit in 8.
    esp_lcd_i80_bus_config_t busCfg = {};
    // The data-command line rides the write strobe and must be a real pin: @xref{both-control-lines-must-be-real-pins|why leaving it unset fails the whole bus}.
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(pins.clk);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(pins.clk);   // WR IS the panel's shift clock
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    busCfg.bus_width = 16;
    // Every lane up to the bus width must be a real pin, so the ones the layout leaves as holes park on the strobe: @xref{both-control-lines-must-be-real-pins|the same reason}.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < 16) ? static_cast<gpio_num_t>(pins.clk) : GPIO_NUM_NC;
    }
    for (size_t i = 0; i < kBusLanes; i++) {
        if (busPins[i] != GPIO_NUM_NC) {
            busCfg.data_gpio_nums[i] = busPins[i];
        }
    }

    busCfg.max_transfer_bytes = frameBytesPre;
    busCfg.dma_burst_size = 64;
    const esp_err_t busErr = esp_lcd_new_i80_bus(&busCfg, &st->bus);
    if (busErr != ESP_OK) {
        // Name the reason: a refused pin returns ESP_ERR_INVALID_ARG, which a generic "in use" message would hide behind a conflict that does not exist.
        g_lastError = (busErr == ESP_ERR_INVALID_ARG)
            ? "the LCD bus refused a pin: check the board's GPIO map"
            : (busErr == ESP_ERR_NOT_FOUND)
                ? "no free LCD bus: another driver is using it"
                : "the LCD bus would not start";
        destroyState(st);
        return false;
    }

    invertOe(busPins[kOeLane], soc_lcd_i80_signals[0].data_sigs[kOeLane]);

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;
    ioCfg.pclk_hz = kPclkHz;
    ioCfg.trans_queue_depth = kLcdQueueDepth;   // why 1: see hub75RefillTask
    ioCfg.on_color_trans_done = hub75DoneCb;
    ioCfg.user_ctx = st;
    ioCfg.lcd_cmd_bits = 0;     // LCD_CAM: tx_color(-1) skips the command phase entirely
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;
    // Same edge choice as the Parlio branch: the display "writes" (samples) on the falling edge when set.
    ioCfg.flags.pclk_active_neg = pins.clkFalling ? 1 : 0;
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        g_lastError = "could not configure the LCD device";
        destroyState(st);
        return false;
    }
#else
        g_lastError = "this chip has no LCD_CAM";
        destroyState(st);
        return false;
#endif
    }

    // Internal transfer-capable memory and only that: @xref{the-frame-must-be-internal-memory|why external fails at this clock}. A single panel at this depth is a few tens of kilobytes and four are a few times that, both internal-sized.
    st->frame = static_cast<uint8_t*>(heap_caps_calloc(frameBytesPre, 1, MALLOC_CAP_DMA));
    if (!st->frame) {
        g_lastError = "the panel frame does not fit in memory: lower the bit depth";
        destroyState(st);
        return false;
    }
    st->frameBytes = frameBytesPre;
    st->windowStartUs = static_cast<uint32_t>(esp_timer_get_time());

    handle.impl = st;
    return true;
}

uint8_t* buffer(Hub75Handle& handle) {
    return handle.impl ? static_cast<PanelState*>(handle.impl)->frame : nullptr;
}

bool start(Hub75Handle& handle) {
    auto* st = static_cast<PanelState*>(handle.impl);
    if (!st || !st->frame) return false;
    if (st->running.load(std::memory_order_acquire)) return true;   // starting twice would queue two
    st->running.store(true, std::memory_order_release);
    bool ok = false;
    if (st->backend == Backend::Parlio) {
#if HUB75_PARLIO
        parlio_transmit_config_t xcfg = {};
        // Repeat forever until the unit is disabled: this is what keeps the panel clocked without an ISR re-arming it, and it is why the Parlio done callback only counts.
        xcfg.flags.loop_transmission = 1;
        // Parlio counts in BITS, the one API difference from the LCD_CAM path.
        ok = st->parlio && parlio_tx_unit_transmit(st->parlio, st->frame,
                                                   st->frameBytes * 8, &xcfg) == ESP_OK;
#endif
    } else {
#if HUB75_LCDCAM
        // The first scan is queued here, so the refill task starts by blocking on it rather than finding the bus idle.
        ok = st->io && esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) == ESP_OK;
        if (ok) {
            st->refillParked.store(false, std::memory_order_release);
            ok = xTaskCreatePinnedToCore(&hub75RefillTask, "hub75", 4096, st, 6, &st->refill, xPortGetCoreID()) == pdPASS;   // the core init ran on, which holds the LCD interrupt
            if (!ok) st->refill = nullptr;
        }
#endif
    }
    if (!ok) {
        st->running.store(false, std::memory_order_release);
        g_lastError = "the panel scan would not start";
    }
    return ok;
}

uint16_t refreshHz(const Hub75Handle& handle) {
    // The handle is a pointer to the state, and the state is what the reading
    // touches: the counters are atomic, the rest is the panel's own.
    auto* st = static_cast<PanelState*>(const_cast<void*>(handle.impl));
    if (!st || !st->running.load(std::memory_order_acquire)) return 0;
    // Averaged over a ~1 s window, recomputed on read. Cheap enough for a 1 Hz status poll, and it keeps the ISR down to one increment.
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t elapsed = now - st->windowStartUs;
    if (elapsed >= 1000000u) {
        const uint32_t scans = st->scans.exchange(0, std::memory_order_relaxed);
        st->refreshHz.store(static_cast<uint16_t>(
            (static_cast<uint64_t>(scans) * 1000000u) / elapsed),
            std::memory_order_relaxed);
        st->windowStartUs = now;
    }
    return st->refreshHz.load(std::memory_order_relaxed);
}

const char* backendName(const Hub75Handle& handle) {
    auto* st = static_cast<PanelState*>(const_cast<void*>(handle.impl));
    return st ? backendLabel(st->backend) : nullptr;
}

void deinit(Hub75Handle& handle) {
    destroyState(static_cast<PanelState*>(handle.impl));
    handle.impl = nullptr;
}

}  // namespace hub75

#else   // HUB75_LCDCAM || HUB75_PARLIO is false

namespace hub75 {

// Inert on silicon without the peripheral (the classic ESP32). The driver reports the cause, the same allocate-and-degrade rule every other output seam follows.
const char* backendLabel(Backend backend) {
    return (backend == Backend::Parlio) ? "PARLIO" : "LCD_CAM";
}
const char* lastError() { return "HUB75 needs an ESP32-S3, P4 or S31"; }
bool backendAvailable(Backend, size_t) { return false; }
bool init(Hub75Handle&, Backend, const Pins&, uint16_t, uint16_t, uint8_t, uint8_t) { return false; }
bool start(Hub75Handle&) { return false; }
uint8_t* buffer(Hub75Handle&) { return nullptr; }
uint16_t refreshHz(const Hub75Handle&) { return 0; }
const char* backendName(const Hub75Handle&) { return nullptr; }
void deinit(Hub75Handle&) {}

}  // namespace hub75

#endif  // HUB75_LCDCAM || HUB75_PARLIO
