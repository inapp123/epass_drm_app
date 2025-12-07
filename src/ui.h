#pragma once
#include "fbdraw.h"
#include "crrefont.h"
#include "drm_warpper.h"

typedef enum {
    UI_STATE_MAINMENU,
    UI_STATE_BRIGHTNESS,
    UI_STATE_SWITCH_INTERVAL,
    UI_STATE_SWITCH_MODE,
    UI_STATE_DISPLAY_PIC,
    UI_STATE_NONE,
} ui_state_t;

typedef enum {
    SW_INTERVAL_1MIN,
    SW_INTERVAL_3MIN,
    SW_INTERVAL_5MIN,
    SW_INTERVAL_10MIN,
} sw_interval_t;

typedef enum {
    SW_MODE_SEQUENCE,
    SW_MODE_RANDOM,
    SW_MODE_MANUAL,
} sw_mode_t;

typedef enum {
    TRANSITION_FILL_LEFT_RIGHT,
    TRANSITION_FILL_TOP_BOTTOM,
    TRANSITION_OPERATOR_INFO,
    TRANSITION_NONE,
} transition_state_t;

typedef struct {
    uint64_t magic; // EPASCONF
    uint32_t version;
    int brightness;
    sw_interval_t switch_interval;
    sw_mode_t switch_mode;
} ui_epass_config_t;

typedef struct {
    fbdraw_t *drawer;
    CRREFont *font;
    ui_state_t state;
    int hover_index;
    struct libevdev *evdev;
    int evdev_fd;
    int brightness;
    sw_interval_t switch_interval;
    sw_mode_t switch_mode;
    int state_updated;
    transition_state_t transition_state;
    long long transition_start_time;
    drm_warpper_t *drm_warpper;
    char transition_bitmap_path[128];
    char operator_info_path[128];
    void (*transition_middle_cb[UI_TRANSITION_MIDDLE_CB_COUNT])(void);
    uint32_t transition_bg_color;
} ui_t;

void ui_init(ui_t *ui,int width,int height,uint32_t* vaddr,drm_warpper_t *drm_warpper);
void ui_destroy(ui_t *ui);
void ui_tick(ui_t *ui);
void ui_process_input(ui_t *ui);
void ui_start_transition(ui_t *ui,transition_state_t state);
void ui_set_transition_bitmap_path(ui_t *ui,char *path);
void ui_set_operator_info_path(ui_t *ui,char *path);
void ui_add_transition_middle_cb(ui_t *ui,void (*cb)(void));
void ui_set_transition_bg_color(ui_t *ui,uint32_t color);