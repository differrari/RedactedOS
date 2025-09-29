#include "cuatro.h"
#include "math/math.h"

float sample_raw_wave(WAVE_TYPE type, uint32_t phase){
    switch (type) {
        case WAVE_TRIG: {
            float t = ((float)phase/(float)PHASE_MAX);
            float trig = 2*(absf(t-floor(t + 0.5f)));
            return trig;
        }
        case WAVE_SAW:
            return (float)(PHASE_MAX - phase) / (float)PHASE_MAX;
        case WAVE_SQUARE:
            return (phase < PHASE_MID) ? 0.f : 1.f;
    }
    return 0;
}

uint32_t sample_wave(WAVE_TYPE type, uint32_t phase, uint32_t amplitude){
    return sample_raw_wave(type, phase) * amplitude;
}
