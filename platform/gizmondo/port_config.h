// port specific settings

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define CASE_SENSITIVE_FS 0
#define DONT_OPEN_MANY_FILES 0
#define REDUCE_IO_CALLS 0
#define SIMPLE_WRITE_SOUND 0

#define SCREEN_SIZE_FIXED 1
#define SCREEN_WIDTH  321
#define SCREEN_HEIGHT 240

// draw.c
#define OVERRIDE_HIGHCOL 1

// draw2.c
#define START_ROW  0 // which row of tiles to start rendering at?
#define END_ROW   28 // ..end

// pico.c
#define CAN_HANDLE_240_LINES	1

// logging emu events
#define EL_LOGMASK (EL_STATUS|EL_IDLE) // (EL_STATUS|EL_ANOMALY|EL_UIO|EL_SRAMIO|EL_INTS|EL_CDPOLL) // xffff

//#define dprintf(f,...) printf("%05i:%03i: " f "\n",Pico.m.frame_count,Pico.m.scanline,##__VA_ARGS__)
#define dprintf(x...)

// platform
#define PLAT_MAX_KEYS 32
#define PLAT_HAVE_JOY 0
#define PATH_SEP      "/" // because of cegcc
#define PATH_SEP_C    '/'

#endif //PORT_CONFIG_H
