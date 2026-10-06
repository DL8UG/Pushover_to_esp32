#include "render.h"

#include <cstring>

const FontGlyph *font_find(const Font &font, uint32_t cp)
{
    int lo = 0, hi = font.glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = font.glyphs[mid].codepoint;
        if (c == cp) return &font.glyphs[mid];
        if (c < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

void Canvas::clear()
{
    memset(m_buf, 0, sizeof(m_buf));
}

void Canvas::pixel(int x, int y, bool ink)
{
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return;
    uint8_t bit = 0x80 >> (x & 7);
    uint8_t &b = m_buf[y * (CANVAS_W / 8) + x / 8];
    if (ink) b |= bit;
    else b &= ~bit;
}

bool Canvas::get(int x, int y) const
{
    if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) return false;
    return m_buf[y * (CANVAS_W / 8) + x / 8] & (0x80 >> (x & 7));
}

void Canvas::fill_rect(int x, int y, int w, int h, bool ink)
{
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) pixel(px, py, ink);
    }
}

void Canvas::fill_circle(int cx, int cy, int r, bool ink)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r + r) pixel(cx + dx, cy + dy, ink);
        }
    }
}

void Canvas::ring(int cx, int cy, int r, int width, bool ink)
{
    int ri = r - width;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= r * r + r && d > ri * ri + ri) pixel(cx + dx, cy + dy, ink);
        }
    }
}

void Canvas::line(int x0, int y0, int x1, int y1, int width, bool ink)
{
    // Bresenham, stamping a disc of the stroke width at every step.
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int r = width / 2;
    for (;;) {
        if (width <= 1) pixel(x0, y0, ink);
        else fill_circle(x0, y0, r, ink);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Canvas::art(int x, int y, const char *const *rows, int row_count, bool ink)
{
    for (int ry = 0; ry < row_count; ry++) {
        for (int rx = 0; rows[ry][rx]; rx++) {
            if (rows[ry][rx] == '#') pixel(x + rx, y + ry, ink);
        }
    }
}

uint32_t utf8_next(const char *&p)
{
    const auto *s = reinterpret_cast<const uint8_t *>(p);
    uint32_t cp;
    int extra;
    if (s[0] < 0x80) { cp = s[0]; extra = 0; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; extra = 3; }
    else { p++; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { p++; return 0xFFFD; } // also stops at '\0'
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    p += 1 + extra;
    return cp;
}

namespace {

const FontGlyph &glyph_or_fallback(const Font &font, uint32_t cp)
{
    const FontGlyph *g = font_find(font, cp);
    if (!g) g = font_find(font, '?');
    return *g;
}

} // namespace

int text_width(const Font &font, const char *text, size_t len)
{
    int w = 0;
    const char *p = text, *end = text + len;
    while (p < end && *p) w += glyph_or_fallback(font, utf8_next(p)).advance;
    return w;
}

int text_width(const Font &font, const char *text)
{
    return text_width(font, text, strlen(text));
}

int draw_text(Canvas &c, const Font &font, int x, int baseline, const char *text, bool ink)
{
    const char *p = text;
    while (*p) {
        const FontGlyph &g = glyph_or_fallback(font, utf8_next(p));
        const uint8_t *rows = font.bitmap + g.offset;
        int stride = (g.w + 7) / 8;
        int x0 = x + g.x_off, y0 = baseline - g.top;
        for (int ry = 0; ry < g.h; ry++) {
            for (int rx = 0; rx < g.w; rx++) {
                if (rows[ry * stride + rx / 8] & (0x80 >> (rx & 7))) c.pixel(x0 + rx, y0 + ry, ink);
            }
        }
        x += g.advance;
    }
    return x;
}

int draw_text_centered(Canvas &c, const Font &font, int center_x, int baseline, const char *text, bool ink)
{
    return draw_text(c, font, center_x - text_width(font, text) / 2, baseline, text, ink);
}

int draw_text_right(Canvas &c, const Font &font, int right_x, int baseline, const char *text, bool ink)
{
    return draw_text(c, font, right_x - text_width(font, text), baseline, text, ink);
}
