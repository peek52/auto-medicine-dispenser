#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "ui_core.h"
#include "ui_utf8_font_data.h"

static inline bool ui_utf8_is_cont_byte(uint8_t ch)
{
    return (ch & 0xC0u) == 0x80u;
}

static inline bool ui_utf8_has_non_ascii(const char *text)
{
    if (!text) return false;
    while (*text) {
        if (((uint8_t)*text) & 0x80u) return true;
        ++text;
    }
    return false;
}

static inline uint32_t ui_utf8_decode_next(const char **cursor)
{
    const uint8_t *s = (const uint8_t *)(*cursor);
    if (!s || !*s) return 0;

    uint32_t cp = 0;
    if (s[0] < 0x80u) {
        cp = s[0];
        *cursor += 1;
        return cp;
    }
    if ((s[0] & 0xE0u) == 0xC0u && s[1]) {
        cp = ((uint32_t)(s[0] & 0x1Fu) << 6) |
             ((uint32_t)(s[1] & 0x3Fu));
        *cursor += 2;
        return cp;
    }
    if ((s[0] & 0xF0u) == 0xE0u && s[1] && s[2]) {
        cp = ((uint32_t)(s[0] & 0x0Fu) << 12) |
             ((uint32_t)(s[1] & 0x3Fu) << 6) |
             ((uint32_t)(s[2] & 0x3Fu));
        *cursor += 3;
        return cp;
    }
    if ((s[0] & 0xF8u) == 0xF0u && s[1] && s[2] && s[3]) {
        cp = ((uint32_t)(s[0] & 0x07u) << 18) |
             ((uint32_t)(s[1] & 0x3Fu) << 12) |
             ((uint32_t)(s[2] & 0x3Fu) << 6) |
             ((uint32_t)(s[3] & 0x3Fu));
        *cursor += 4;
        return cp;
    }

    *cursor += 1;
    return '?';
}

static inline const ui_utf8_font_glyph_t *ui_utf8_find_glyph(uint32_t codepoint)
{
    for (uint16_t i = 0; i < kUiUtf8FontGlyphCount; ++i) {
        if (kUiUtf8FontGlyphs[i].codepoint == codepoint) return &kUiUtf8FontGlyphs[i];
    }
    return NULL;
}

static inline bool ui_utf8_is_above_mark(uint32_t cp)
{
    return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37) || (cp >= 0x0E47 && cp <= 0x0E4E);
}

static inline bool ui_utf8_is_upper_vowel(uint32_t cp)
{
    return cp >= 0x0E34 && cp <= 0x0E37;
}

static inline bool ui_utf8_is_tone_mark(uint32_t cp)
{
    return cp >= 0x0E48 && cp <= 0x0E4C;
}

static inline bool ui_utf8_is_below_mark(uint32_t cp)
{
    return cp >= 0x0E38 && cp <= 0x0E3A;
}

static inline bool ui_utf8_is_sara_am(uint32_t cp)
{
    return cp == 0x0E33;
}

static inline void ui_utf8_draw_glyph_mask(int16_t x, int16_t y, const ui_utf8_font_glyph_t *glyph, uint16_t color)
{
    if (!glyph || !glyph->bitmap || glyph->width == 0 || glyph->height == 0) return;

    uint32_t bit_index = 0;
    for (uint8_t row = 0; row < glyph->height; ++row) {
        int16_t run_start = -1;
        for (uint8_t col = 0; col < glyph->width; ++col, ++bit_index) {
            uint8_t byte = glyph->bitmap[bit_index >> 3];
            bool on = ((byte >> (7 - (bit_index & 7))) & 0x01u) != 0;
            if (on && run_start < 0) {
                run_start = col;
            } else if (!on && run_start >= 0) {
                fill_rect(x + run_start, y + row, col - run_start, 1, color);
                run_start = -1;
            }
        }
        if (run_start >= 0) {
            fill_rect(x + run_start, y + row, glyph->width - run_start, 1, color);
        }
    }
}

static inline int16_t ui_utf8_text_width(const char *text)
{
    int16_t width = 0;
    int16_t last_advance = 0;
    const char *cursor = text;
    while (cursor && *cursor) {
        uint32_t cp = ui_utf8_decode_next(&cursor);
        const ui_utf8_font_glyph_t *glyph = ui_utf8_find_glyph(cp);
        if (!glyph) glyph = ui_utf8_find_glyph('?');
        if (!glyph) continue;
        if (ui_utf8_is_above_mark(cp) || ui_utf8_is_below_mark(cp)) continue;
        if (ui_utf8_is_sara_am(cp)) {
            const ui_utf8_font_glyph_t *aa = ui_utf8_find_glyph(0x0E32);
            if (aa) {
                width += aa->advance;
                last_advance = aa->advance;
                continue;
            }
        }
        width += glyph->advance;
        last_advance = glyph->advance;
    }
    if (width > 0 && last_advance > 0) width -= 1;
    return width;
}

static inline int16_t ui_utf8_draw_text(int16_t x, int16_t y, const char *text, uint16_t color)
{
    int16_t cursor_x = x;
    int16_t last_base_x = x;
    uint32_t prev_cp = 0;
    const char *cursor = text;

    while (cursor && *cursor) {
        uint32_t cp = ui_utf8_decode_next(&cursor);
        const ui_utf8_font_glyph_t *glyph = ui_utf8_find_glyph(cp);
        if (!glyph) glyph = ui_utf8_find_glyph('?');
        if (!glyph) continue;

        if (ui_utf8_is_sara_am(cp)) {
            const ui_utf8_font_glyph_t *nikhahit = ui_utf8_find_glyph(0x0E4D);
            const ui_utf8_font_glyph_t *sara_aa = ui_utf8_find_glyph(0x0E32);

            if (nikhahit) {
                int16_t mark_x = last_base_x + nikhahit->x_offset;
                int16_t mark_y = y + nikhahit->y_offset;
                ui_utf8_draw_glyph_mask(mark_x, mark_y, nikhahit, color);
            }

            if (sara_aa) {
                int16_t aa_x = cursor_x + sara_aa->x_offset;
                int16_t aa_y = y + sara_aa->y_offset;
                ui_utf8_draw_glyph_mask(aa_x, aa_y, sara_aa, color);
                cursor_x += sara_aa->advance;
                last_base_x = cursor_x - sara_aa->advance;
                continue;
            }
        }

        int16_t draw_x = cursor_x + glyph->x_offset;
        int16_t draw_y = y + glyph->y_offset;

        if (ui_utf8_is_above_mark(cp)) {
            draw_x = last_base_x + glyph->x_offset;
            draw_y = y + glyph->y_offset;
            if (ui_utf8_is_tone_mark(cp) && (ui_utf8_is_upper_vowel(prev_cp) || prev_cp == 0x0E31)) {
                draw_y -= 6;
                draw_x += 1;
            }
        } else if (ui_utf8_is_below_mark(cp)) {
            draw_x = last_base_x + glyph->x_offset;
            draw_y = y + glyph->y_offset;
        } else {
            last_base_x = cursor_x;
        }

        ui_utf8_draw_glyph_mask(draw_x, draw_y, glyph, color);

        if (!(ui_utf8_is_above_mark(cp) || ui_utf8_is_below_mark(cp))) {
            cursor_x += glyph->advance;
        }

        prev_cp = cp;
    }

    return cursor_x;
}

static inline int ui_utf8_prev_char_len(const char *text)
{
    if (!text) return 0;
    int len = (int)strlen(text);
    while (len > 0 && ui_utf8_is_cont_byte((uint8_t)text[len - 1])) --len;
    return (int)strlen(text) - len + (len > 0 ? 1 : 0);
}

static inline bool ui_utf8_backspace(char *text)
{
    if (!text) return false;
    int len = (int)strlen(text);
    if (len <= 0) return false;
    do {
        --len;
    } while (len > 0 && ui_utf8_is_cont_byte((uint8_t)text[len]));
    text[len] = '\0';
    return true;
}

static inline bool ui_utf8_append(char *dst, size_t dst_cap, const char *utf8_char, size_t max_bytes)
{
    if (!dst || !utf8_char || dst_cap == 0) return false;
    size_t len = strlen(dst);
    size_t add_len = strlen(utf8_char);
    size_t limit = max_bytes < (dst_cap - 1) ? max_bytes : (dst_cap - 1);
    if (len + add_len > limit) return false;
    memcpy(dst + len, utf8_char, add_len + 1);
    return true;
}

static inline void ui_utf8_safe_truncate_copy(char *dst, size_t dst_cap, const char *src)
{
    if (!dst || dst_cap == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t used = 0;
    const char *cursor = src;
    while (*cursor) {
        const char *char_start = cursor;
        (void)ui_utf8_decode_next(&cursor);
        size_t char_len = (size_t)(cursor - char_start);
        if (used + char_len > dst_cap - 1) break;
        memcpy(dst + used, char_start, char_len);
        used += char_len;
    }
    dst[used] = '\0';
}
