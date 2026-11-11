#ifndef GLASS_MUSIC_MPRIS_H
#define GLASS_MUSIC_MPRIS_H

#include <glib.h>
#include <gio/gio.h>

typedef struct {
    char *player_bus_name;
    char *title;
    char *artist;
    char *album;
    char *art_url;
    char *cached_art_path;
    gboolean is_playing;
    gint64 length_us;
} MprisTrackInfo;

typedef void (*MprisTrackCallback)(const MprisTrackInfo *track, void *user_data);

typedef struct _MprisClient MprisClient;

MprisClient *mpris_client_new(MprisTrackCallback callback, void *user_data);
void mpris_client_free(MprisClient *client);

void mpris_client_play_pause(MprisClient *client);
void mpris_client_next(MprisClient *client);
void mpris_client_previous(MprisClient *client);
void mpris_client_adjust_volume(MprisClient *client, double delta);

const MprisTrackInfo *mpris_client_get_current_track(MprisClient *client);
void mpris_client_set_mock_track(MprisClient *client, const char *title, const char *artist, const char *art_path);

#endif /* GLASS_MUSIC_MPRIS_H */

