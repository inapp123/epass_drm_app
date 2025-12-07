#include "drm_warpper.h"
#include "mediaplayer.h"
#include "log.h"
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include "config.h"
#include "ui.h"
#include "prts.h"


static drm_warpper_t g_drm_warpper;
static mediaplayer_t g_mediaplayer;
static fbdraw_t g_fbdraw;
static ui_t g_ui;
static prts_t g_prts;

static int g_running = 1;
void signal_handler(int sig)
{
    log_info("received signal %d, shutting down", sig);
    g_running = 0;
}

void set_brightness(int brightness){
    FILE *f = fopen("/sys/class/backlight/backlight/brightness", "w");
    if (f) {
        fprintf(f, "%d\n", brightness);
        fclose(f);
    } else {
        log_error("Failed to set brightness");
    }
}


void next_video(){
    prts_next_operator(&g_prts);
}

void prev_video(){
    prts_prev_operator(&g_prts);
}

void set_switch_interval(sw_interval_t interval){
    g_prts.sw_interval = interval;
}

void set_switch_mode(sw_mode_t mode){
    g_prts.sw_mode = mode;
}

void enter_usb_download(){
    g_running = 0;
}

void init_transition_middle_cb(){
    drm_warpper_set_layer_position(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0);
}

void prts_transition_play_loop_middle_cb(){
    char pathbuf[128];
    snprintf(pathbuf, 128, "%s/%s", g_prts.operator_entries[g_prts.current_operator_index]->path, PRTS_LOOP_VIDEO_FILENAME);
    mediaplayer_stop(&g_mediaplayer);
    mediaplayer_set_video(&g_mediaplayer, pathbuf);
    mediaplayer_start(&g_mediaplayer);
}

void prts_transition_play_intro_middle_cb(){
    char pathbuf[128];
    snprintf(pathbuf, 128, "%s/%s", g_prts.operator_entries[g_prts.current_operator_index]->path, PRTS_INTRO_VIDEO_FILENAME);
    mediaplayer_stop(&g_mediaplayer);
    mediaplayer_set_video(&g_mediaplayer, pathbuf);
    mediaplayer_start(&g_mediaplayer);
}

bool ui_blocked(){
    return g_prts.status != PRTS_STATUS_IDLE;
}

int main(int argc, char *argv[]){
    if(argc == 2){
        if(strcmp(argv[1], "version") == 0){
            log_info("EPASS_GIT_VERSION: %s", EPASS_GIT_VERSION);
            return 0;
        }
    }
    log_info("starting up prts terminal application: %s", EPASS_GIT_VERSION);
        /* setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    prts_init(&g_prts, &g_ui, &g_mediaplayer);
    prts_scan_assets(&g_prts);
    if(g_prts.operator_entries_count == 0){
        log_error("no operator assets found");
        printf(" _   _ _____  ______  ___ _____ ___  \n");
        printf("| \\ | |  _  | |  _  \\/ _ \\_   _/ _ \\ \n");
        printf("|  \\| | | | | | | | / /_\\ \\| |/ /_\\ \n");
        printf("| . ` | | | | | | | |  _  || ||  _  |\n");
        printf("| |\\  \\ \\_/ / | |/ /| | | || || | | |\n");
        printf("\\_| \\_/\\___/  |___/ \\_| |_/\\_/\\_| |_/\n");
        printf("No operator assets found. Please check the assets directory.\n");
        return -1;
    }

    /* initialize DRM */
    if (drm_warpper_init(&g_drm_warpper) != 0) {
        log_error("failed to initialize DRM warpper");
        return -1;
    }

    drm_warpper_init_layer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, VIDEO_WIDTH, VIDEO_HEIGHT, DRM_WARPPER_LAYER_MODE_MB32_NV12);
    uint8_t *video_vaddr;
    drm_warpper_get_layer_buffer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, &video_vaddr);
    // drm_warpper_set_layer_position(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, 4, 0);

    /* initialize mediaplayer */
    if (mediaplayer_init(&g_mediaplayer) != 0) {
        log_error("failed to initialize mediaplayer");
        drm_warpper_destroy(&g_drm_warpper);
        return -1;
    }

    
    drm_warpper_init_layer(&g_drm_warpper, DRM_WARPPER_LAYER_UI, UI_WIDTH, UI_HEIGHT, DRM_WARPPER_LAYER_MODE_ARGB8888);
    drm_warpper_set_layer_position(&g_drm_warpper, DRM_WARPPER_LAYER_UI, 24, 0);
    uint8_t *ui_addr;
    drm_warpper_get_layer_buffer(&g_drm_warpper, DRM_WARPPER_LAYER_UI, &ui_addr);

    // mediaplayer_play_video(&g_mediaplayer, "/root/out.mp4",video_vaddr);
    mediaplayer_set_output_buffer(&g_mediaplayer, video_vaddr);
    ui_init(&g_ui, UI_WIDTH, UI_HEIGHT, (uint32_t*)ui_addr, &g_drm_warpper);
    ui_add_transition_middle_cb(&g_ui, init_transition_middle_cb);

    prts_next_operator(&g_prts);
    
    /* keep main thread running */
    while (g_running) {
        prts_tick(&g_prts);
        ui_process_input(&g_ui);
        ui_tick(&g_ui);
        usleep(50000);
    }

    /* cleanup */
    log_info("shutting down");
    mediaplayer_stop(&g_mediaplayer);
    mediaplayer_destroy(&g_mediaplayer);
    drm_warpper_destroy(&g_drm_warpper);
    
    return 0;

}