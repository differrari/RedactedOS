#include <arm_neon.h>

typedef struct vector2 {
    float x,y;
} vector2;

static inline float magnitude_vector2(vector2 v)
{
    float32x2_t xy  = vld1_f32(&v.x);        // [x, y]
    float32x2_t sq  = vmul_f32(xy, xy);      // [x^2, y^2]
    float32x2_t s   = vpadd_f32(sq, sq);     // [x^2+y^2, x^2+y^2]
    s = vmax_f32(s, vdup_n_f32(1e-20f));     // avoid 0/denorm -> NaNs/Infs

    float32x2_t rinv = vrsqrte_f32(s);       // ~1/sqrt(s)

    return 1.f/rinv[0];
}