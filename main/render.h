#pragma once
#include <cstddef>
#include <cstdint>

#include "font.h"

constexpr int CANVAS_W = 200;
constexpr int CANVAS_H = 200;

// 1-bit framebuffer the whole UI is drawn into. Plain memory, no hardware
// access, so the same drawing code runs on the device (display.cpp ships
// the finished frame to the e-paper) and in the PC simulator (test/sim).
// `ink` = true draws black, false draws white (e.g. text on a black bar).
// Coordinates outside the canvas are silently clipped.
class Canvas {
public:
    void clear();
    void pixel(int x, int y, bool ink = true);
    bool get(int x, int y) const;
    void fill_rect(int x, int y, int w, int h, bool ink = true);
    // Line of the given stroke width (round caps), e.g. for icons.
    void line(int x0, int y0, int x1, int y1, int width = 1, bool ink = true);
    void fill_circle(int cx, int cy, int r, bool ink = true);
    // Ring with outer radius r and the given stroke width.
    void ring(int cx, int cy, int r, int width, bool ink = true);
    // Draws a picture given as rows of '#' (ink) and '.' (skip) characters.
    void art(int x, int y, const char *const *rows, int row_count, bool ink = true);

    // Raw pixels: CANVAS_H rows of CANVAS_W / 8 bytes, MSB = leftmost
    // pixel, bit set = ink.
    static constexpr int BYTES = CANVAS_W * CANVAS_H / 8;
    const uint8_t *bits() const { return m_buf; }

private:
    uint8_t m_buf[BYTES] = {};
};

// Decodes the next UTF-8 code point and advances `p`. Malformed bytes come
// back as U+FFFD one byte at a time, so truncated titles never hang or
// overrun.
uint32_t utf8_next(const char *&p);

// Text is UTF-8 and anchored at its baseline. Characters the font lacks
// are drawn as '?'.
int text_width(const Font &font, const char *text);
int text_width(const Font &font, const char *text, size_t len);
// Returns the pen x after the last glyph.
int draw_text(Canvas &c, const Font &font, int x, int baseline, const char *text, bool ink = true);
int draw_text_centered(Canvas &c, const Font &font, int center_x, int baseline, const char *text,
                       bool ink = true);
int draw_text_right(Canvas &c, const Font &font, int right_x, int baseline, const char *text,
                    bool ink = true);
