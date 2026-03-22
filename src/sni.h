#ifndef CLEVO_SNI_H
#define CLEVO_SNI_H

#include <glib.h>

typedef struct ClevoSni ClevoSni;

typedef struct
{
    void (*activate)(int x, int y, void *user_data);
    void (*context_menu)(int x, int y, void *user_data);
    void (*secondary_activate)(int x, int y, void *user_data);
} ClevoSniHandlers;

ClevoSni *clevo_sni_new(const ClevoSniHandlers *handlers, void *user_data);
void clevo_sni_free(ClevoSni *sni);

void clevo_sni_set_label(ClevoSni *sni, const char *label, const char *guide);
void clevo_sni_set_title(ClevoSni *sni, const char *title);
void clevo_sni_set_status(ClevoSni *sni, const char *status);
void clevo_sni_set_icon_name(ClevoSni *sni, const char *icon_name);
void clevo_sni_set_show_icon(ClevoSni *sni, gboolean show_icon);
void clevo_sni_set_prefer_activate(ClevoSni *sni, gboolean prefer_activate);

const char *clevo_sni_get_bus_name(ClevoSni *sni);
const char *clevo_sni_get_object_path(ClevoSni *sni);

#endif
