/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Rodrigo Moya
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <X11/Xatom.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#include "mate-settings-profile.h"
#include "msd-xsettings-manager.h"
#include "xsettings-manager.h"
#include "fontconfig-monitor.h"

#define MATE_XSETTINGS_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), MATE_TYPE_XSETTINGS_MANAGER, MateXSettingsManagerPrivate))

#define MOUSE_SCHEMA          "org.mate.peripherals-mouse"
#define INTERFACE_SCHEMA      "org.mate.interface"
#define SOUND_SCHEMA          "org.mate.sound"

#define FONT_RENDER_SCHEMA    "org.mate.font-rendering"
#define FONT_ANTIALIASING_KEY "antialiasing"
#define FONT_HINTING_KEY      "hinting"
#define FONT_RGBA_ORDER_KEY   "rgba-order"
#define FONT_DPI_KEY          "dpi"

/* X servers sometimes lie about the screen's physical dimensions, so we cannot
 * compute an accurate DPI value.  When this happens, the user gets fonts that
 * are too huge or too tiny.  So, we see what the server returns:  if it reports
 * something outside of the range [DPI_LOW_REASONABLE_VALUE,
 * DPI_HIGH_REASONABLE_VALUE], then we assume that it is lying and we use
 * DPI_FALLBACK instead.
 *
 * See get_dpi_from_gsettings_or_server() below, and also
 * https://bugzilla.novell.com/show_bug.cgi?id=217790
 */
#define DPI_FALLBACK 96
#define DPI_LOW_REASONABLE_VALUE 50
#define DPI_HIGH_REASONABLE_VALUE 500

typedef struct _TranslationEntry TranslationEntry;
typedef void (* TranslationFunc) (MateXSettingsManager  *manager,
                                  TranslationEntry      *trans,
                                  GVariant              *value);

struct _TranslationEntry {
        const char     *gsettings_schema;
        const char     *gsettings_key;
        const char     *xsetting_name;

        TranslationFunc translate;
};

struct MateXSettingsManagerPrivate
{
        XSettingsManager **managers;
        GHashTable *gsettings;
        GSettings *gsettings_font;
        fontconfig_monitor_handle_t *fontconfig_handle;
};

#define MSD_XSETTINGS_ERROR msd_xsettings_error_quark ()

enum {
        MSD_XSETTINGS_ERROR_INIT
};

static void     mate_xsettings_manager_class_init  (MateXSettingsManagerClass *klass);
static void     mate_xsettings_manager_init        (MateXSettingsManager      *xsettings_manager);
static void     mate_xsettings_manager_finalize    (GObject                  *object);

G_DEFINE_TYPE (MateXSettingsManager, mate_xsettings_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static GQuark
msd_xsettings_error_quark (void)
{
        return g_quark_from_static_string ("msd-xsettings-error-quark");
}

static void
translate_bool_int (MateXSettingsManager  *manager,
                    TranslationEntry      *trans,
                    GVariant              *value)
{
        int i;

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_int (manager->priv->managers [i], trans->xsetting_name,
                                           g_variant_get_boolean (value));
        }
}

static void
translate_int_int (MateXSettingsManager  *manager,
                   TranslationEntry      *trans,
                   GVariant              *value)
{
        int i;

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_int (manager->priv->managers [i], trans->xsetting_name,
                                           g_variant_get_int32 (value));
        }
}

static void
translate_string_string (MateXSettingsManager  *manager,
                         TranslationEntry      *trans,
                         GVariant              *value)
{
        int i;

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_string (manager->priv->managers [i],
                                              trans->xsetting_name,
                                              g_variant_get_string (value, NULL));
        }
}

static void
translate_string_string_toolbar (MateXSettingsManager  *manager,
                                 TranslationEntry      *trans,
                                 GVariant              *value)
{
        int         i;
        const char *tmp;

        /* This is kind of a workaround since GNOME expects the key value to be
         * "both_horiz" and gtk+ wants the XSetting to be "both-horiz".
         */
        tmp = g_variant_get_string (value, NULL);
        if (tmp && strcmp (tmp, "both_horiz") == 0) {
                tmp = "both-horiz";
        }

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_string (manager->priv->managers [i],
                                              trans->xsetting_name,
                                              tmp);
        }
}

static TranslationEntry translations [] = {
        { MOUSE_SCHEMA,     "double-click",           "Net/DoubleClickTime",           translate_int_int },
        { MOUSE_SCHEMA,     "drag-threshold",         "Net/DndDragThreshold",          translate_int_int },
        { MOUSE_SCHEMA,     "cursor-theme",           "Gtk/CursorThemeName",           translate_string_string },
        { MOUSE_SCHEMA,     "cursor-size",            "Gtk/CursorThemeSize",           translate_int_int },

        { INTERFACE_SCHEMA, "font-name",              "Gtk/FontName",                  translate_string_string },
        { INTERFACE_SCHEMA, "gtk-key-theme",          "Gtk/KeyThemeName",              translate_string_string },
        { INTERFACE_SCHEMA, "toolbar-style",          "Gtk/ToolbarStyle",              translate_string_string_toolbar },
        { INTERFACE_SCHEMA, "toolbar-icons-size",     "Gtk/ToolbarIconSize",           translate_string_string },
        { INTERFACE_SCHEMA, "can-change-accels",      "Gtk/CanChangeAccels",           translate_bool_int },
        { INTERFACE_SCHEMA, "cursor-blink",           "Net/CursorBlink",               translate_bool_int },
        { INTERFACE_SCHEMA, "cursor-blink-time",      "Net/CursorBlinkTime",           translate_int_int },
        { INTERFACE_SCHEMA, "gtk-theme",              "Net/ThemeName",                 translate_string_string },
        { INTERFACE_SCHEMA, "gtk-color-scheme",       "Gtk/ColorScheme",               translate_string_string },
        { INTERFACE_SCHEMA, "gtk-im-preedit-style",   "Gtk/IMPreeditStyle",            translate_string_string },
        { INTERFACE_SCHEMA, "gtk-im-status-style",    "Gtk/IMStatusStyle",             translate_string_string },
        { INTERFACE_SCHEMA, "gtk-im-module",          "Gtk/IMModule",                  translate_string_string },
        { INTERFACE_SCHEMA, "icon-theme",             "Net/IconThemeName",             translate_string_string },
        { INTERFACE_SCHEMA, "file-chooser-backend",   "Gtk/FileChooserBackend",        translate_string_string },
        { INTERFACE_SCHEMA, "gtk-decoration-layout",  "Gtk/DecorationLayout",          translate_string_string },
        { INTERFACE_SCHEMA, "menus-have-icons",       "Gtk/MenuImages",                translate_bool_int },
        { INTERFACE_SCHEMA, "buttons-have-icons",     "Gtk/ButtonImages",              translate_bool_int },
        { INTERFACE_SCHEMA, "menubar-accel",          "Gtk/MenuBarAccel",              translate_string_string },
        { INTERFACE_SCHEMA, "show-input-method-menu", "Gtk/ShowInputMethodMenu",       translate_bool_int },
        { INTERFACE_SCHEMA, "show-unicode-menu",      "Gtk/ShowUnicodeMenu",           translate_bool_int },
        { INTERFACE_SCHEMA, "automatic-mnemonics",    "Gtk/AutoMnemonics",             translate_bool_int },
        { INTERFACE_SCHEMA, "gtk-enable-animations",  "Gtk/EnableAnimations",          translate_bool_int },
        { INTERFACE_SCHEMA, "gtk-dialogs-use-header", "Gtk/DialogsUseHeader",          translate_bool_int },

        { SOUND_SCHEMA, "theme-name",                 "Net/SoundThemeName",            translate_string_string },
        { SOUND_SCHEMA, "event-sounds",               "Net/EnableEventSounds" ,        translate_bool_int },
        { SOUND_SCHEMA, "input-feedback-sounds",      "Net/EnableInputFeedbackSounds", translate_bool_int }
};

static double
dpi_from_pixels_and_mm (int pixels,
                        int mm)
{
        double dpi;

        if (mm >= 1)
                dpi = pixels / (mm / 25.4);
        else
                dpi = 0;

        return dpi;
}

static double
get_dpi_from_x_server (void)
{
        GdkScreen *screen;
        double     dpi;

        screen = gdk_screen_get_default ();
        if (screen != NULL) {
                double width_dpi, height_dpi;

                width_dpi = dpi_from_pixels_and_mm (gdk_screen_get_width (screen), gdk_screen_get_width_mm (screen));
                height_dpi = dpi_from_pixels_and_mm (gdk_screen_get_height (screen), gdk_screen_get_height_mm (screen));

                if (width_dpi < DPI_LOW_REASONABLE_VALUE || width_dpi > DPI_HIGH_REASONABLE_VALUE
                    || height_dpi < DPI_LOW_REASONABLE_VALUE || height_dpi > DPI_HIGH_REASONABLE_VALUE) {
                        dpi = DPI_FALLBACK;
                } else {
                        dpi = (width_dpi + height_dpi) / 2.0;
                }
        } else {
                /* Huh!?  No screen? */

                dpi = DPI_FALLBACK;
        }

        return dpi;
}

static double
get_dpi_from_gsettings_or_x_server (GSettings *gsettings)
{
        double value;
        double dpi;

        value = g_settings_get_double (gsettings, FONT_DPI_KEY);

        /* If the user has ever set the DPI preference in GSettings, we use that.
         * Otherwise, we see if the X server reports a reasonable DPI value:  some X
         * servers report completely bogus values, and the user gets huge or tiny
         * fonts which are unusable.
         */

        if (value != 0) {
                dpi = value;
        } else {
                dpi = get_dpi_from_x_server ();
        }

        return dpi;
}

typedef struct
{
        gboolean    antialias;
        gboolean    hinting;
        int         dpi;
        const char *rgba;
        const char *hintstyle;
} MateXftSettings;

static const char *rgba_types[] = { "rgb", "bgr", "vbgr", "vrgb" };

/* Read GSettings values and determine the appropriate Xft settings based on them
 * This probably could be done a bit more cleanly with g_settings_get_enum
 */
static void
xft_settings_get (GSettings        *gsettings,
                  MateXftSettings *settings)
{
        char  *antialiasing;
        char  *hinting;
        char  *rgba_order;
        double dpi;

        antialiasing = g_settings_get_string (gsettings, FONT_ANTIALIASING_KEY);
        hinting = g_settings_get_string (gsettings, FONT_HINTING_KEY);
        rgba_order = g_settings_get_string (gsettings, FONT_RGBA_ORDER_KEY);
        dpi = get_dpi_from_gsettings_or_x_server (gsettings);

        settings->antialias = TRUE;
        settings->hinting = TRUE;
        settings->hintstyle = "hintfull";
        settings->dpi = dpi * 1024; /* Xft wants 1/1024ths of an inch */
        settings->rgba = "rgb";

        if (rgba_order) {
                int i;
                gboolean found = FALSE;

                for (i = 0; i < G_N_ELEMENTS (rgba_types) && !found; i++) {
                        if (strcmp (rgba_order, rgba_types[i]) == 0) {
                                settings->rgba = rgba_types[i];
                                found = TRUE;
                        }
                }

                if (!found) {
                        g_warning ("Invalid value for " FONT_RGBA_ORDER_KEY ": '%s'",
                                   rgba_order);
                }
        }

        if (hinting) {
                if (strcmp (hinting, "none") == 0) {
                        settings->hinting = 0;
                        settings->hintstyle = "hintnone";
                } else if (strcmp (hinting, "slight") == 0) {
                        settings->hinting = 1;
                        settings->hintstyle = "hintslight";
                } else if (strcmp (hinting, "medium") == 0) {
                        settings->hinting = 1;
                        settings->hintstyle = "hintmedium";
                } else if (strcmp (hinting, "full") == 0) {
                        settings->hinting = 1;
                        settings->hintstyle = "hintfull";
                } else {
                        g_warning ("Invalid value for " FONT_HINTING_KEY ": '%s'",
                                   hinting);
                }
        }

        if (antialiasing) {
                gboolean use_rgba = FALSE;

                if (strcmp (antialiasing, "none") == 0) {
                        settings->antialias = 0;
                } else if (strcmp (antialiasing, "grayscale") == 0) {
                        settings->antialias = 1;
                } else if (strcmp (antialiasing, "rgba") == 0) {
                        settings->antialias = 1;
                        use_rgba = TRUE;
                } else {
                        g_warning ("Invalid value for " FONT_ANTIALIASING_KEY " : '%s'",
                                   antialiasing);
                }

                if (!use_rgba) {
                        settings->rgba = "none";
                }
        }

        g_free (rgba_order);
        g_free (hinting);
        g_free (antialiasing);
}

static void
xft_settings_set_xsettings (MateXSettingsManager *manager,
                            MateXftSettings      *settings)
{
        int i;

        mate_settings_profile_start (NULL);

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_int (manager->priv->managers [i], "Xft/Antialias", settings->antialias);
                xsettings_manager_set_int (manager->priv->managers [i], "Xft/Hinting", settings->hinting);
                xsettings_manager_set_string (manager->priv->managers [i], "Xft/HintStyle", settings->hintstyle);
                xsettings_manager_set_int (manager->priv->managers [i], "Xft/DPI", settings->dpi);
                xsettings_manager_set_string (manager->priv->managers [i], "Xft/RGBA", settings->rgba);
                xsettings_manager_set_string (manager->priv->managers [i], "Xft/lcdfilter",
                                              g_str_equal (settings->rgba, "rgb") ? "lcddefault" : "none");
        }
        mate_settings_profile_end (NULL);
}

static void
update_property (GString *props, const gchar* key, const gchar* value)
{
        gchar* needle;
        size_t needle_len;
        gchar* found = NULL;

        /* update an existing property */
        needle = g_strconcat (key, ":", NULL);
        needle_len = strlen (needle);
        if (g_str_has_prefix (props->str, needle))
                found = props->str;
        else 
                found = strstr (props->str, needle);

        if (found) {
                size_t value_index;
                gchar* end;

                end = strchr (found, '\n');
                value_index = (found - props->str) + needle_len + 1;
                g_string_erase (props, value_index, end ? (end - found - needle_len) : -1);
                g_string_insert (props, value_index, "\n");
                g_string_insert (props, value_index, value);
        } else {
                g_string_append_printf (props, "%s:\t%s\n", key, value);
        }

        g_free (needle);
}

static void
xft_settings_set_xresources (MateXftSettings *settings)
{
        GString    *add_string;
        char        dpibuf[G_ASCII_DTOSTR_BUF_SIZE];
        Display    *dpy;

        mate_settings_profile_start (NULL);

        /* get existing properties */
        dpy = XOpenDisplay (NULL);
        g_return_if_fail (dpy != NULL);
        add_string = g_string_new (XResourceManagerString (dpy));

        g_debug("xft_settings_set_xresources: orig res '%s'", add_string->str);

        update_property (add_string, "Xft.dpi",
                                g_ascii_dtostr (dpibuf, sizeof (dpibuf), (double) settings->dpi / 1024.0));
        update_property (add_string, "Xft.antialias",
                                settings->antialias ? "1" : "0");
        update_property (add_string, "Xft.hinting",
                                settings->hinting ? "1" : "0");
        update_property (add_string, "Xft.hintstyle",
                                settings->hintstyle);
        update_property (add_string, "Xft.rgba",
                                settings->rgba);
        update_property (add_string, "Xft.lcdfilter",
                         g_str_equal (settings->rgba, "rgb") ? "lcddefault" : "none");

        g_debug("xft_settings_set_xresources: new res '%s'", add_string->str);

        /* Set the new X property */
        XChangeProperty(dpy, RootWindow (dpy, 0),
                XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace, (const unsigned char *)add_string->str, add_string->len);
        XCloseDisplay (dpy);

        g_string_free (add_string, TRUE);

        mate_settings_profile_end (NULL);
}

/* We mirror the Xft properties both through XSETTINGS and through
 * X resources
 */
static void
update_xft_settings (MateXSettingsManager *manager,
                     GSettings            *gsettings)
{
        MateXftSettings settings;

        mate_settings_profile_start (NULL);

        xft_settings_get (gsettings, &settings);
        xft_settings_set_xsettings (manager, &settings);
        xft_settings_set_xresources (&settings);

        mate_settings_profile_end (NULL);
}

static void
xft_callback (GSettings            *gsettings,
              gchar                *key,
              MateXSettingsManager *manager)
{
        int i;

        update_xft_settings (manager, gsettings);

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_notify (manager->priv->managers [i]);
        }
}

static void
fontconfig_callback (fontconfig_monitor_handle_t *handle,
                     MateXSettingsManager       *manager)
{
        int i;
        int timestamp = time (NULL);

        mate_settings_profile_start (NULL);

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_int (manager->priv->managers [i], "Fontconfig/Timestamp", timestamp);
                xsettings_manager_notify (manager->priv->managers [i]);
        }
        mate_settings_profile_end (NULL);
}

static gboolean
start_fontconfig_monitor_idle_cb (MateXSettingsManager *manager)
{
        mate_settings_profile_start (NULL);

        manager->priv->fontconfig_handle = fontconfig_monitor_start ((GFunc) fontconfig_callback, manager);

        mate_settings_profile_end (NULL);

        return FALSE;
}

static void
start_fontconfig_monitor (MateXSettingsManager  *manager)
{
        mate_settings_profile_start (NULL);

        fontconfig_cache_init ();

        g_idle_add ((GSourceFunc) start_fontconfig_monitor_idle_cb, manager);

        mate_settings_profile_end (NULL);
}

static void
stop_fontconfig_monitor (MateXSettingsManager  *manager)
{
        if (manager->priv->fontconfig_handle) {
                fontconfig_monitor_stop (manager->priv->fontconfig_handle);
                manager->priv->fontconfig_handle = NULL;
        }
}

static void
process_value (MateXSettingsManager *manager,
               TranslationEntry     *trans,
               GVariant             *value)
{
        (* trans->translate) (manager, trans, value);
}

static TranslationEntry *
find_translation_entry (GSettings *gsettings, const char *key)
{
        guint i;
        char *schema;

        g_object_get (gsettings, "schema", &schema, NULL);

        for (i = 0; i < G_N_ELEMENTS (translations); i++) {
                if (g_str_equal (schema, translations[i].gsettings_schema) &&
                    g_str_equal (key, translations[i].gsettings_key)) {
                            g_free (schema);
                        return &translations[i];
                }
        }

        g_free (schema);

        return NULL;
}

static void
xsettings_callback (GSettings             *gsettings,
                    const char            *key,
                    MateXSettingsManager  *manager)
{
        TranslationEntry *trans;
        int               i;
        GVariant         *value;

        trans = find_translation_entry (gsettings, key);
        if (trans == NULL) {
                return;
        }

        value = g_settings_get_value (gsettings, key);

        process_value (manager, trans, value);

        g_variant_unref (value);

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_set_string (manager->priv->managers [i],
                                              "Net/FallbackIconTheme",
                                              "mate");
        }

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_notify (manager->priv->managers [i]);
        }
}

static void
terminate_cb (void *data)
{
        gboolean *terminated = data;

        if (*terminated) {
                return;
        }

        *terminated = TRUE;

        gtk_main_quit ();
}

static gboolean
setup_xsettings_managers (MateXSettingsManager *manager)
{
        GdkDisplay *display;
        int         i;
        int         n_screens;
        gboolean    res;
        gboolean    terminated;

        display = gdk_display_get_default ();
#if GTK_CHECK_VERSION(3, 10, 0)
        n_screens = 1;
#else
        n_screens = gdk_display_get_n_screens (display);
#endif

        res = xsettings_manager_check_running (gdk_x11_display_get_xdisplay (display),
                                               gdk_screen_get_number (gdk_screen_get_default ()));
        if (res) {
                g_warning ("You can only run one xsettings manager at a time; exiting");
                return FALSE;
        }

        manager->priv->managers = g_new0 (XSettingsManager *, n_screens + 1);

        terminated = FALSE;
        for (i = 0; i < n_screens; i++) {
                GdkScreen *screen;

                screen = gdk_display_get_screen (display, i);

                manager->priv->managers [i] = xsettings_manager_new (gdk_x11_display_get_xdisplay (display),
                                                                     gdk_screen_get_number (screen),
                                                                     terminate_cb,
                                                                     &terminated);
                if (! manager->priv->managers [i]) {
                        g_warning ("Could not create xsettings manager for screen %d!", i);
                        return FALSE;
                }
        }

        return TRUE;
}

gboolean
mate_xsettings_manager_start (MateXSettingsManager *manager,
                               GError               **error)
{
        guint        i;
        GList       *list, *l;

        g_debug ("Starting xsettings manager");
        mate_settings_profile_start (NULL);

        if (!setup_xsettings_managers (manager)) {
                g_set_error (error, MSD_XSETTINGS_ERROR,
                             MSD_XSETTINGS_ERROR_INIT,
                             "Could not initialize xsettings manager.");
                return FALSE;
        }

        manager->priv->gsettings = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                         NULL, (GDestroyNotify) g_object_unref);

        g_hash_table_insert (manager->priv->gsettings,
                             MOUSE_SCHEMA, g_settings_new (MOUSE_SCHEMA));
        g_hash_table_insert (manager->priv->gsettings,
                             INTERFACE_SCHEMA, g_settings_new (INTERFACE_SCHEMA));
        g_hash_table_insert (manager->priv->gsettings,
                             SOUND_SCHEMA, g_settings_new (SOUND_SCHEMA));

        for (i = 0; i < G_N_ELEMENTS (translations); i++) {
                GVariant  *val;
                GSettings *gsettings;

                gsettings = g_hash_table_lookup (manager->priv->gsettings,
                                                translations[i].gsettings_schema);

		if (gsettings == NULL) {
			g_warning ("Schemas '%s' has not been setup", translations[i].gsettings_schema);
			continue;
		}

                val = g_settings_get_value (gsettings, translations[i].gsettings_key);

                process_value (manager, &translations[i], val);
                g_variant_unref (val);
        }

        list = g_hash_table_get_values (manager->priv->gsettings);
        for (l = list; l != NULL; l = l->next) {
                g_signal_connect_object (G_OBJECT (l->data), "changed",
                			 G_CALLBACK (xsettings_callback), manager, 0);
        }
        g_list_free (list);

        manager->priv->gsettings_font = g_settings_new (FONT_RENDER_SCHEMA);
        g_signal_connect (manager->priv->gsettings_font, "changed", G_CALLBACK (xft_callback), manager);
        update_xft_settings (manager, manager->priv->gsettings_font);

        start_fontconfig_monitor (manager);

        for (i = 0; manager->priv->managers [i]; i++)
                xsettings_manager_set_string (manager->priv->managers [i],
                                              "Net/FallbackIconTheme",
                                              "mate");

        for (i = 0; manager->priv->managers [i]; i++) {
                xsettings_manager_notify (manager->priv->managers [i]);
        }

        mate_settings_profile_end (NULL);

        return TRUE;
}

void
mate_xsettings_manager_stop (MateXSettingsManager *manager)
{
        MateXSettingsManagerPrivate *p = manager->priv;
        int i;

        g_debug ("Stopping xsettings manager");

        if (p->managers != NULL) {
                for (i = 0; p->managers [i]; ++i)
                        xsettings_manager_destroy (p->managers [i]);

                g_free (p->managers);
                p->managers = NULL;
        }

        if (p->gsettings != NULL) {
                g_hash_table_destroy (p->gsettings);
                p->gsettings = NULL;
        }

        if (p->gsettings_font != NULL) {
                g_object_unref (p->gsettings_font);
                p->gsettings_font = NULL;
        }

        stop_fontconfig_monitor (manager);

}

static void
mate_xsettings_manager_set_property (GObject        *object,
                                      guint           prop_id,
                                      const GValue   *value,
                                      GParamSpec     *pspec)
{
        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
mate_xsettings_manager_get_property (GObject        *object,
                                      guint           prop_id,
                                      GValue         *value,
                                      GParamSpec     *pspec)
{
        switch (prop_id) {
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static GObject *
mate_xsettings_manager_constructor (GType                  type,
                                     guint                  n_construct_properties,
                                     GObjectConstructParam *construct_properties)
{
        MateXSettingsManager      *xsettings_manager;

        xsettings_manager = MATE_XSETTINGS_MANAGER (G_OBJECT_CLASS (mate_xsettings_manager_parent_class)->constructor (type,
                                                                                                                  n_construct_properties,
                                                                                                                  construct_properties));

        return G_OBJECT (xsettings_manager);
}

static void
mate_xsettings_manager_dispose (GObject *object)
{
        G_OBJECT_CLASS (mate_xsettings_manager_parent_class)->dispose (object);
}

static void
mate_xsettings_manager_class_init (MateXSettingsManagerClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = mate_xsettings_manager_get_property;
        object_class->set_property = mate_xsettings_manager_set_property;
        object_class->constructor = mate_xsettings_manager_constructor;
        object_class->dispose = mate_xsettings_manager_dispose;
        object_class->finalize = mate_xsettings_manager_finalize;

        g_type_class_add_private (klass, sizeof (MateXSettingsManagerPrivate));
}

static void
mate_xsettings_manager_init (MateXSettingsManager *manager)
{
        manager->priv = MATE_XSETTINGS_MANAGER_GET_PRIVATE (manager);
}

static void
mate_xsettings_manager_finalize (GObject *object)
{
        MateXSettingsManager *xsettings_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (MATE_IS_XSETTINGS_MANAGER (object));

        xsettings_manager = MATE_XSETTINGS_MANAGER (object);

        g_return_if_fail (xsettings_manager->priv != NULL);

        G_OBJECT_CLASS (mate_xsettings_manager_parent_class)->finalize (object);
}

MateXSettingsManager *
mate_xsettings_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (MATE_TYPE_XSETTINGS_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return MATE_XSETTINGS_MANAGER (manager_object);
}
