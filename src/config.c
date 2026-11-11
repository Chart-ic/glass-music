#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

static char *get_config_path(void) {
    const char *user_cfg = g_get_user_config_dir();
    char *dir = g_build_filename(user_cfg, "glass_music", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "config.ini", NULL);
    g_free(dir);
    return path;
}

AppConfig *config_load(void) {
    AppConfig *cfg = g_new0(AppConfig, 1);

    // Defaults
    cfg->title_font = g_strdup("Pretendard, Noto Sans CJK KR, Noto Sans CJK JP, Noto Sans, Sans-Serif Bold 12.5");
    cfg->artist_font = g_strdup("Pretendard, Inter, Noto Sans, Sans-Serif Bold 10");
    cfg->not_playing_font = g_strdup("Pretendard, Noto Sans CJK KR, Noto Sans, Sans-Serif Bold 13");
    cfg->margin_top = 16;
    cfg->autohide_delay_sec = 5;
    cfg->autohide_not_playing = TRUE;
    cfg->always_show = FALSE;
    cfg->max_text_width = 340;
    cfg->marquee_speed = 26.0; // pixels per second (smooth, natural reading speed)
    cfg->default_art = NULL; // No default test art in production build!

    char *path = get_config_path();
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        char *str = g_key_file_get_string(kf, "fonts", "title", NULL);
        if (str && strlen(str) > 0) {
            g_free(cfg->title_font);
            cfg->title_font = str;
        }

        str = g_key_file_get_string(kf, "fonts", "artist", NULL);
        if (str && strlen(str) > 0) {
            g_free(cfg->artist_font);
            cfg->artist_font = str;
        }

        str = g_key_file_get_string(kf, "fonts", "not_playing", NULL);
        if (str && strlen(str) > 0) {
            g_free(cfg->not_playing_font);
            cfg->not_playing_font = str;
        }

        if (g_key_file_has_key(kf, "display", "margin_top", NULL)) {
            cfg->margin_top = g_key_file_get_integer(kf, "display", "margin_top", NULL);
        }

        if (g_key_file_has_key(kf, "display", "autohide_delay_sec", NULL)) {
            cfg->autohide_delay_sec = g_key_file_get_integer(kf, "display", "autohide_delay_sec", NULL);
        }

        if (g_key_file_has_key(kf, "display", "autohide_not_playing", NULL)) {
            cfg->autohide_not_playing = g_key_file_get_boolean(kf, "display", "autohide_not_playing", NULL);
            cfg->always_show = !cfg->autohide_not_playing;
        } else if (g_key_file_has_key(kf, "display", "always_show", NULL)) {
            cfg->always_show = g_key_file_get_boolean(kf, "display", "always_show", NULL);
            cfg->autohide_not_playing = !cfg->always_show;
        }

        if (g_key_file_has_key(kf, "display", "max_text_width", NULL)) {
            cfg->max_text_width = g_key_file_get_integer(kf, "display", "max_text_width", NULL);
        }

        if (g_key_file_has_key(kf, "display", "marquee_speed", NULL)) {
            cfg->marquee_speed = g_key_file_get_double(kf, "display", "marquee_speed", NULL);
        }

        str = g_key_file_get_string(kf, "display", "default_art", NULL);
        if (str && strlen(str) > 0 && strstr(str, "default_art.png") == NULL) {
            g_free(cfg->default_art);
            cfg->default_art = str;
        } else {
            g_free(str);
        }
    } else {
        if (err) g_error_free(err);
        // Save initial default file
        config_save(cfg);
    }

    g_key_file_free(kf);
    g_free(path);
    return cfg;
}

void config_save(const AppConfig *cfg) {
    if (!cfg) return;

    GKeyFile *kf = g_key_file_new();
    char *path = get_config_path();

    g_key_file_set_string(kf, "fonts", "title", cfg->title_font);
    g_key_file_set_string(kf, "fonts", "artist", cfg->artist_font);
    g_key_file_set_string(kf, "fonts", "not_playing", cfg->not_playing_font);

    g_key_file_set_integer(kf, "display", "margin_top", cfg->margin_top);
    g_key_file_set_integer(kf, "display", "autohide_delay_sec", cfg->autohide_delay_sec);
    g_key_file_set_boolean(kf, "display", "autohide_not_playing", cfg->autohide_not_playing);
    g_key_file_set_boolean(kf, "display", "always_show", !cfg->autohide_not_playing);
    g_key_file_set_integer(kf, "display", "max_text_width", cfg->max_text_width);
    g_key_file_set_double(kf, "display", "marquee_speed", cfg->marquee_speed);
    if (cfg->default_art) {
        g_key_file_set_string(kf, "display", "default_art", cfg->default_art);
    }

    GError *err = NULL;
    g_key_file_save_to_file(kf, path, &err);
    if (err) g_error_free(err);

    g_key_file_free(kf);
    g_free(path);
}

void config_free(AppConfig *cfg) {
    if (!cfg) return;
    g_free(cfg->title_font);
    g_free(cfg->artist_font);
    g_free(cfg->not_playing_font);
    g_free(cfg->default_art);
    g_free(cfg);
}
