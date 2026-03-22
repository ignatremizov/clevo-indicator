#include "sni.h"

#include <gio/gio.h>
#include <glib/gprintf.h>
#include <unistd.h>

#define CLEVO_SNI_OBJECT_PATH "/com/clevo/Indicator/StatusNotifierItem"
#define CLEVO_SNI_KDE_WATCHER "org.kde.StatusNotifierWatcher"
#define CLEVO_SNI_FDO_WATCHER "org.freedesktop.StatusNotifierWatcher"
#define CLEVO_SNI_WATCHER_PATH "/StatusNotifierWatcher"

struct ClevoSni
{
    GDBusConnection *connection;
    GDBusNodeInfo *node_info;
    guint registration_id;
    guint owner_id;
    guint kde_watcher_id;
    guint fdo_watcher_id;

    char *bus_name;
    char *title;
    char *status;
    char *label;
    char *label_guide;
    char *icon_name;
    char *activation_token;
    gboolean show_icon;
    gboolean prefer_activate;

    ClevoSniHandlers handlers;
    void *user_data;
};

static const char introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='WindowId' type='i' access='read'/>"
    "    <property name='IconThemePath' type='s' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='OverlayIconName' type='s' access='read'/>"
    "    <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "    <property name='XAyatanaLabel' type='s' access='read'/>"
    "    <property name='XAyatanaLabelGuide' type='s' access='read'/>"
    "    <property name='XClevoShowIcon' type='b' access='read'/>"
    "    <property name='XClevoPreferActivate' type='b' access='read'/>"
    "    <method name='ContextMenu'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='Activate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='ProvideXdgActivationToken'>"
    "      <arg name='token' type='s' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg name='x' type='i' direction='in'/>"
    "      <arg name='y' type='i' direction='in'/>"
    "    </method>"
    "    <method name='XAyatanaSecondaryActivate'>"
    "      <arg name='timestamp' type='u' direction='in'/>"
    "    </method>"
    "    <method name='Scroll'>"
    "      <arg name='delta' type='i' direction='in'/>"
    "      <arg name='orientation' type='s' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static GVariant *clevo_sni_empty_pixmaps(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
}

static GVariant *clevo_sni_get_property(GDBusConnection *connection,
                                        const char *sender,
                                        const char *object_path,
                                        const char *interface_name,
                                        const char *property_name,
                                        GError **error,
                                        gpointer user_data)
{
    ClevoSni *sni = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;

    if (g_strcmp0(property_name, "Category") == 0)
        return g_variant_new_string("Hardware");
    if (g_strcmp0(property_name, "Id") == 0)
        return g_variant_new_string("clevo-indicator");
    if (g_strcmp0(property_name, "Title") == 0)
        return g_variant_new_string(sni->title ? sni->title : "Clevo Indicator");
    if (g_strcmp0(property_name, "Status") == 0)
        return g_variant_new_string(sni->status ? sni->status : "Active");
    if (g_strcmp0(property_name, "WindowId") == 0)
        return g_variant_new_int32(0);
    if (g_strcmp0(property_name, "IconThemePath") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(property_name, "Menu") == 0)
        return g_variant_new_object_path("/NO_DBUSMENU");
    if (g_strcmp0(property_name, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(property_name, "IconName") == 0)
        return g_variant_new_string(sni->icon_name ? sni->icon_name : "");
    if (g_strcmp0(property_name, "IconPixmap") == 0)
        return clevo_sni_empty_pixmaps();
    if (g_strcmp0(property_name, "OverlayIconName") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(property_name, "OverlayIconPixmap") == 0)
        return clevo_sni_empty_pixmaps();
    if (g_strcmp0(property_name, "AttentionIconName") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(property_name, "AttentionIconPixmap") == 0)
        return clevo_sni_empty_pixmaps();
    if (g_strcmp0(property_name, "XAyatanaLabel") == 0)
        return g_variant_new_string(sni->label ? sni->label : "");
    if (g_strcmp0(property_name, "XAyatanaLabelGuide") == 0)
        return g_variant_new_string(sni->label_guide ? sni->label_guide : "");
    if (g_strcmp0(property_name, "XClevoShowIcon") == 0)
        return g_variant_new_boolean(sni->show_icon);
    if (g_strcmp0(property_name, "XClevoPreferActivate") == 0)
        return g_variant_new_boolean(sni->prefer_activate);

    return NULL;
}

static void clevo_sni_handle_method_call(GDBusConnection *connection,
                                         const char *sender,
                                         const char *object_path,
                                         const char *interface_name,
                                         const char *method_name,
                                         GVariant *parameters,
                                         GDBusMethodInvocation *invocation,
                                         gpointer user_data)
{
    ClevoSni *sni = user_data;
    int x = 0;
    int y = 0;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;

    if (g_strcmp0(method_name, "Activate") == 0)
    {
        g_variant_get(parameters, "(ii)", &x, &y);
        if (sni->handlers.activate)
            sni->handlers.activate(x, y, sni->user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "ContextMenu") == 0)
    {
        g_variant_get(parameters, "(ii)", &x, &y);
        if (sni->handlers.context_menu)
            sni->handlers.context_menu(x, y, sni->user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "SecondaryActivate") == 0)
    {
        g_variant_get(parameters, "(ii)", &x, &y);
        if (sni->handlers.secondary_activate)
            sni->handlers.secondary_activate(x, y, sni->user_data);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "ProvideXdgActivationToken") == 0)
    {
        const char *token = "";
        g_variant_get(parameters, "(&s)", &token);
        g_free(sni->activation_token);
        sni->activation_token = g_strdup(token);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "XAyatanaSecondaryActivate") == 0 ||
        g_strcmp0(method_name, "Scroll") == 0)
    {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation,
        "org.freedesktop.DBus.Error.UnknownMethod",
        "Unsupported StatusNotifierItem method");
}

static const GDBusInterfaceVTable clevo_sni_vtable = {
    .method_call = clevo_sni_handle_method_call,
    .get_property = clevo_sni_get_property,
    .set_property = NULL,
};

static void clevo_sni_emit_properties_changed(ClevoSni *sni,
                                              const char *name1,
                                              GVariant *value1,
                                              const char *name2,
                                              GVariant *value2)
{
    GVariantBuilder builder;

    if (!sni || !sni->connection)
        return;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    if (name1 && value1)
        g_variant_builder_add(&builder, "{sv}", name1, value1);
    if (name2 && value2)
        g_variant_builder_add(&builder, "{sv}", name2, value2);

    g_dbus_connection_emit_signal(
        sni->connection,
        NULL,
        CLEVO_SNI_OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new("(sa{sv}as)",
                      "org.kde.StatusNotifierItem",
                      &builder,
                      NULL),
        NULL);
}

static void clevo_sni_register_with_watcher(ClevoSni *sni, const char *watcher_name)
{
    GError *error = NULL;

    if (!sni || !sni->connection)
        return;

    g_dbus_connection_call_sync(sni->connection,
                                watcher_name,
                                CLEVO_SNI_WATCHER_PATH,
                                watcher_name,
                                "RegisterStatusNotifierItem",
                                g_variant_new("(s)", CLEVO_SNI_OBJECT_PATH),
                                NULL,
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                &error);
    if (error)
    {
        g_debug("SNI watcher registration with %s failed: %s",
                watcher_name, error->message);
        g_clear_error(&error);
    }
}

static void clevo_sni_watcher_appeared(GDBusConnection *connection,
                                       const char *name,
                                       const char *name_owner,
                                       gpointer user_data)
{
    ClevoSni *sni = user_data;
    (void)connection;
    (void)name_owner;
    clevo_sni_register_with_watcher(sni, name);
}

static void clevo_sni_watcher_vanished(GDBusConnection *connection,
                                       const char *name,
                                       gpointer user_data)
{
    (void)connection;
    (void)name;
    (void)user_data;
}

ClevoSni *clevo_sni_new(const ClevoSniHandlers *handlers, void *user_data)
{
    ClevoSni *sni = g_new0(ClevoSni, 1);
    GError *error = NULL;
    char bus_name[128];

    sni->node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (error)
    {
        g_warning("Failed to parse SNI introspection XML: %s", error->message);
        g_clear_error(&error);
        clevo_sni_free(sni);
        return NULL;
    }

    sni->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error)
    {
        g_warning("Failed to connect to session bus for SNI: %s", error->message);
        g_clear_error(&error);
        clevo_sni_free(sni);
        return NULL;
    }

    snprintf(bus_name, sizeof(bus_name), "org.kde.StatusNotifierItem-%d-1", getpid());
    sni->bus_name = g_strdup(bus_name);
    sni->title = g_strdup("Clevo Indicator");
    sni->status = g_strdup("Active");
    sni->label = g_strdup("");
    sni->label_guide = g_strdup("");
    sni->icon_name = g_strdup("");
    sni->activation_token = NULL;
    sni->show_icon = FALSE;
    sni->prefer_activate = TRUE;
    sni->user_data = user_data;
    if (handlers)
        sni->handlers = *handlers;

    sni->owner_id = g_bus_own_name_on_connection(
        sni->connection,
        sni->bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        NULL,
        NULL,
        NULL,
        NULL);

    sni->registration_id = g_dbus_connection_register_object(
        sni->connection,
        CLEVO_SNI_OBJECT_PATH,
        sni->node_info->interfaces[0],
        &clevo_sni_vtable,
        sni,
        NULL,
        &error);
    if (error)
    {
        g_warning("Failed to register SNI object: %s", error->message);
        g_clear_error(&error);
        clevo_sni_free(sni);
        return NULL;
    }

    sni->kde_watcher_id = g_bus_watch_name_on_connection(
        sni->connection,
        CLEVO_SNI_KDE_WATCHER,
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        clevo_sni_watcher_appeared,
        clevo_sni_watcher_vanished,
        sni,
        NULL);
    sni->fdo_watcher_id = g_bus_watch_name_on_connection(
        sni->connection,
        CLEVO_SNI_FDO_WATCHER,
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        clevo_sni_watcher_appeared,
        clevo_sni_watcher_vanished,
        sni,
        NULL);

    return sni;
}

void clevo_sni_free(ClevoSni *sni)
{
    if (!sni)
        return;

    if (sni->kde_watcher_id)
        g_bus_unwatch_name(sni->kde_watcher_id);
    if (sni->fdo_watcher_id)
        g_bus_unwatch_name(sni->fdo_watcher_id);
    if (sni->registration_id && sni->connection)
        g_dbus_connection_unregister_object(sni->connection, sni->registration_id);
    if (sni->owner_id)
        g_bus_unown_name(sni->owner_id);
    if (sni->node_info)
        g_dbus_node_info_unref(sni->node_info);
    if (sni->connection)
        g_object_unref(sni->connection);

    g_free(sni->bus_name);
    g_free(sni->title);
    g_free(sni->status);
    g_free(sni->label);
    g_free(sni->label_guide);
    g_free(sni->icon_name);
    g_free(sni->activation_token);
    g_free(sni);
}

void clevo_sni_set_label(ClevoSni *sni, const char *label, const char *guide)
{
    if (!sni)
        return;

    g_free(sni->label);
    g_free(sni->label_guide);
    sni->label = g_strdup(label ? label : "");
    sni->label_guide = g_strdup(guide ? guide : "");

    clevo_sni_emit_properties_changed(
        sni,
        "XAyatanaLabel", g_variant_new_string(sni->label),
        "XAyatanaLabelGuide", g_variant_new_string(sni->label_guide));
}

void clevo_sni_set_title(ClevoSni *sni, const char *title)
{
    if (!sni)
        return;

    g_free(sni->title);
    sni->title = g_strdup(title ? title : "");
    clevo_sni_emit_properties_changed(
        sni,
        "Title", g_variant_new_string(sni->title),
        NULL, NULL);
}

void clevo_sni_set_status(ClevoSni *sni, const char *status)
{
    if (!sni)
        return;

    g_free(sni->status);
    sni->status = g_strdup(status ? status : "Active");
    clevo_sni_emit_properties_changed(
        sni,
        "Status", g_variant_new_string(sni->status),
        NULL, NULL);
}

void clevo_sni_set_icon_name(ClevoSni *sni, const char *icon_name)
{
    if (!sni)
        return;

    g_free(sni->icon_name);
    sni->icon_name = g_strdup(icon_name ? icon_name : "");
    clevo_sni_emit_properties_changed(
        sni,
        "IconName", g_variant_new_string(sni->icon_name),
        NULL, NULL);
}

void clevo_sni_set_show_icon(ClevoSni *sni, gboolean show_icon)
{
    if (!sni)
        return;

    sni->show_icon = !!show_icon;
    clevo_sni_emit_properties_changed(
        sni,
        "XClevoShowIcon", g_variant_new_boolean(sni->show_icon),
        NULL, NULL);
}

void clevo_sni_set_prefer_activate(ClevoSni *sni, gboolean prefer_activate)
{
    if (!sni)
        return;

    sni->prefer_activate = !!prefer_activate;
    clevo_sni_emit_properties_changed(
        sni,
        "XClevoPreferActivate", g_variant_new_boolean(sni->prefer_activate),
        NULL, NULL);
}

char *clevo_sni_take_activation_token(ClevoSni *sni)
{
    char *token;

    if (!sni)
        return NULL;

    token = sni->activation_token;
    sni->activation_token = NULL;
    return token;
}

const char *clevo_sni_get_bus_name(ClevoSni *sni)
{
    return sni ? sni->bus_name : NULL;
}

const char *clevo_sni_get_object_path(ClevoSni *sni)
{
    (void)sni;
    return CLEVO_SNI_OBJECT_PATH;
}
