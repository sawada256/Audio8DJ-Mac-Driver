// Float32 <-> signed 24-bit, shared by the engine and the encoder tests.
//
// Both directions use 2^23, the CoreAudio convention, so a 24-bit source that
// went through float comes back bit for bit. Scaling by 2^23 - 1 on the way
// out and truncating, as before, turned every positive sample into one LSB
// less than it should be.
#ifndef A8DJ_SAMPLE_CONVERT_H
#define A8DJ_SAMPLE_CONVERT_H
#include <math.h>
#include <stdint.h>

#define A8DJ_S24_SCALE 8388608.0f

static inline int32_t A8DJ_FloatToS24(float x)
{
    if (!isfinite(x)) return 0;
    float s = x * A8DJ_S24_SCALE;
    if (s >=  8388607.0f) return  8388607;
    if (s <= -8388608.0f) return -8388608;
    return (int32_t)lrintf(s);
}

static inline float A8DJ_S24ToFloat(int32_t v)
{
    return (float)v * (1.0f / A8DJ_S24_SCALE);
}
#endif
