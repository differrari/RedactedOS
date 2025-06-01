#define BOOTSCREEN_TEXT "JesOS - The Christian Operating System"

#define BOOTSCREEN_NUM_SYMBOLS 12
#define BOOTSCREEN_NUM_STEPS 12
#define BOOTSCREEN_NUM_LINES 1

#define BOOTSCREEN_REPEAT 1

#define BOOTSCREEN_DIV 15

#define BOOTSCREEN_OFFSETS {\
    {-3,1},\
    {-1,1},\
    {-1,5},\
    {1,5},\
    {1,1},\
    {3,1},\
    {3,-1},\
    {1,-1},\
    {1,-5},\
    {-1,-5},\
    {-1,-1},\
    {-3,-1},\
}

#define BOOTSCREEN_ASYMM (gpu_point){0,50}
#define BOOTSCREEN_PADDING 10

#define BOOTSCREEN_INNER_X_CONST 30
#define BOOTSCREEN_OUTER_X_DIV 5
#define BOOTSCREEN_UPPER_Y_DIV 3
#define BOOTSCREEN_LOWER_Y_CONST 40

#define PANIC_TEXT "CARDINAL SIN"

#define BG_COLOR 0x703285

#define default_pwd "amen"