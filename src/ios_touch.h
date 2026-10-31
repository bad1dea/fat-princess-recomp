// On-screen controller (iOS). Bits follow the PS3 pad; the stick is -32767..32767.
#pragma once
#include <stdint.h>

enum {
    FP_TOUCH_CROSS = 1u << 0, FP_TOUCH_CIRCLE = 1u << 1, FP_TOUCH_SQUARE = 1u << 2, FP_TOUCH_TRIANGLE = 1u << 3,
    FP_TOUCH_START = 1u << 4, FP_TOUCH_SELECT = 1u << 5, FP_TOUCH_L1 = 1u << 6, FP_TOUCH_R1 = 1u << 7,
    FP_TOUCH_L3 = 1u << 8, FP_TOUCH_R3 = 1u << 9, FP_TOUCH_L2 = 1u << 14, FP_TOUCH_R2 = 1u << 15, FP_TOUCH_MENU = 1u << 16,
};

typedef struct fp_touch_state { uint32_t buttons; int32_t lx, ly; } fp_touch_state;

#ifdef __cplusplus
extern "C" {
#endif
void fp_touch_tick(void);                  // main thread, once per frame
// Main thread: safe content rectangle (normalized) and view size in UIKit points.
void fp_touch_overlay_area(float area[6]);
int fp_touch_keyboard_connected(void);
int fp_touch_controls_visible(void);
int fp_touch_controller_family(void);     // 0 unknown, 1 PlayStation, 2 Xbox, 3 Nintendo
void fp_touch_read(fp_touch_state* out);   // any thread
int fp_touch_take_menu(void);              // 1 once per press of the menu button
unsigned fp_touch_pad_bits(void);          // held controls as CELL_PAD_CTRL bits (menu navigation)
#ifdef __cplusplus
}
#endif
