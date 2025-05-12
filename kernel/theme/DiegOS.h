#define BOOTSCREEN_TEXT "[REDACTED] OS - %c"

#define BOOTSCREEN_NUM_SYMBOLS 13
#define BOOTSCREEN_NUM_STEPS 12

#define BOOTSCREEN_OFFSETS {\
    {-2,-2},\
    {2,-2},\
    {2,2},\
    {-2,2},\
    {-2,0},\
    {-1,0},\
    {-1,1},\
    {1,1},\
    {1,0},\
    {1,-1},\
    {-1,-1},\
    {-1,0},\
    {-2,0 },\
}

#define BOOTSCREEN_ASYMM (point){0,0}
#define BOOTSCREEN_PADDING 10

#define BOOTSCREEN_INNER_X_CONST 30
#define BOOTSCREEN_OUTER_X_DIV 3
#define BOOTSCREEN_UPPER_Y_DIV 3
#define BOOTSCREEN_LOWER_Y_CONST 30

#define PANIC_TEXT "FATAL ERROR"