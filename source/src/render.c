/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lab.h"
#include "font_atlas.h"
#include <stdio.h>

static void rect(uint32_t *p, int x, int y, int w, int h, uint32_t c) {
    x+=(LAB_WIDTH-1280)/2;y+=(LAB_HEIGHT-720)/2;
    int endx = x + w, endy = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (endx > LAB_WIDTH) endx = LAB_WIDTH;
    if (endy > LAB_HEIGHT) endy = LAB_HEIGHT;
    for (int row = y; row < endy; ++row)
        for (int col = x; col < endx; ++col) p[row * LAB_WIDTH + col] = 0xff000000 | c;
}
static unsigned next_char(const unsigned char **s) {
    unsigned c = *(*s)++;
    if (c < 128) return c;
    if ((c & 0xe0) == 0xc0 && **s && (**s & 0xc0) == 0x80)
        return ((c & 31) << 6) | (*(*s)++ & 63);
    return '?';
}
static void text(uint32_t *p, int x, int y, const char *str, uint32_t color) {
    x+=(LAB_WIDTH-1280)/2;y+=(LAB_HEIGHT-720)/2;
    const unsigned char *s = (const unsigned char *)str;
    while (*s) {
        unsigned c = next_char(&s);
        int index = c >= 32 && c < 127 ? (int)c - 32 :
                    c >= 0x400 && c < 0x460 ? (int)c - 0x400 + 95 : '?' - 32;
        const unsigned char *glyph = lab_font + index * 32 * 36;
        for (int yy = 0; yy < 36; ++yy) for (int xx = 0; xx < 32; ++xx) {
            int px = x + xx, py = y + yy;
            if (px < 0 || px >= LAB_WIDTH || py < 0 || py >= LAB_HEIGHT) continue;
            unsigned a = glyph[yy * 32 + xx];
            if (!a) continue;
            uint32_t old = p[py * LAB_WIDTH + px], result = 0xff000000;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned v = (((color >> shift) & 255) * a + ((old >> shift) & 255) * (255 - a)) / 255;
                result |= v << shift;
            }
            p[py * LAB_WIDTH + px] = result;
        }
        x += lab_advances[index];
    }
}
void lab_render(const LabState *s, uint32_t *p) {
    const uint32_t white = 0xeef3fb, muted = 0xa3b4cb, mint = 0x73e3ce;
    char line[160];
    int ru=s->language;
    for(size_t i=0;i<LAB_WIDTH*LAB_HEIGHT;++i)p[i]=0xff0d1422;
    rect(p, 48, 48, 5, 83, mint);
    text(p, 76, 42, "PS4 AirPlay", white);
    text(p, 76, 86, "1.12-RC13 / Full HD", muted);
    rect(p, 48, 156, 1184, 176, 0x182336);
    snprintf(line, sizeof(line), ru?"Система: %s":"System: %s", s->firmware);
    text(p, 72, 170, line, white);
    snprintf(line, sizeof(line), "Safari: http://%s:%d", s->ip, LAB_PORT);
    text(p, 72, 213, line, s->net_rc == 0 ? mint : 0xf1ba79);
    snprintf(line, sizeof(line), "VideoOut: %d   AudioOut: %d   Pad: %d", s->video_rc, s->audio_rc, s->pad_rc);
    text(p, 72, 257, line, muted);
    snprintf(line, sizeof(line), ru?"HTTP: %s / код %d / подключений %lu":"HTTP: %s / code %d / connections %lu",
             s->network_stage, s->net_rc, s->connections);
    text(p, 72, 294, line, s->net_rc == 0 ? mint : 0xf1ba79);
    if (s->pattern) {
        const uint32_t colors[] = {0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x202020};
        for (int i = 0; i < 8; ++i) rect(p, 48 + i * 148, 358, 148, 128, colors[i]);
        rect(p, 48, 492, 1184, 12, 0x26364d);
        rect(p, 48 + (int)((s->frames * 6) % 1144), 492, 40, 12, mint);
        text(p, 60, 516, ru?"Тест вывода. Декодирование H.264 не проверено.":"Display test. H.264 decoding is not tested here.", muted);
    } else {
        text(p, 60, 354, ru?"X  Пауза / продолжение прямого видео":"X  Pause / resume direct video", white);
        text(p, 60, 402, ru?"Треугольник  Цветные полосы и движение":"TRIANGLE  Color bars and motion", white);
        text(p, 60, 450, ru?"Квадрат  Видео / статус     Options  Настройки":"SQUARE  Video / status     OPTIONS  Settings", white);
        snprintf(line, sizeof(line), ru?"HEN SDK: 0x%x / права: %s / код %d":"HEN SDK: 0x%x / privileges: %s / code %d", s->hen_sdk_version,
                 s->hen_active ? (ru?"получены":"active") : (ru?"нет":"none"), s->hen_acquire_rc);
        text(p, 60, 508, line, muted);
    }
    rect(p, 48, 584, 1184, 82, 0x182336);
    snprintf(line, sizeof(line), ru?"Видео: %lu кадров / %ux%u / ошибок %lu":"Video: %lu frames / %ux%u / errors %lu", s->video_decoded,s->video_width,s->video_height,s->video_errors);
    text(p, 68, 593, line, white);
    text(p, 68, 628, s->recovery_mode ? (ru?"Запуск приостановлен. Скачайте отчёт в Safari.":"Startup paused. Download a report in Safari.") :
         s->video_packets && !s->video_stream_active ? (ru?"Видеосеанс завершён. Подключитесь с iPhone снова.":"Video session ended. Reconnect from iPhone.") : (ru?"iPhone и PS4 должны быть в одной локальной сети.":"iPhone and PS4 must use the same local network."), muted);
}
