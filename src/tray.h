#ifndef GLASS_MUSIC_TRAY_H
#define GLASS_MUSIC_TRAY_H

#include <glib.h>
#include <gio/gio.h>
#include "config.h"

typedef struct _TrayService TrayService;

typedef void (*ConfigChangedCallback)(const AppConfig *new_config, void *user_data);

TrayService *tray_service_new(AppConfig *config, ConfigChangedCallback cb, void *user_data);
void tray_service_free(TrayService *tray);
void tray_service_open_font_dialog(TrayService *tray);

#endif /* GLASS_MUSIC_TRAY_H */

