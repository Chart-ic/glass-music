#ifndef GLASS_MUSIC_CONFIG_H
#define GLASS_MUSIC_CONFIG_H

#include <glib.h>

typedef struct {
    char *title_font;
    char *artist_font;
    char *not_playing_font;
    int margin_top;
    int autohide_delay_sec;
    gboolean autohide_not_playing;
    gboolean always_show;
    int max_text_width;
    double marquee_speed;
    char *default_art;
} AppConfig;

AppConfig *config_load(void);
void config_save(const AppConfig *cfg);
void config_free(AppConfig *cfg);

#endif /* GLASS_MUSIC_CONFIG_H */

