#pragma once
#include "types.h"
#include "vector.h"

// axis aligned bounding box, 2D
typedef struct aabb2 {
    vector2 min;
    vector2 max;
} aabb2;

typedef struct ray2 {
    vector2 origin;
    vector2 dir;
} ray2;

bool aabb2_line_intersect(aabb2 bb, vector2 origin, vector2 dir, float *t_hit);
static inline bool aabb2_ray_intersect(aabb2 bb, ray2 ray, float *t_hit){
    return aabb2_line_intersect(bb, ray.origin, ray.dir, t_hit);
}

bool aabb2_check_movement(aabb2 moving_bb, aabb2 static_bb, vector2 dir, float *t_hit);

static inline bool aabb2_check_collision(aabb2 bb1, aabb2 bb2) {
    // collision = NOT (no gap left or no gap right or no gap top or no gap bottom)
    // use "NOT(a OR b OR ...) = NOT(a) AND NOT(b) ...":
    return (bb1.min.x < bb2.max.x) && (bb2.min.x < bb1.max.x) &&
           (bb1.min.y < bb2.max.y) && (bb2.min.y < bb1.max.y);
}