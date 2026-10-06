#pragma once
#include <cstdint>

// Proportional 1-bit bitmap font. The tables themselves are generated from
// the BDF sources in fonts/ by tools/fontgen.py into fonts_data.cpp - edit
// the generator's font list, not the generated file.
struct FontGlyph {
    uint32_t codepoint;
    uint32_t offset;  // first byte of this glyph in Font::bitmap
    uint8_t w, h;     // bitmap size in pixels
    int8_t x_off;     // left bearing: bitmap column 0 sits at pen x + x_off
    int8_t top;       // bitmap row 0 sits this many pixels above the baseline
    uint8_t advance;  // pen movement after this glyph
};

struct Font {
    const FontGlyph *glyphs;  // sorted by codepoint
    uint16_t glyph_count;
    const uint8_t *bitmap;    // rows MSB-first, each row padded to whole bytes
    uint8_t ascent;           // pixels above the baseline
    uint8_t descent;          // pixels below the baseline
};

// nullptr if the font has no glyph for `cp`.
const FontGlyph *font_find(const Font &font, uint32_t cp);
