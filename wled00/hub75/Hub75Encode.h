#pragma once

/*
    WLED-MM-P4 - HUB75 panel driver.

    The frame encoder: the RGB picture becomes bit planes of 16-bit bus words,
    one word per pixel clock (a slot).

    Ported from MoonLight (https://github.com/MoonModules/MoonLight), file
    src/platform/Hub75Encode.h, which is GPL-3.0 like WLED.  The namespace is
    MoonLight's "mm" renamed to "hub75"; see hub75/README.md.

    @author Ewowi (MoonLight)
    @license GNU GENERAL PUBLIC LICENSE Version 3
*/

#include "Hub75Bus.h"   // Hub75Layout, Hub75Geometry: the bus contract the encode builds

#include <cstddef>   // size_t
#include <cstdint>

namespace hub75 {

/// HUB75 encode: the frame the driver hands a port, as bit planes of 16-bit bus words, one word per pixel clock (a slot). A pure data transform on the bus contract (Hub75Bus.h).
///
/// @attribution mrcodetastic, hzeller, ESPHome
///
/// @technical
///
/// ## The wire layout
///
/// One encoded frame, outermost first:
///
///   for each bit plane p (0 = least significant)
///     for each scan row r
///       hold words, then one word per column x -> one bus word: the six color bits for
///                                     (x, r) and (x, r + rows), addressed to
///                                     the row in the latch; the last column's
///                                     word also latches row r, dark
///
/// Every word on the bus is a clock, so the latch rides the last column's word: a word of its own would shift one pixel more than the panel holds. The hold words lead the row's words only where the row is narrow (Hub75Bus.h, Hold words); a wider row's group is its columns exactly.
///
/// ## The address lags the data by one row
///
/// A panel lights where output-enable, the addressed row and the latched row meet, and the latch holds the row last strobed. So row r's words name row r - 1, the latch word moves the address to r in the dark, and row 0's words name the last row.
///
/// The lit window follows the data in the latch: row 0's words light the last row of the plane before, so they take that plane's window, and plane 0's take the top plane's, where the buffer wraps.
///
/// A leading address (Hub75Bus.h, `addressLeadsData`) is the other convention: row r's words name row r itself and the latch names it too, which is what a panel whose shift register belongs to the row needs. The lit window is then this plane's own, since the latch holds the plane's row. The word the address changes on stays dark: the row the panel shows changes there, and it must not change while the panel is lit.
///
/// ## Brightness is time
///
/// A panel's chips only switch a column on or off, so plane p lights 2^p as long as plane 0 and brightness shortens every plane's window: plane p lights `2^p × brightness × (pass - 1) / (255 × 2^(bitDepth-1))` words of its pass, rounded. The hold words (Hub75Bus.h, Hold words) make the weights exact on a narrow row's stretched pass at full brightness.
///
/// The lowest plane rounds to zero below brightness `255 × 2^(bitDepth-2) / (pass - 1)`: a quarter for a 64-word row at 6 bits and a 256-word row at 8 bits (below 64 the picture is 7-bit), half for a stretched row, where the lowest plane is one word at full brightness.

namespace detail {

/// One body, compiled twice: with `kHold` false the hold words are the constant 0, a wide row's per-word loop.
template <bool kHold>
inline size_t hub75EncodeFrame(const uint8_t* rgb, uint8_t* out, const Hub75Geometry& geo) {
    // The default layout, a compile-time constant: every bit position below folds into an immediate.
    constexpr Hub75Layout lay{};
    const uint16_t rows = geo.scanRows();
    const uint16_t pairs = geo.rowsPerScan() / 2;   // color passes per address step
    const uint16_t pairSpan = geo.height / 2;       // upper row pairs with row + span
    // Only the first `addrBits` are read, so a 1/8 panel leaves D and E unread.
    constexpr uint8_t addr[5] = {lay.a, lay.b, lay.c, lay.d, lay.e};
    uint8_t addrBits = 3;                       // 1/8 scan
    if (geo.scanRate > 8) addrBits = 4;         // 1/16
    if (geo.scanRate > 16) addrBits = 5;        // 1/32

    // A row's address lines as one mask, built once per scan row rather than bit by bit on every word.
    auto addrMask = [&](uint16_t row) {
        uint16_t m = 0;
        for (uint8_t bit = 0; bit < addrBits; bit++)
            if ((row >> bit) & 1) m |= static_cast<uint16_t>(1u << addr[bit]);
        return m;
    };
    const size_t holdWords = kHold ? geo.holdWords() : 0;
    const size_t passWords = geo.rowWords() + holdWords;
    // Lit words in plane p's window: one division per plane, and one more for row 0's group, which lights the plane before. The numerator is 64-bit: 32 bits overflow above ~131,000 row words at 8 bits.
    const uint32_t span = 255u << (geo.bitDepth - 1);
    auto windowWords = [&](uint8_t p) {
        return static_cast<uint32_t>((static_cast<uint64_t>(passWords - 1) * geo.brightness * (static_cast<uint64_t>(1) << p) + span / 2) / span);
    };

    size_t w = 0;
    for (uint8_t plane = 0; plane < geo.bitDepth; plane++) {
        // Depth < 8 keeps the HIGH bits: dropping those would dim every bright pixel.
        const uint8_t shift = static_cast<uint8_t>(8 - geo.bitDepth + plane);
        const uint32_t planeWindow = windowWords(plane);
        for (uint16_t r = 0; r < rows; r++) {
            // The row in the LATCH while these words clock in.  When the address leads the data it is the row being encoded; when it lags, it is the previous one, and for row 0 the last row of the previous plane or frame.
            const uint16_t latched = geo.addressLeadsData
                ? r
                : static_cast<uint16_t>((r + rows - 1) % rows);
            // Lit words name the latched row; the latch word strobes row r and moves the address to it, in the dark.
            const uint16_t darkWord = addrMask(latched);
            const uint16_t litWord = static_cast<uint16_t>(darkWord | (1u << lay.oe));
            const uint16_t latchWord = static_cast<uint16_t>(addrMask(r) | (1u << lay.lat));
            // The plane whose data the latch holds while these words clock in, the previous one for row 0: its window is the one these words light.  With a leading address the latch holds this plane's own row, so the window is this plane's.
            const uint32_t litWords = (!geo.addressLeadsData && r == 0)
                ? windowWords(static_cast<uint8_t>((plane + geo.bitDepth - 1) % geo.bitDepth))
                : planeWindow;
            // The hold words lead the pass: no data shifts in yet (color 0), the address names the latched row, and OE follows the window like any other word.
            if constexpr (kHold) {
                for (size_t i = 0; i < holdWords; i++) {
                    // The first word of the pass is the one the address changes on, so it stays dark when the address leads.
                    const bool addressWord = (i == 0 && geo.addressLeadsData);
                    const uint16_t word = (!addressWord && i < litWords) ? litWord : darkWord;
                    // Little-endian, matching how the peripheral latches a 16-bit bus word.
                    out[w++] = static_cast<uint8_t>(word & 0xFF);
                    out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
                }
            }
            for (uint16_t pair = 0; pair < pairs; pair++)
            for (uint16_t x = 0; x < geo.width; x++) {
                // Pair p of step r is row r + p*scanRate, paired half a panel below.
                const uint16_t upRow = static_cast<uint16_t>(r + pair * geo.scanRate);
                const size_t up = (static_cast<size_t>(upRow) * geo.width + x) * 3;
                const size_t lo = (static_cast<size_t>(upRow + pairSpan) * geo.width + x) * 3;
                uint16_t word = 0;
                if ((rgb[up + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r1);
                if ((rgb[up + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g1);
                if ((rgb[up + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b1);
                if ((rgb[lo + 0] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.r2);
                if ((rgb[lo + 1] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.g2);
                if ((rgb[lo + 2] >> shift) & 1) word |= static_cast<uint16_t>(1u << lay.b2);
                // The last word latches; the rest are lit inside this plane's window and dark after it. The index counts from the pass start, hold words first, so the window spans both.
                const size_t i = holdWords + static_cast<size_t>(pair) * geo.width + x;
                // The word the address changes on is dark when the address leads: the row the panel shows changes there, and it must not change while the panel is lit.
                const bool addressWord = (i == 0 && geo.addressLeadsData);
                word |= (i == passWords - 1) ? latchWord : ((!addressWord && i < litWords) ? litWord : darkWord);
                // Little-endian, matching how the peripheral latches a 16-bit bus word.
                out[w++] = static_cast<uint8_t>(word & 0xFF);
                out[w++] = static_cast<uint8_t>((word >> 8) & 0xFF);
            }
        }
    }
    // The frame ends on the last row's latch word, dark, and row 0's data words name that last row: the loop closes with every row lit once.
    return w;
}

}  // namespace detail

/// Encode one RGB frame into @xref{the-wire-layout|bit planes>, @xref{the-address-lags-the-data-by-one-row|the address a row late>, @xref{brightness-is-time|brightness as time>; bytes written, or 0 on a bad geometry: a dark frame beats half a frame.
inline size_t hub75Encode(const uint8_t* rgb, uint8_t* out, const Hub75Geometry& geo) {
    if (!rgb || !out) return 0;
    if (!geo.valid()) return 0;
    return geo.holdWords() ? detail::hub75EncodeFrame<true>(rgb, out, geo)
                           : detail::hub75EncodeFrame<false>(rgb, out, geo);
}

}  // namespace hub75
