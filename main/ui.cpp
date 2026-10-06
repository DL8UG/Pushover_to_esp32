#include "ui.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "fonts.h"

namespace {

constexpr int MARGIN = 8;
constexpr int BAR_H = 26; // status bar (home) / header bar (message)

// ---- icons ---------------------------------------------------------------

// Diagonal "off" stroke across an icon, with a white halo so it stays
// readable on top of the icon's own black pixels.
void slash(Canvas &c, int x, int y, int size)
{
    c.line(x, y, x + size, y + size, 5, false);
    c.line(x, y, x + size, y + size, 2, true);
}

// WiFi fan: two arcs and a dot inside a 90-degree wedge pointing up,
// anchored at its bottom tip (cx, by). 18 x 14 pixels.
void icon_wifi(Canvas &c, int cx, int by, bool ok)
{
    for (int dy = -14; dy <= 0; dy++) {
        for (int dx = -14; dx <= 14; dx++) {
            if (dx > -dy || -dx > -dy) continue; // outside the wedge
            int d = dx * dx + dy * dy;
            bool outer = d <= 14 * 14 && d > 11 * 11;
            bool middle = d <= 8 * 8 && d > 5 * 5;
            bool dot = d <= 2 * 2 + 2;
            if (outer || middle || dot) c.pixel(cx + dx, by + dy);
        }
    }
    if (!ok) slash(c, cx - 9, by - 13, 16);
}

// Pushover: black disc with a white "P", 17 pixels across; when down,
// only the outline, crossed out.
void icon_pushover(Canvas &c, int cx, int cy, bool ok)
{
    if (ok) {
        c.fill_circle(cx, cy, 8);
        draw_text_centered(c, FONT_B10, cx + 1, cy + 5, "P", false);
    } else {
        c.ring(cx, cy, 8, 2);
        draw_text_centered(c, FONT_B10, cx + 1, cy + 5, "P");
        slash(c, cx - 8, cy - 8, 16);
    }
}

const char *const BELL[] = {
    "......##......",
    "....######....",
    "...########...",
    "..##########..",
    "..##########..",
    "..##########..",
    "..##########..",
    ".############.",
    ".############.",
    "##############",
    "##############",
    "..............",
    ".....####.....",
    "......##......",
};
constexpr int BELL_W = 14, BELL_H = 14;

void icon_bell(Canvas &c, int x, int y, bool on)
{
    c.art(x, y, BELL, BELL_H);
    if (!on) slash(c, x - 1, y - 1, BELL_W + 1);
}

const char *const WARNING[] = {
    "........#........",
    ".......###.......",
    ".......###.......",
    "......#####......",
    "......##.##......",
    ".....###.###.....",
    ".....###.###.....",
    "....####.####....",
    "....####.####....",
    "...###########...",
    "...#####.#####...",
    "..#############..",
    ".###############.",
    "#################",
};
constexpr int WARNING_H = 14;

// ---- text layout ---------------------------------------------------------

constexpr int MAX_LINES = 8;
constexpr int LINE_BYTES = 96;
struct Lines {
    char text[MAX_LINES][LINE_BYTES];
    int count = 0;
    bool complete = true; // false: text did not fit into MAX_LINES / max_lines
};

// Appends `len` bytes to `line` if the result still fits `max_w`.
bool try_append(const Font &f, char *line, const char *s, size_t len, int max_w)
{
    size_t cur = strlen(line);
    if (cur + len >= LINE_BYTES) return false;
    char tmp[LINE_BYTES];
    memcpy(tmp, line, cur);
    memcpy(tmp + cur, s, len);
    tmp[cur + len] = '\0';
    if (text_width(f, tmp) > max_w) return false;
    memcpy(line, tmp, cur + len + 1);
    return true;
}

// Word-wraps UTF-8 `text` into at most `max_lines` lines of `max_w` pixels.
// Words longer than a whole line are broken between characters. Fills
// `out` in place - Lines is ~800 bytes, too much to copy around on the
// small task stacks this runs on.
void wrap(const Font &f, const char *text, int max_w, int max_lines, Lines &out)
{
    out.count = 0;
    out.complete = true;
    if (max_lines > MAX_LINES) max_lines = MAX_LINES;
    char *line = out.text[0];
    line[0] = '\0';
    out.count = 1;

    auto new_line = [&]() -> bool {
        if (out.count == max_lines) { out.complete = false; return false; }
        line = out.text[out.count++];
        line[0] = '\0';
        return true;
    };

    const char *p = text;
    while (*p) {
        if (*p == '\n') { p++; if (!new_line()) break; continue; }
        if (*p == ' ' || *p == '\r' || *p == '\t') { p++; continue; }

        const char *w = p;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
        size_t wlen = p - w;

        if (line[0]) {
            // " word" onto the current line, else the word starts a new one
            char sp_word[LINE_BYTES];
            if (wlen + 1 < LINE_BYTES) {
                sp_word[0] = ' ';
                memcpy(sp_word + 1, w, wlen);
                if (try_append(f, line, sp_word, wlen + 1, max_w)) continue;
            }
            if (!new_line()) break;
        }
        if (try_append(f, line, w, wlen, max_w)) continue;

        // Too long even for a line of its own: break it character-wise.
        const char *q = w;
        while (q < p) {
            const char *start = q;
            utf8_next(q);
            if (!try_append(f, line, start, q - start, max_w)) {
                if (!line[0]) break; // a single glyph wider than the line
                if (!new_line()) break;
                q = start;
            }
        }
        if (!out.complete) break;
    }
    // Drop a trailing empty line from a final '\n'.
    if (out.count > 1 && !out.text[out.count - 1][0]) out.count--;
}

// Ends the last line with "…", dropping characters until it fits.
void add_ellipsis(const Font &f, char *line, int max_w)
{
    static const char ELLIPSIS[] = "\xE2\x80\xA6";
    for (;;) {
        size_t len = strlen(line);
        while (len > 0 && line[len - 1] == ' ') line[--len] = '\0';
        if (len + sizeof(ELLIPSIS) <= LINE_BYTES) {
            char tmp[LINE_BYTES];
            snprintf(tmp, sizeof(tmp), "%s%s", line, ELLIPSIS);
            if (text_width(f, tmp) <= max_w || len == 0) {
                memcpy(line, tmp, strlen(tmp) + 1);
                return;
            }
        }
        // remove the last UTF-8 character
        do { len--; } while (len > 0 && (line[len] & 0xC0) == 0x80);
        line[len] = '\0';
    }
}

int line_height(const Font &f) { return f.ascent + f.descent; }

// ---- screens -------------------------------------------------------------

void render_home(const UiState &s, Canvas &c)
{
    // Status bar: connection icons left, alarm bell right.
    icon_wifi(c, MARGIN + 10, 19, s.wifi_ok);
    icon_pushover(c, MARGIN + 38, 12, s.pushover_ok);
    icon_bell(c, CANVAS_W - MARGIN - BELL_W, 5, s.alarm_enabled);
    char clock[8] = "--:--";
    if (clock_is_set(s.now)) {
        time_t t = (time_t)s.now;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(clock, sizeof(clock), "%H:%M", &tm);
    }
    draw_text_centered(c, FONT_B14, CANVAS_W / 2, 19, clock);
    c.fill_rect(0, BAR_H, CANVAS_W, 1);

    const int banner_h = 40;
    const int bottom = s.unreachable ? CANVAS_H - banner_h : CANVAS_H;
    const int mid = (BAR_H + bottom) / 2;

    if (s.count > 0) {
        char num[8];
        snprintf(num, sizeof(num), s.count > 99 ? "99+" : "%d", s.count);
        // big digits: 55 px tall above the baseline
        int base = mid + 16;
        draw_text_centered(c, FONT_DIGITS, CANVAS_W / 2, base, num);
        draw_text_centered(c, FONT_B14, CANVAS_W / 2, base + 30, s.count == 1 ? "message" : "messages");
    } else {
        int cy = mid - 12;
        c.ring(CANVAS_W / 2, cy, 26, 4);
        c.line(88, cy + 1, 97, cy + 10, 5);
        c.line(97, cy + 10, 113, cy - 8, 5);
        draw_text_centered(c, FONT_B14, CANVAS_W / 2, cy + 50, "All clear");
    }

    if (s.unreachable) {
        int y = CANVAS_H - banner_h;
        c.fill_rect(0, y, CANVAS_W, banner_h);
        const int text_w = text_width(FONT_B14, "NOT CONNECTED");
        int x = (CANVAS_W - (17 + 8 + text_w)) / 2;
        c.art(x, y + (banner_h - WARNING_H) / 2, WARNING, WARNING_H, false);
        draw_text(c, FONT_B14, x + 17 + 8, y + banner_h / 2 + 7, "NOT CONNECTED", false);
    }
}

void render_message(const UiState &s, Canvas &c)
{
    // Header bar: position left, date right, white on black.
    c.fill_rect(0, 0, CANVAS_W, BAR_H);
    const int hb = 18; // header baseline
    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", s.index + 1, s.total);
    int x = draw_text(c, FONT_B12, MARGIN, hb, pos, false);
    if (s.priority >= 2) {
        // emergency: inverted "!!" badge next to the position
        int w = text_width(FONT_B12, "!!") + 8;
        c.fill_rect(x + 6, 5, w, BAR_H - 10, false);
        draw_text(c, FONT_B12, x + 10, hb, "!!", true);
    }
    if (s.date > 0) {
        time_t t = (time_t)s.date;
        struct tm tm;
        localtime_r(&t, &tm);
        char when[24];
        strftime(when, sizeof(when), "%d.%m. %H:%M", &tm);
        draw_text_right(c, FONT_B12, CANVAS_W - MARGIN, hb, when, false);
    }

    // Page dots at the bottom when there is more than one message.
    const bool dots = s.total > 1 && s.total * 9 <= CANVAS_W - 2 * MARGIN;
    const int area_top = BAR_H + 4;
    const int area_bottom = dots ? CANVAS_H - 18 : CANVAS_H - 4;
    if (dots) {
        int x0 = CANVAS_W / 2 - (s.total - 1) * 9 / 2;
        for (int i = 0; i < s.total; i++) {
            if (i == s.index) c.fill_circle(x0 + i * 9, CANVAS_H - 9, 3);
            else c.ring(x0 + i * 9, CANVAS_H - 9, 3, 1);
        }
    }

    // Title: the largest font it fits into completely, else the smallest
    // one with "…". Vertically centred in the remaining space.
    const char *title = s.title[0] ? s.title : "(no title)";
    const Font *const sizes[] = {&FONT_B24, &FONT_B18, &FONT_B14, &FONT_B12};
    const int max_w = CANVAS_W - 2 * MARGIN;
    const Font *font = nullptr;
    // Static, not on the stack: rendering runs on small task stacks
    // (button task, timer task), and only ever one frame at a time -
    // display.cpp serializes it with its mutex.
    static Lines lines;
    for (const Font *f : sizes) {
        font = f;
        wrap(*f, title, max_w, (area_bottom - area_top) / line_height(*f), lines);
        if (lines.complete) break;
    }
    if (!lines.complete) add_ellipsis(*font, lines.text[lines.count - 1], max_w);

    const int lh = line_height(*font);
    int y = area_top + (area_bottom - area_top - lines.count * lh) / 2 + font->ascent;
    for (int i = 0; i < lines.count; i++, y += lh) {
        draw_text(c, *font, MARGIN, y, lines.text[i]);
    }
}

} // namespace

bool clock_is_set(int64_t unix_time)
{
    return unix_time >= 1735689600; // 2025-01-01
}

void ui_render(const UiState &s, Canvas &c)
{
    c.clear();
    if (s.screen == UiState::Screen::Home) render_home(s, c);
    else render_message(s, c);
}
