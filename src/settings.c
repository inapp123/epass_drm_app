#include "settings.h"
#include "log.h"
#include "config.h"

extern void set_brightness(int brightness);
extern void set_switch_interval(sw_interval_t interval);
extern void set_switch_mode(sw_mode_t mode);

void settings_init(settings_t *settings){
    FILE *f = fopen(SETTINGS_FILE_PATH, "rb");
    if(f == NULL){
        log_error("failed to open settings file");
    }
    else{
        fread(settings, sizeof(settings_t), 1, f);
        if(settings->magic != SETTINGS_MAGIC){
            log_error("invalid settings file");
        }
        else if(settings->version != SETTINGS_VERSION){
            log_error("invalid settings file version");
        }
        else{
            fclose(f);
            return;
        }
        fclose(f);
    }

    log_info("creating new settings file");
    settings->magic = SETTINGS_MAGIC;
    settings->version = SETTINGS_VERSION;
    settings->brightness = 5;
    settings->switch_interval = SW_INTERVAL_5MIN;
    settings->switch_mode = SW_MODE_SEQUENCE;
    settings_save(settings);
    return;
    
}


void settings_save(settings_t *settings){
    FILE *f = fopen(SETTINGS_FILE_PATH, "wb");
    settings->magic = SETTINGS_MAGIC;
    settings->version = SETTINGS_VERSION;
    fwrite(settings, sizeof(settings_t), 1, f);
    fclose(f);
}

