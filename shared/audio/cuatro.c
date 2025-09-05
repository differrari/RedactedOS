#include "cuatro.h"
#include "math/math.h"

float sample_raw_wave(WAVE_TYPE type, uint32_t accumulator, uint32_t period){
    float t = ((float)accumulator/(period*2));
    switch (type) {
        case WAVE_TRIG: {
            float trig = 2*(absf(t-floor(t + 0.5)));
            return trig;
        }
        case WAVE_SAW:
            return ((t-floor(t + 0.5)) + 0.5f);
        case WAVE_SQUARE:
            return (accumulator/period) % 2 == 0 ? 0 : 1;
    }
}

uint32_t sample_wave(WAVE_TYPE type, uint32_t accumulator, uint32_t period, uint32_t amplitude){
    return sample_raw_wave(type, accumulator, period) * amplitude;
}