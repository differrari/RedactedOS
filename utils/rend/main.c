#include "syscalls/syscalls.h"
#include "input_keycodes.h"
#include "ui/draw/draw.h"
#include "math/vector.h"

draw_ctx ctx = {};

typedef struct {
    float x;
    float y;
    float z;
} vector3;

#define NUM_VECS 8

typedef enum {
    prim_none,
    prim_pixel,
    prim_line,
    prim_trig,
    prim_quad
} primitives;

tern draw_segment(int i0, int i1, vector3* v, int num_verts, vector2 origin){
    if (i0 >= num_verts){
        print("Wrong index %i. %i vertices",i0,num_verts);
        return -1;
    }
    if (i1 >= num_verts){
        print("Wrong index %i. %i vertices",i1,num_verts);
        return -1;
    }
    vector3 v0 = v[i0];
    vector3 v1 = v[i1];
    
    float x0 = (v0.x/v0.z)+origin.x;
    float x1 = (v1.x/v1.z)+origin.x;
    float y0 = (v0.y/v0.z)+origin.y;
    float y1 = (v1.y/v1.z)+origin.y;
    
    fb_draw_line(&ctx, x0, y0, x1, y1, 0xFFB4DD13);
    
    return true;
}

int main(int argc, char* argv[]){
    request_draw_ctx(&ctx);
    vector2 mid = {ctx.width/2.f,ctx.height/2.f};
    vector3 v[NUM_VECS] = {
        {-100,-100,1.f},//0
        {100, -100,1.f},//1
        {100, 100,1.f},//0
        {-100, 100,1.f},//2
        
        {-100,-100,2.f},//0
        {100, -100,2.f},//1
        {100, 100,2.f},//0
        {-100, 100,2.f},//2
    };
    float last_time = get_time()/1000.f;
    // char buf[10] = {};
    primitives prim_type = prim_quad;
    
    int segments[24] = {
        0, 1, 2, 3,//FRONT
        0, 4, 7, 3,//LEFT
        1, 5, 6, 2,//RIGHT
        0, 4, 5, 1,//TOP
        3, 7, 6, 2,//BOTTOM
        4, 5, 6, 7,//BACK
    };
    
    if (!prim_type){
        print("No primitive type specified");
        return -1;
    }
    
    int num_segment_entries = sizeof(segments)/sizeof(int);
    
    if (num_segment_entries % prim_type != 0){
        print("Wrong number of segments, found %i, must be a multiple of %i",num_segment_entries, prim_type);
        return -1;
    }
    int num_segments = num_segment_entries/prim_type;
    int num_verts = sizeof(v)/sizeof(vector3);
    
    while (!should_close_ctx()){
        kbd_event ev = {};
        read_event(&ev);
        float time = get_time()/1000.f;
        float dt = time-last_time;
        last_time = time;
        if (ev.type == KEY_PRESS && ev.key == KEY_ESC) return 0;
        
        fb_clear(&ctx, 0);
        
        // string_format_buf(buf, 10, "%f", 1.f/dt);
        
        // fb_draw_string(&ctx, buf, 30, 30, 3, 0xFFFFFFFF);
        
        for (int i = 0; i < num_segments; i++){
            for (int j = 0; j < (int)prim_type; j++){
                int i0 = segments[(i * prim_type)+((j)%prim_type)];
                int i1 = segments[(i * prim_type)+((j+1)%prim_type)];
                if (draw_segment(i0, i1, v, num_verts, mid) == false) return -1;
            }
        }
        
        for (int i = 0; i < NUM_VECS; i++){
            v[i].z += dt/2;
        }
        commit_draw_ctx(&ctx);
    }
    destroy_draw_ctx(&ctx);
    return 0;
}