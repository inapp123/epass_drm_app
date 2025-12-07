#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "ui.h"
#include "log.h"
#include "fbdraw.h"
#include "crrefont.h"
#include "RREFont/rre_chicago_20x24.h"
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include "config.h"
#include "prts_logo.h"

extern void set_brightness(int brightness);
extern void set_switch_interval(sw_interval_t interval);
extern void set_switch_mode(sw_mode_t mode);
extern void next_video();
extern void prev_video();
extern void enter_usb_download();
extern bool ui_blocked();


long long get_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000ll + tv.tv_usec;
}

static fbdraw_t g_fbdraw;


void rrefont_rect_cb(int x, int y, int w, int h, int c){
    fbdraw_fill_rect(&g_fbdraw, x, y, w, h, c);
}

const char *ui_mainmenu_text[] = {
    "Brightness",
    "Switch Interval",
    "Switch Mode",
    "USB Download"
};

static void ui_draw_mainmenu(ui_t *ui){
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 10, "====== MAIN MENU ======");
   
    for (int i = 0; i < sizeof(ui_mainmenu_text) / sizeof(ui_mainmenu_text[0]); i++) {
        CRREFont_printf(ui->font, 10, UI_Y_OFFSET + 50 + i * 30, ui->hover_index == i ? "> %s" : "  %s", ui_mainmenu_text[i]);
    }
}

static void ui_draw_brightness(ui_t *ui){
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 10, "====== Brightness ======");
    CRREFont_printf(ui->font, 10, UI_Y_OFFSET + 50, "Level: %d/10", ui->brightness);
    
    int bar_x = 10;
    int bar_y = UI_Y_OFFSET + 90;
    int bar_w = 300;
    int bar_h = 30;
    
    fbdraw_fill_rect(ui->drawer, bar_x, bar_y, bar_w, bar_h, 0xFF404040);
    fbdraw_fill_rect(ui->drawer, bar_x, bar_y, (bar_w * ui->brightness) / 10, bar_h, 0xFF00FF00);
    
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 140, "KEY_1 Brightness Up");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 170, "KEY_2 Brightness Down");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 200, "KEY_3/4 Back");

}

const char *interval_text[] = {"1 Min", "3 Min", "5 Min", "10 Min"};

static void ui_draw_switch_interval(ui_t *ui){
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 10, "====== Switch Interval ======");
    
    for (int i = 0; i < 4; i++) {
        CRREFont_printf(ui->font, 10, UI_Y_OFFSET + 50 + i * 30, ui->hover_index == i ? "> %s" : "  %s", interval_text[i]);
    }
    
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 200, "KEY_1 Go Up");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 230, "KEY_2 Go Down");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 260, "KEY_3 Confirm");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 290, "KEY_4 Back");

}

const char *mode_text[] = {"Sequence", "Random", "Manual"};

static void ui_draw_switch_mode(ui_t *ui){
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 10, "====== Switch Mode ======");
    
    for (int i = 0; i < 3; i++) {
        CRREFont_printf(ui->font, 10, UI_Y_OFFSET + 50 + i * 30, ui->hover_index == i ? "> %s" : "  %s", mode_text[i]);
    }
    
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 200, "KEY_1 Go Up");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 230, "KEY_2 Go Down");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 260, "KEY_3 Confirm");
    CRREFont_printStr(ui->font, 10, UI_Y_OFFSET + 290, "KEY_4 Back");
}

static void ui_draw_display_pic(ui_t *ui){
    int ret = fbdraw_argb_bitmap_from_file(ui->drawer, 0, 0, 360, 640, UI_DISPLAY_PIC_PATH);
    if (ret < 0) {
        fbdraw_fill_rect(ui->drawer, 0, 0, 360, 640, UI_BACKGROUND_COLOR);
        CRREFont_printStr(ui->font, 10, 20, "=== NO PICTURE ===");
        CRREFont_printStr(ui->font, 10, 50, "Please put a picture");
        CRREFont_printStr(ui->font, 10, 80, "in /assets/display.argb");
        CRREFont_printStr(ui->font, 10, 110, "Press Any Key ....");
    }
}

static void ui_load_config(ui_t *ui){
    FILE *f = fopen(UI_CONFIG_FILE_PATH, "rb");
    ui_epass_config_t config;
    if(f == NULL){
        log_error("failed to open config file");
        return;
    }
    fread(&config, sizeof(ui_epass_config_t), 1, f);
    if(config.magic != UI_CONFIG_MAGIC){
        log_error("invalid config file");
        return;
    }
    if(config.version != UI_CONFIG_VERSION){
        log_error("invalid config file version");
        return;
    }
    ui->brightness = config.brightness;
    ui->switch_interval = config.switch_interval;
    ui->switch_mode = config.switch_mode;
    set_brightness(config.brightness);
    set_switch_interval(config.switch_interval);
    set_switch_mode(config.switch_mode);
    fclose(f);
}


static void ui_save_config(ui_t *ui){
    FILE *f = fopen(UI_CONFIG_FILE_PATH, "wb");
    ui_epass_config_t config;
    config.magic = UI_CONFIG_MAGIC;
    config.version = UI_CONFIG_VERSION;
    config.brightness = ui->brightness;
    config.switch_interval = ui->switch_interval;
    config.switch_mode = ui->switch_mode;
    fwrite(&config, sizeof(ui_epass_config_t), 1, f);
    fclose(f);
}

static void ui_handle_key(ui_t *ui, int key){

    if(ui->transition_state != TRANSITION_NONE || ui_blocked()){
        return;
    }

    ui->state_updated = 1;
    switch (ui->state) {
        case UI_STATE_NONE:
            if (key == KEY_1) {
                prev_video();
            }if (key == KEY_2) {
                next_video();
            }if (key == KEY_3) {
                ui->state = UI_STATE_DISPLAY_PIC;
            }
            if (key == KEY_4) {
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 0;
            }
            break;
            
        case UI_STATE_MAINMENU:
            if (key == KEY_1) {
                if (ui->hover_index > 0) ui->hover_index--;
            } else if (key == KEY_2) {
                if (ui->hover_index < 3) ui->hover_index++;
            } else if (key == KEY_3) {
                switch (ui->hover_index) {
                    case 0:
                        ui->state = UI_STATE_BRIGHTNESS;
                        break;
                    case 1:
                        ui->state = UI_STATE_SWITCH_INTERVAL;
                        ui->hover_index = ui->switch_interval;
                        break;
                    case 2:
                        ui->state = UI_STATE_SWITCH_MODE;
                        ui->hover_index = ui->switch_mode;
                        break;
                    case 3:
                        enter_usb_download();
                        break;
                }
            } else if (key == KEY_4) {
                ui->state = UI_STATE_NONE;
            }
            break;
            
        case UI_STATE_BRIGHTNESS:
            if (key == KEY_1) {
                if (ui->brightness < 10) {
                    ui->brightness++;
                    set_brightness(ui->brightness);
                    ui_save_config(ui);
                }
            } else if (key == KEY_2) {
                if (ui->brightness > 0) {
                    ui->brightness--;
                    set_brightness(ui->brightness);
                    ui_save_config(ui);
                }
            } else if (key == KEY_3 || key == KEY_4) {
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 0;
            }
            break;
            
        case UI_STATE_SWITCH_INTERVAL:
            if (key == KEY_1) {
                if (ui->hover_index > 0) ui->hover_index--;
            } else if (key == KEY_2) {
                if (ui->hover_index < 3) ui->hover_index++;
            } else if (key == KEY_3) {
                ui->switch_interval = ui->hover_index;
                set_switch_interval(ui->switch_interval);
                ui_save_config(ui);
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 1;
            } else if (key == KEY_4) {
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 1;
            }
            break;
            
        case UI_STATE_SWITCH_MODE:
            if (key == KEY_1) {
                if (ui->hover_index > 0) ui->hover_index--;
            } else if (key == KEY_2) {
                if (ui->hover_index < 2) ui->hover_index++;
            } else if (key == KEY_3) {
                ui->switch_mode = ui->hover_index;
                set_switch_mode(ui->switch_mode);
                ui_save_config(ui);
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 2;
            } else if (key == KEY_4) {
                ui->state = UI_STATE_MAINMENU;
                ui->hover_index = 2;
            }
            break;
        case UI_STATE_DISPLAY_PIC: 
            ui->state = UI_STATE_NONE;
            break;
        default:
            break;
    }
}

static void ui_fire_transition_middle_cb(ui_t *ui){
    for(int i = 0; i < UI_TRANSITION_MIDDLE_CB_COUNT; i++){
        if(ui->transition_middle_cb[i] != NULL){
            ui->transition_middle_cb[i]();
            ui->transition_middle_cb[i] = NULL;
        }
    }
}

static void ui_transition_tick(ui_t *ui){
    if(ui->transition_state == TRANSITION_FILL_LEFT_RIGHT){
        for(int i = 0; i < UI_WIDTH; i++){
            fbdraw_fill_rect(ui->drawer, i, 0, 1, UI_HEIGHT, ui->transition_bg_color);
            usleep(UI_TRANSITION_LINE_SLEEP);
        }
        ui_fire_transition_middle_cb(ui);
        usleep(UI_TRANSITION_HOLD_TIME);
        for(int i = 0; i < UI_WIDTH; i++){
            fbdraw_fill_rect(ui->drawer, i, 0, 1, UI_HEIGHT, 0x00000000);
            usleep(UI_TRANSITION_LINE_SLEEP);
        }
        ui->transition_state = TRANSITION_NONE;
    }
    else if(ui->transition_state == TRANSITION_FILL_TOP_BOTTOM){
        int center_x = (UI_WIDTH - UI_TRANSITION_LOGO_WIDTH) / 2;
        int center_y = (UI_HEIGHT - UI_TRANSITION_LOGO_HEIGHT) / 2;
        for(int i = 0; i < UI_HEIGHT; i++){
            fbdraw_fill_rect(ui->drawer, 0, i, UI_WIDTH, 1, ui->transition_bg_color);
            if (i >= center_y && i < center_y + 256){
                fbdraw_argb_bitmap_region_from_file(
                    ui->drawer, 
                    center_x, i, 
                    UI_TRANSITION_LOGO_WIDTH, UI_TRANSITION_LOGO_HEIGHT, 
                    0, i - center_y, 
                    UI_TRANSITION_LOGO_WIDTH, 1, 
                    ui->transition_bitmap_path
                );
            }
            usleep(UI_TRANSITION_ROW_SLEEP);
        }
        ui_fire_transition_middle_cb(ui);
        usleep(UI_TRANSITION_LOGO_HOLD_TIME);
        for(int i = 0; i < UI_HEIGHT; i++){
            fbdraw_fill_rect(ui->drawer, 0, i, UI_WIDTH, 1, 0x00000000);
            usleep(UI_TRANSITION_ROW_SLEEP);
        }
    }
    else if(ui->transition_state == TRANSITION_OPERATOR_INFO){
        fbdraw_argb_bitmap_from_file_with_delay(
            ui->drawer, 0, 0, 
            UI_OPERATOR_INFO_WIDTH, UI_OPERATOR_INFO_HEIGHT, 
            ui->operator_info_path, 
            UI_TRANSITION_OPERATOR_INFO_SLEEP);
    }
    ui->transition_state = TRANSITION_NONE;
}

static void draw_battery_level(ui_t *ui){
    int fd;
    char buf[32];
    fd = open(UI_BATTERY_ADC_PATH, O_RDONLY);
    if (fd < 0) {
        log_error("failed to open battery adc file");
        return;
    }
    read(fd, buf, sizeof(buf));
    close(fd);

    int battery_level_width = 200;
    int battery_level_height = 20;
    int battery_level_x = 10;
    int battery_level_y = UI_HEIGHT - 120;

    int battery_val = atoi(buf);
    if(battery_val > UI_BATTERY_ADC_CHARGING_VALUE){
        CRREFont_printStr(ui->font, battery_level_x, battery_level_y, "Battery: Charging...");
        return;
    }
    int battery_level = (battery_val - UI_BATTERY_ADC_EMPTY_VALUE) * 100 / (UI_BATTERY_ADC_FULL_VALUE - UI_BATTERY_ADC_EMPTY_VALUE);
    if(battery_level > 100) battery_level = 100;
    if(battery_level < 0) battery_level = 0;
    int battery_bar_color = 0xFF00FF00;
    if (battery_level < 20) 
        battery_bar_color = 0xFFFF0000;
    else if(battery_level < 30)
        battery_bar_color = 0xFFFFFF00;
    else
        battery_bar_color = 0xFF00FF00;

    // round to nearest 10
    battery_level = (battery_level + 5) / 10;
    battery_level = battery_level * 10;

    CRREFont_printf(ui->font, battery_level_x, battery_level_y, "Battery:%d", battery_level);
    fbdraw_fill_rect(ui->drawer, battery_level_x + 130, battery_level_y, battery_level_width, battery_level_height, 0xFF404040);
    fbdraw_fill_rect(ui->drawer, battery_level_x + 130, battery_level_y, (battery_level_width * battery_level) / 100, battery_level_height, battery_bar_color);
}

void ui_start_transition(ui_t *ui,transition_state_t state){
    ui->transition_state = state;
    ui->transition_start_time = get_now_us();
}


void ui_process_input(ui_t *ui){
    struct input_event ev;
    int rc;
    
    while ((rc = libevdev_next_event(ui->evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
        if (ev.type == EV_KEY && ev.value == 1) {
            log_info("key pressed:%d", ev.code);
            ui_handle_key(ui, ev.code);
        }
    }
}

void ui_init(ui_t *ui, int width, int height, uint32_t *vaddr, drm_warpper_t *drm_warpper){

    ui->evdev_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (ui->evdev_fd < 0) {
        log_error("failed to open event device");
        return;
    }
    
    int ret = libevdev_new_from_fd(ui->evdev_fd, &ui->evdev);
    if (ret < 0) {
        log_error("failed to create libevdev");
        close(ui->evdev_fd);
        return;
    }

    log_info("event device opened: %s",libevdev_get_name(ui->evdev));


    g_fbdraw.fb_height = height;
    g_fbdraw.fb_width = width;
    g_fbdraw.vaddr = (uint32_t*) vaddr;

    CRREFont *font = CRREFont_new();
    CRREFont_init(font,rrefont_rect_cb, width, height);
    CRREFont_setFont(font, &rre_chicago_20x24);

    CRREFont_setFg(font, UI_TEXT_COLOR);
    CRREFont_setBg(font, UI_BACKGROUND_COLOR);

    ui->state = UI_STATE_NONE;
    ui->hover_index = 0;
    ui->font = font;
    ui->drawer = &g_fbdraw;
    ui->brightness = 5;
    ui->switch_interval = SW_INTERVAL_5MIN;
    ui->switch_mode = SW_MODE_SEQUENCE;
    ui->state_updated = 0;
    ui->transition_state = TRANSITION_NONE;
    ui->transition_start_time = 0;
    ui->drm_warpper = drm_warpper;

    ui_load_config(ui);
}

void ui_tick(ui_t *ui){
    if(ui->transition_state != TRANSITION_NONE){
        ui_transition_tick(ui);
        return;
    }

    if(!ui->state_updated){
        return;
    }
    ui->state_updated = 0;

    if (ui->state == UI_STATE_NONE) {
        fbdraw_fill_rect(ui->drawer, 0, 0, g_fbdraw.fb_width, g_fbdraw.fb_height, 0x00000000);
        if(strlen(ui->operator_info_path) > 0){
        fbdraw_argb_bitmap_from_file(
            ui->drawer, 0, 0, 
            UI_OPERATOR_INFO_WIDTH, UI_OPERATOR_INFO_HEIGHT, 
            ui->operator_info_path);
        }
        return;
    }

    if (ui->state != UI_STATE_DISPLAY_PIC) {
        fbdraw_fill_rect(ui->drawer, 0, 0, g_fbdraw.fb_width, g_fbdraw.fb_height, UI_BACKGROUND_COLOR);
        fbdraw_draw_bitmap_1_bit(ui->drawer, 10, 0, gImage_prts_logo, 340, 120,UI_BACKGROUND_COLOR,UI_TEXT_COLOR);
        CRREFont_printStr(ui->font, 10, UI_HEIGHT - 90, "PRTS Terminal Application");
        CRREFont_printStr(ui->font, 10, UI_HEIGHT - 60, EPASS_GIT_VERSION);
        CRREFont_printStr(ui->font, 10, UI_HEIGHT - 30, "(c) 1097 Rhodes Island.");
        draw_battery_level(ui);
    }

    switch (ui->state) {
        case UI_STATE_MAINMENU:
            ui_draw_mainmenu(ui);
            break;
        case UI_STATE_BRIGHTNESS:
            ui_draw_brightness(ui);
            break;
        case UI_STATE_SWITCH_INTERVAL:
            ui_draw_switch_interval(ui);
            break;
        case UI_STATE_SWITCH_MODE:
            ui_draw_switch_mode(ui);
            break;
        case UI_STATE_DISPLAY_PIC:
            ui_draw_display_pic(ui);
            break;
        case UI_STATE_NONE:
            break;
        default:
            break;
    }

}

void ui_set_transition_bitmap_path(ui_t *ui,char *path){
    strncpy(ui->transition_bitmap_path, path, sizeof(ui->transition_bitmap_path));
}

void ui_set_operator_info_path(ui_t *ui,char *path){
    strncpy(ui->operator_info_path, path, sizeof(ui->operator_info_path));
}
void ui_add_transition_middle_cb(ui_t *ui,void (*cb)(void)){
    for(int i = 0; i < UI_TRANSITION_MIDDLE_CB_COUNT; i++){
        if(ui->transition_middle_cb[i] == NULL){
            ui->transition_middle_cb[i] = cb;
            return;
        }
    }
    log_error("transition middle cb count exceeded");
}

void ui_set_transition_bg_color(ui_t *ui,uint32_t color){
    ui->transition_bg_color = color;
}