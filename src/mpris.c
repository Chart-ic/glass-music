#include "mpris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

typedef struct {
    char *bus_name;
    char *owner;
    guint prop_sub_id;
    MprisTrackInfo track;
} PlayerInstance;

struct _MprisClient {
    GDBusConnection *conn;
    guint name_owner_sub_id;
    GList *players; // List of PlayerInstance*
    PlayerInstance *active_player;
    MprisTrackCallback callback;
    void *user_data;
    char *cache_dir;
    MprisTrackInfo current_info;
    gboolean is_mock;
};

static void player_instance_free(PlayerInstance *p) {
    if (!p) return;
    g_free(p->bus_name);
    g_free(p->owner);
    g_free(p->track.player_bus_name);
    g_free(p->track.title);
    g_free(p->track.artist);
    g_free(p->track.album);
    g_free(p->track.art_url);
    g_free(p->track.cached_art_path);
    g_free(p);
}

static void update_active_player(MprisClient *client);
static void query_player_properties(MprisClient *client, PlayerInstance *player);

static char *get_cache_dir(void) {
    const char *user_cache = g_get_user_cache_dir();
    char *dir = g_build_filename(user_cache, "glass_music", NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

static void on_art_downloaded(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GFile *file = G_FILE(source_object);
    MprisClient *client = (MprisClient *)user_data;
    char *contents = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (g_file_load_contents_finish(file, res, &contents, &length, NULL, &error)) {
        char *uri = g_file_get_uri(file);
        char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, uri, -1);
        char *cache_path = g_build_filename(client->cache_dir, hash, NULL);
        char *cache_path_ext = g_strdup_printf("%s.img", cache_path);

        GError *write_err = NULL;
        if (g_file_set_contents(cache_path_ext, contents, (gssize)length, &write_err)) {
            // Update active player if matching
            if (client->active_player && client->active_player->track.art_url &&
                strcmp(client->active_player->track.art_url, uri) == 0) {
                g_free(client->active_player->track.cached_art_path);
                client->active_player->track.cached_art_path = g_strdup(cache_path_ext);
                update_active_player(client);
            }
        }
        g_free(cache_path_ext);
        g_free(cache_path);
        g_free(hash);
        g_free(uri);
        g_free(contents);
    }
}

static char *resolve_art_url(MprisClient *client, const char *art_url) {
    if (!art_url || strlen(art_url) == 0) {
        return NULL;
    }

    if (g_str_has_prefix(art_url, "file://")) {
        GError *err = NULL;
        char *path = g_filename_from_uri(art_url, NULL, &err);
        if (path) return path;
    }

    if (g_str_has_prefix(art_url, "http://") || g_str_has_prefix(art_url, "https://")) {
        char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, art_url, -1);
        char *cache_path = g_strdup_printf("%s/%s.img", client->cache_dir, hash);
        g_free(hash);

        if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
            return cache_path;
        }

        // Asynchronously download
        GFile *remote = g_file_new_for_uri(art_url);
        g_file_load_contents_async(remote, NULL, on_art_downloaded, client);
        g_object_unref(remote);

        g_free(cache_path);
        return NULL;
    }

    if (g_str_has_prefix(art_url, "data:image/")) {
        const char *comma = strchr(art_url, ',');
        if (comma) {
            comma++;
            gsize out_len = 0;
            guchar *data = g_base64_decode(comma, &out_len);
            if (data && out_len > 0) {
                char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, out_len);
                char *cache_path = g_strdup_printf("%s/%s.img", client->cache_dir, hash);
                g_free(hash);
                g_file_set_contents(cache_path, (const char *)data, (gssize)out_len, NULL);
                g_free(data);
                return cache_path;
            }
        }
    }

    if (g_file_test(art_url, G_FILE_TEST_EXISTS)) {
        return g_strdup(art_url);
    }

    return NULL;
}

static void parse_metadata_dict(MprisClient *client, GVariant *metadata, MprisTrackInfo *track) {
    g_free(track->title);
    g_free(track->artist);
    g_free(track->album);
    g_free(track->art_url);
    g_free(track->cached_art_path);

    track->title = NULL;
    track->artist = NULL;
    track->album = NULL;
    track->art_url = NULL;
    track->cached_art_path = NULL;
    track->length_us = 0;

    if (!metadata) return;

    GVariantIter iter;
    g_variant_iter_init(&iter, metadata);
    gchar *key = NULL;
    GVariant *val = NULL;

    while (g_variant_iter_loop(&iter, "{sv}", &key, &val)) {
        if (strcmp(key, "xesam:title") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                track->title = g_strdup(g_variant_get_string(val, NULL));
            }
        } else if (strcmp(key, "xesam:artist") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING_ARRAY)) {
                gsize len = 0;
                const gchar **arr = g_variant_get_strv(val, &len);
                if (arr && len > 0) {
                    track->artist = g_strjoinv(", ", (gchar **)arr);
                    g_free(arr);
                }
            } else if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                track->artist = g_strdup(g_variant_get_string(val, NULL));
            }
        } else if (strcmp(key, "xesam:album") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                track->album = g_strdup(g_variant_get_string(val, NULL));
            }
        } else if (strcmp(key, "mpris:artUrl") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                track->art_url = g_strdup(g_variant_get_string(val, NULL));
            }
        } else if (strcmp(key, "mpris:length") == 0) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_INT64)) {
                track->length_us = g_variant_get_int64(val);
            }
        }
    }

    if (track->art_url) {
        track->cached_art_path = resolve_art_url(client, track->art_url);
    }
}

static void update_active_player(MprisClient *client) {
    if (client->is_mock) return;

    PlayerInstance *best = NULL;
    for (GList *l = client->players; l != NULL; l = l->next) {
        PlayerInstance *p = (PlayerInstance *)l->data;
        if (p->track.is_playing) {
            best = p;
            break;
        }
    }

    if (!best && client->players) {
        best = (PlayerInstance *)client->players->data;
    }

    client->active_player = best;

    // Deep copy to current_info
    g_free(client->current_info.player_bus_name);
    g_free(client->current_info.title);
    g_free(client->current_info.artist);
    g_free(client->current_info.album);
    g_free(client->current_info.art_url);
    g_free(client->current_info.cached_art_path);

    if (best) {
        client->current_info.player_bus_name = g_strdup(best->bus_name);
        client->current_info.title = g_strdup(best->track.title ? best->track.title : "Unknown Title");
        client->current_info.artist = g_strdup(best->track.artist ? best->track.artist : "Unknown Artist");
        client->current_info.album = g_strdup(best->track.album);
        client->current_info.art_url = g_strdup(best->track.art_url);
        client->current_info.cached_art_path = g_strdup(best->track.cached_art_path);
        client->current_info.is_playing = best->track.is_playing;
        client->current_info.length_us = best->track.length_us;
    } else {
        memset(&client->current_info, 0, sizeof(MprisTrackInfo));
    }

    if (client->callback) {
        client->callback(&client->current_info, client->user_data);
    }
}

static void on_properties_changed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters,
                                  gpointer user_data) {
    (void)conn; (void)object_path; (void)interface_name; (void)signal_name;
    MprisClient *client = (MprisClient *)user_data;

    // Find matching player
    PlayerInstance *player = NULL;
    for (GList *l = client->players; l != NULL; l = l->next) {
        PlayerInstance *p = (PlayerInstance *)l->data;
        if (p->owner && sender_name && strcmp(p->owner, sender_name) == 0) {
            player = p;
            break;
        }
        if (p->bus_name && sender_name && strcmp(p->bus_name, sender_name) == 0) {
            player = p;
            break;
        }
    }

    if (!player) return;

    const gchar *target_iface = NULL;
    GVariant *changed_props = NULL;
    g_variant_get(parameters, "(&s@a{sv}^as)", &target_iface, &changed_props, NULL);

    if (changed_props) {
        GVariantIter iter;
        g_variant_iter_init(&iter, changed_props);
        gchar *key = NULL;
        GVariant *val = NULL;

        while (g_variant_iter_loop(&iter, "{sv}", &key, &val)) {
            if (strcmp(key, "PlaybackStatus") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
                const char *status = g_variant_get_string(val, NULL);
                player->track.is_playing = (strcmp(status, "Playing") == 0);
            } else if (strcmp(key, "Metadata") == 0 && g_variant_is_of_type(val, G_VARIANT_TYPE_VARDICT)) {
                parse_metadata_dict(client, val, &player->track);
            }
        }
        g_variant_unref(changed_props);
    }

    update_active_player(client);
}


static void query_player_properties(MprisClient *client, PlayerInstance *player) {
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        client->conn,
        player->bus_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        150,
        NULL,
        &error
    );

    if (result) {
        GVariant *dict = NULL;
        g_variant_get(result, "(@a{sv})", &dict);
        if (dict) {
            GVariant *status_var = g_variant_lookup_value(dict, "PlaybackStatus", G_VARIANT_TYPE_STRING);
            if (status_var) {
                const char *st = g_variant_get_string(status_var, NULL);
                player->track.is_playing = (strcmp(st, "Playing") == 0);
                g_variant_unref(status_var);
            }

            GVariant *meta_var = g_variant_lookup_value(dict, "Metadata", G_VARIANT_TYPE_VARDICT);
            if (meta_var) {
                parse_metadata_dict(client, meta_var, &player->track);
                g_variant_unref(meta_var);
            }
            g_variant_unref(dict);
        }
        g_variant_unref(result);
    } else {
        if (error) g_error_free(error);
    }
}

static void add_player(MprisClient *client, const char *bus_name) {
    for (GList *l = client->players; l != NULL; l = l->next) {
        PlayerInstance *p = (PlayerInstance *)l->data;
        if (strcmp(p->bus_name, bus_name) == 0) return;
    }

    PlayerInstance *player = g_new0(PlayerInstance, 1);
    player->bus_name = g_strdup(bus_name);
    player->track.player_bus_name = g_strdup(bus_name);

    // Get owner
    GError *err = NULL;
    GVariant *owner_res = g_dbus_connection_call_sync(
        client->conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner",
        g_variant_new("(s)", bus_name),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        150,
        NULL,
        &err
    );
    if (owner_res) {
        const char *owner = NULL;
        g_variant_get(owner_res, "(&s)", &owner);
        player->owner = g_strdup(owner);
        g_variant_unref(owner_res);
    } else {
        if (err) g_error_free(err);
    }

    // Subscribe to PropertiesChanged
    player->prop_sub_id = g_dbus_connection_signal_subscribe(
        client->conn,
        player->bus_name,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed,
        client,
        NULL
    );

    query_player_properties(client, player);
    client->players = g_list_append(client->players, player);
    update_active_player(client);
}

static void remove_player(MprisClient *client, const char *name) {
    PlayerInstance *to_remove = NULL;
    for (GList *l = client->players; l != NULL; l = l->next) {
        PlayerInstance *p = (PlayerInstance *)l->data;
        if ((p->bus_name && strcmp(p->bus_name, name) == 0) ||
            (p->owner && strcmp(p->owner, name) == 0)) {
            to_remove = p;
            break;
        }
    }

    if (to_remove) {
        if (to_remove->prop_sub_id > 0) {
            g_dbus_connection_signal_unsubscribe(client->conn, to_remove->prop_sub_id);
        }
        client->players = g_list_remove(client->players, to_remove);
        if (client->active_player == to_remove) {
            client->active_player = NULL;
        }
        player_instance_free(to_remove);
        update_active_player(client);
    }
}

static void on_name_owner_changed(GDBusConnection *conn,
                                 const gchar *sender_name,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *signal_name,
                                 GVariant *parameters,
                                 gpointer user_data) {
    (void)conn; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;
    MprisClient *client = (MprisClient *)user_data;

    const gchar *name = NULL;
    const gchar *old_owner = NULL;
    const gchar *new_owner = NULL;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);

    if (!g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
        return;
    }

    if (new_owner && strlen(new_owner) > 0) {
        add_player(client, name);
    } else if (old_owner && strlen(old_owner) > 0) {
        remove_player(client, name);
    }
}

static void discover_initial_players(MprisClient *client) {
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        client->conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (!result) {
        if (error) g_error_free(error);
        return;
    }

    GVariantIter *iter = NULL;
    g_variant_get(result, "(as)", &iter);
    gchar *name = NULL;

    while (g_variant_iter_loop(iter, "s", &name)) {
        if (g_str_has_prefix(name, "org.mpris.MediaPlayer2.")) {
            add_player(client, name);
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(result);
}

MprisClient *mpris_client_new(MprisTrackCallback callback, void *user_data) {
    GError *error = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!conn) {
        g_printerr("MPRIS: Failed to connect to session bus: %s\n", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return NULL;
    }

    MprisClient *client = g_new0(MprisClient, 1);
    client->conn = conn;
    client->callback = callback;
    client->user_data = user_data;
    client->cache_dir = get_cache_dir();

    client->name_owner_sub_id = g_dbus_connection_signal_subscribe(
        client->conn,
        "org.freedesktop.DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        "/org/freedesktop/DBus",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_name_owner_changed,
        client,
        NULL
    );

    discover_initial_players(client);
    return client;
}

void mpris_client_free(MprisClient *client) {
    if (!client) return;

    if (client->name_owner_sub_id > 0) {
        g_dbus_connection_signal_unsubscribe(client->conn, client->name_owner_sub_id);
    }

    for (GList *l = client->players; l != NULL; l = l->next) {
        PlayerInstance *p = (PlayerInstance *)l->data;
        if (p->prop_sub_id > 0) {
            g_dbus_connection_signal_unsubscribe(client->conn, p->prop_sub_id);
        }
        player_instance_free(p);
    }
    g_list_free(client->players);

    g_free(client->current_info.player_bus_name);
    g_free(client->current_info.title);
    g_free(client->current_info.artist);
    g_free(client->current_info.album);
    g_free(client->current_info.art_url);
    g_free(client->current_info.cached_art_path);

    if (client->conn) {
        g_object_unref(client->conn);
    }
    g_free(client->cache_dir);
    g_free(client);
}

void mpris_client_play_pause(MprisClient *client) {
    if (!client || !client->active_player || !client->active_player->bus_name) return;

    g_dbus_connection_call(
        client->conn,
        client->active_player->bus_name,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "PlayPause",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL
    );
}

void mpris_client_next(MprisClient *client) {
    if (!client || !client->active_player || !client->active_player->bus_name) return;

    g_dbus_connection_call(
        client->conn,
        client->active_player->bus_name,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "Next",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL
    );
}

void mpris_client_previous(MprisClient *client) {
    if (!client || !client->active_player || !client->active_player->bus_name) return;

    g_dbus_connection_call(
        client->conn,
        client->active_player->bus_name,
        "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player",
        "Previous",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL,
        NULL
    );
}

void mpris_client_adjust_volume(MprisClient *client, double delta) {
    if (!client || !client->active_player || !client->active_player->bus_name) return;

    GError *err = NULL;
    GVariant *res = g_dbus_connection_call_sync(
        client->conn,
        client->active_player->bus_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Volume"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        500,
        NULL,
        &err
    );

    if (res) {
        GVariant *v = NULL;
        g_variant_get(res, "(v)", &v);
        double cur_vol = g_variant_get_double(v);
        g_variant_unref(v);
        g_variant_unref(res);

        double new_vol = cur_vol + delta;
        if (new_vol < 0.0) new_vol = 0.0;
        if (new_vol > 1.0) new_vol = 1.0;

        g_dbus_connection_call(
            client->conn,
            client->active_player->bus_name,
            "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties",
            "Set",
            g_variant_new("(ssv)", "org.mpris.MediaPlayer2.Player", "Volume", g_variant_new_double(new_vol)),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            NULL,
            NULL
        );
    } else {
        if (err) g_error_free(err);
    }
}

const MprisTrackInfo *mpris_client_get_current_track(MprisClient *client) {
    if (!client) return NULL;
    return &client->current_info;
}

void mpris_client_set_mock_track(MprisClient *client, const char *title, const char *artist, const char *art_path) {
    if (!client) return;
    client->is_mock = TRUE;

    g_free(client->current_info.player_bus_name);
    g_free(client->current_info.title);
    g_free(client->current_info.artist);
    g_free(client->current_info.album);
    g_free(client->current_info.art_url);
    g_free(client->current_info.cached_art_path);

    client->current_info.player_bus_name = g_strdup("org.mpris.MediaPlayer2.mock");
    client->current_info.title = g_strdup(title ? title : "キスキツネ");
    client->current_info.artist = g_strdup(artist ? artist : "HIMEHINA");
    client->current_info.album = g_strdup("HIMEHINA 3rd Album");
    client->current_info.art_url = g_strdup(art_path);
    client->current_info.cached_art_path = g_strdup(art_path);
    client->current_info.is_playing = TRUE;
    client->current_info.length_us = 210000000;

    if (client->callback) {
        client->callback(&client->current_info, client->user_data);
    }
}
