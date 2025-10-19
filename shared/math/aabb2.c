#include "aabb2.h"
#include "math.h"

bool aabb2_line_intersect(aabb2 bb, vector2 origin, vector2 dir, float *t_hit)
{
    vector2 t_min = (vector2) {-INFINITY, -INFINITY};
    vector2 t_max = (vector2) { INFINITY,  INFINITY};

    // vertical (x_min...x_max) slab of AABB intersecting with line
    if (dir.x != 0.0f) {
        float invx = 1.0f / dir.x;
        float t1 = (bb.min.x - origin.x) * invx;
        float t2 = (bb.max.x - origin.x) * invx;
        t_min.x = minf(t1, t2);
        t_max.x = maxf(t1, t2);
    } else {
        if ((origin.x < bb.min.x) || (origin.x > bb.max.x)) return false;
    }

    // horizontal (y_min...y_max) slab of AABB intersecting with line
    if (dir.y != 0.0f) {
        float invy = 1.0f / dir.y;
        float t1 = (bb.min.y - origin.y) * invy;
        float t2 = (bb.max.y - origin.y) * invy;
        t_min.y = minf(t1, t2);
        t_max.y = maxf(t1, t2);
    } else {
        // horizontal case (check if line is outside)
        if ((origin.y < bb.min.y) || (origin.y > bb.max.y)) return false;
    }

    // enter and exit intersection (if ray was an infinite line)
    float t_enter = maxf(t_min.x, t_min.y);
    float t_exit  = minf(t_max.x, t_max.y);

    // limit line to ray (t=0..1) and check interval validity
    if (minf(t_exit, 1.0f) < maxf(t_enter, 0.0f)) return false;

    // select first valid hit (if we want to know)
    // * entry: origin outside AABB
    // * exit:  origin inside AABB
    if (t_hit) *t_hit = (t_enter >= 0.0f) ? t_enter : t_exit;

    return true;
}

bool aabb2_check_movement(aabb2 moving_bb, aabb2 static_bb, vector2 dir, float *t_hit){
    // basically equivalent to
    // * reduce moving AABB to ray/line segment (with center as origin)
    // * and expand static AABB symmetrically by mABB size
    // * collision/intersection test
    vector2 mbb_half_size = vector2_scale(vector2_sub(moving_bb.max, moving_bb.min), 0.5f);
    aabb2 expanded_sbb = (aabb2){
        vector2_sub(static_bb.min, mbb_half_size), 
        vector2_add(static_bb.max, mbb_half_size)
    };
    ray2 reduced_mbb = (ray2){
        vector2_scale(vector2_add(moving_bb.min, moving_bb.max), 0.5f),
        dir
    };
    bool collision = aabb2_ray_intersect(expanded_sbb, reduced_mbb, t_hit);
    if (!collision || !t_hit) return false;

    if (float_zero(*t_hit)){
        
    }

    return true;
}