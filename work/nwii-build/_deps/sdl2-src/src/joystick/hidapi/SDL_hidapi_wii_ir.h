/* SkyBreak addition to SDL 2.28.5 (2026). Original basic Wii IR report decoder.
   Distributed under SDL's zlib license; see the SDL source LICENSE.txt.
   This is an altered source addition, not part of upstream SDL 2.28.5. */
#ifndef SKYBREAK_WII_IR_H
#define SKYBREAK_WII_IR_H
#include <math.h>
typedef struct {
    int count;
    unsigned short x[4], y[4];
    float centre_x, centre_y, horizon_x, horizon_y;
    int valid;
} SkyBreakWiiIR;
static SkyBreakWiiIR SkyBreakDecodeWiiIR(const unsigned char* bytes, unsigned size)
{
    SkyBreakWiiIR out = {0};
    unsigned i, j;
    float sum_x = 0, sum_y = 0, best = 0, dx = 0, dy = 0;
    if (!bytes || size != 10) return out;
    for (i = 0; i < 4; ++i) {
        const unsigned char* b = bytes + (i / 2) * 5;
        const unsigned x = i & 1 ? b[3] | ((b[2] & 3) << 8) : b[0] | ((b[2] & 0x30) << 4);
        const unsigned y = i & 1 ? b[4] | ((b[2] & 12) << 6) : b[1] | ((b[2] & 0xC0) << 2);
        if (x == 1023 && y == 1023) continue;
        if (y > 767) continue;
        out.x[out.count] = (unsigned short)x; out.y[out.count] = (unsigned short)y;
        ++out.count; sum_x += x; sum_y += y;
    }
    for (i = 0; i < (unsigned)out.count; ++i) for (j = i + 1; j < (unsigned)out.count; ++j) {
        float px = (float)out.x[j] - out.x[i], py = (float)out.y[j] - out.y[i];
        const float length = px * px + py * py;
        if (length > best) { best = length; dx = px; dy = py; }
    }
    if (out.count < 2 || best < 16) return out; /* one blob cannot establish the bar's horizon */
    if (dx < 0) { dx = -dx; dy = -dy; }
    out.centre_x = sum_x / out.count / 1023.0f;
    out.centre_y = sum_y / out.count / 767.0f;
    out.horizon_x = dx / sqrtf(best); out.horizon_y = -dy / sqrtf(best);
    out.valid = 1;
    return out;
}
#endif
