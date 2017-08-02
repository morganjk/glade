/*
 * Copyright (C) 2001 Ximian, Inc.
 * Copyright (C) 2007 Vincent Geddes.
 * Copyright (C) 2012-2017 Juan Pablo Ugarte.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Authors:
 *   Chema Celorio <chema@celorio.com>
 *   Paolo Borelli <pborelli@katamail.com>
 *   Vincent Geddes <vgeddes@gnome.org>
 *   Juan Pablo Ugarte <juanpablougarte@gmail.com>
 */

#include <config.h>

#include "glade-window.h"
#include "glade-resources.h"
#include "glade-preferences.h"
#include "glade-registration.h"

#include <gladeui/glade.h>
#include <gladeui/glade-popup.h>
#include <gladeui/glade-inspector.h>
#include <gladeui/glade-adaptor-chooser.h>

#include <gladeui/glade-project.h>

#include <string.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#ifdef MAC_INTEGRATION
#  include <gtkosxapplication.h>
#endif


#define ACTION_GROUP_STATIC             "GladeStatic"
#define ACTION_GROUP_PROJECT            "GladeProject"
#define ACTION_GROUP_PROJECTS_LIST_MENU "GladeProjectsList"

#define READONLY_INDICATOR (_("[Read Only]"))

#define URL_DEVELOPER_MANUAL "http://library.gnome.org/devel/gladeui/"

#define CONFIG_GROUP_WINDOWS        "Glade Windows"
#define GLADE_WINDOW_DEFAULT_WIDTH  720
#define GLADE_WINDOW_DEFAULT_HEIGHT 540
#define CONFIG_KEY_X                "x"
#define CONFIG_KEY_Y                "y"
#define CONFIG_KEY_WIDTH            "width"
#define CONFIG_KEY_HEIGHT           "height"
#define CONFIG_KEY_MAXIMIZED        "maximized"

#define CONFIG_GROUP_LOAD_SAVE      "Load and Save"
#define CONFIG_KEY_BACKUP           "backup"
#define CONFIG_KEY_AUTOSAVE         "autosave"
#define CONFIG_KEY_AUTOSAVE_SECONDS "autosave-seconds"

#define GLADE_WINDOW_ACTIVE_VIEW(w) ((GladeDesignView *) gtk_stack_get_visible_child (w->priv->stack))

struct _GladeWindowPrivate
{
  GladeApp *app;

  GtkStack *stack;

  GtkHeaderBar *headerbar;
  GtkWindow *about_dialog;
  GladePreferences *preferences;

  GtkLabel *title;
  GtkLabel *subtitle;
  GtkWidget *project_button;

  GladeAdaptorChooser *adaptor_chooser;
  GtkStack *inspectors_stack;           /* Cached per project inspectors */

  GladeEditor  *editor;                 /* The editor */

  GtkWidget *statusbar;                 /* A pointer to the status bar. */
  guint statusbar_context_id;           /* The context id of general messages */
  guint statusbar_menu_context_id;      /* The context id of the menu bar */
  guint statusbar_actions_context_id;   /* The context id of actions messages */

  GtkAccelGroup *accelgroup;

  GtkAction *save_action, *quit_action;
  GtkAction *undo_action, *redo_action, *cut_action, *copy_action, *paste_action, *delete_action;
  GtkAction *selector_radioaction;

  GtkActionGroup *project_actiongroup;      /* All the project actions */
  GtkActionGroup *pointer_mode_actiongroup;
  GtkActionGroup *static_actiongroup;
  
  GtkRecentManager *recent_manager;
  GtkWidget *recent_menu;

  GtkWidget *quit_menuitem;
  GtkWidget *about_menuitem;

  gchar *default_path;          /* the default path for open/save operations */

  GtkMenuButton *undo_menu_button; /* customized buttons for undo/redo with history */
  GtkMenuButton *redo_menu_button;

  GtkWidget *toolbar;           /* Actions are added to the toolbar */
  gint actions_start;           /* start of action items */

  GtkWidget *center_paned;
  GtkWidget *left_paned;
  GtkWidget *open_button_box;   /* gtk_button_box_set_layout() set homogeneous to TRUE, and we do not want that in this case  */

  GtkWidget *registration;      /* Registration and user survey dialog */
  
  GdkRectangle position;
};

static void check_reload_project (GladeWindow *window, GladeProject *project);

G_DEFINE_TYPE_WITH_PRIVATE (GladeWindow, glade_window, GTK_TYPE_WINDOW)

/* the following functions are taken from gedit-utils.c */
static gchar *
str_middle_truncate (const gchar *string, guint truncate_length)
{
  GString *truncated;
  guint length;
  guint n_chars;
  guint num_left_chars;
  guint right_offset;
  guint delimiter_length;
  const gchar *delimiter = "\342\200\246";

  g_return_val_if_fail (string != NULL, NULL);

  length = strlen (string);

  g_return_val_if_fail (g_utf8_validate (string, length, NULL), NULL);

  /* It doesnt make sense to truncate strings to less than
   * the size of the delimiter plus 2 characters (one on each
   * side)
   */
  delimiter_length = g_utf8_strlen (delimiter, -1);
  if (truncate_length < (delimiter_length + 2))
    {
      return g_strdup (string);
    }

  n_chars = g_utf8_strlen (string, length);

  /* Make sure the string is not already small enough. */
  if (n_chars <= truncate_length)
    {
      return g_strdup (string);
    }

  /* Find the 'middle' where the truncation will occur. */
  num_left_chars = (truncate_length - delimiter_length) / 2;
  right_offset = n_chars - truncate_length + num_left_chars + delimiter_length;

  truncated = g_string_new_len (string,
                                g_utf8_offset_to_pointer (string,
                                                          num_left_chars) -
                                string);
  g_string_append (truncated, delimiter);
  g_string_append (truncated, g_utf8_offset_to_pointer (string, right_offset));

  return g_string_free (truncated, FALSE);
}

/*
 * Doubles underscore to avoid spurious menu accels - taken from gedit-utils.c
 */
static gchar *
escape_underscores (const gchar *text, gssize length)
{
  GString *str;
  const gchar *p;
  const gchar *end;

  g_return_val_if_fail (text != NULL, NULL);

  if (length < 0)
    length = strlen (text);

  str = g_string_sized_new (length);

  p = text;
  end = text + length;

  while (p != end)
    {
      const gchar *next;
      next = g_utf8_next_char (p);

      switch (*p)
        {
          case '_':
            g_string_append (str, "__");
            break;
          default:
            g_string_append_len (str, p, next - p);
            break;
        }

      p = next;
    }

  return g_string_free (str, FALSE);
}

typedef enum
{
  FORMAT_NAME_MARK_UNSAVED = 1 << 0,
  FORMAT_NAME_ESCAPE_UNDERSCORES = 1 << 1,
  FORMAT_NAME_MIDDLE_TRUNCATE = 1 << 2
} FormatNameFlags;

#define MAX_TITLE_LENGTH 100

static gchar *
get_formatted_project_name_for_display (GladeProject *project,
                                        FormatNameFlags format_flags)
{
  gchar *name, *pass1, *pass2, *pass3;

  g_return_val_if_fail (project != NULL, NULL);

  name = glade_project_get_name (project);

  if ((format_flags & FORMAT_NAME_MARK_UNSAVED)
      && glade_project_get_modified (project))
    pass1 = g_strdup_printf ("*%s", name);
  else
    pass1 = g_strdup (name);

  if (format_flags & FORMAT_NAME_ESCAPE_UNDERSCORES)
    pass2 = escape_underscores (pass1, -1);
  else
    pass2 = g_strdup (pass1);

  if (format_flags & FORMAT_NAME_MIDDLE_TRUNCATE)
    pass3 = str_middle_truncate (pass2, MAX_TITLE_LENGTH);
  else
    pass3 = g_strdup (pass2);

  g_free (name);
  g_free (pass1);
  g_free (pass2);

  return pass3;
}

static void
refresh_title (GladeWindow *window)
{
  GladeProject *project = NULL;
  gchar *title, *name = NULL;
  const gchar *path;

  if (GLADE_WINDOW_ACTIVE_VIEW (window))
    {
      project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

      name = get_formatted_project_name_for_display (project,
                                                     FORMAT_NAME_MARK_UNSAVED |
                                                     FORMAT_NAME_MIDDLE_TRUNCATE);

      if (glade_project_get_readonly (project) != FALSE)
        title = g_strdup_printf ("%s %s", name, READONLY_INDICATOR);
      else
        title = g_strdup_printf ("%s", name);

      g_free (name);
    }
  else
    title = g_strdup (_("User Interface Designer"));

  gtk_label_set_label (window->priv->title, title);

  /* Show path */
  if (project && (path = glade_project_get_path (project)))
    {
      gchar *dirname = g_path_get_dirname (path);
      const gchar *home = g_get_home_dir ();

      if (g_str_has_prefix (dirname, home))
        {
          char *subtitle = &dirname[g_utf8_strlen (home, -1) - 1];
          subtitle[0] = '~';
          gtk_label_set_label (window->priv->subtitle, subtitle);
        }
      else
        gtk_label_set_label (window->priv->subtitle, dirname);

      gtk_style_context_add_class (gtk_widget_get_style_context (window->priv->project_button),
                                   "glade-tight-fit");
      gtk_widget_show (GTK_WIDGET (window->priv->subtitle));
      g_free (dirname);
    }
  else
    {
      gtk_style_context_remove_class (gtk_widget_get_style_context (window->priv->project_button),
                                     "glade-tight-fit");
      gtk_label_set_label (window->priv->subtitle, NULL);
      gtk_widget_hide (GTK_WIDGET (window->priv->subtitle));
    }

  g_free (title);
}

static const gchar *
get_default_path (GladeWindow *window)
{
  return window->priv->default_path;
}

static void
update_default_path (GladeWindow *window, const gchar *filename)
{
  gchar *path;

  g_return_if_fail (filename != NULL);

  path = g_path_get_dirname (filename);

  g_free (window->priv->default_path);
  window->priv->default_path = g_strdup (path);

  g_free (path);
}

static void
activate_action (GtkToolButton *toolbutton, GladeWidgetAction *action)
{
  GladeWidget   *widget;
  GWActionClass *aclass = glade_widget_action_get_class (action);

  if ((widget = g_object_get_data (G_OBJECT (toolbutton), "glade-widget")))
    glade_widget_adaptor_action_activate (glade_widget_get_adaptor (widget),
                                          glade_widget_get_object (widget), 
					  aclass->path);
}

static void
action_notify_sensitive (GObject *gobject, GParamSpec *arg1, GtkWidget *item)
{
  GladeWidgetAction *action = GLADE_WIDGET_ACTION (gobject);
  gtk_widget_set_sensitive (item, glade_widget_action_get_sensitive (action));
}

static void
action_disconnect (gpointer data, GClosure *closure)
{
  g_signal_handlers_disconnect_matched (data, G_SIGNAL_MATCH_FUNC,
                                        0, 0, NULL,
                                        action_notify_sensitive, NULL);
}

static void
clean_actions (GladeWindow *window)
{
  GtkContainer *container = GTK_CONTAINER (window->priv->toolbar);
  GtkToolbar *bar = GTK_TOOLBAR (window->priv->toolbar);
  GtkToolItem *item;

  if (window->priv->actions_start)
    {
      while ((item =
              gtk_toolbar_get_nth_item (bar, window->priv->actions_start)))
        gtk_container_remove (container, GTK_WIDGET (item));
    }
}

static void
add_actions (GladeWindow *window, GladeWidget *widget, GList *actions)
{
  GtkToolbar *bar = GTK_TOOLBAR (window->priv->toolbar);
  GtkToolItem *item = gtk_separator_tool_item_new ();
  gint n = 0;
  GList *l;

  gtk_toolbar_insert (bar, item, -1);
  gtk_widget_show (GTK_WIDGET (item));

  if (window->priv->actions_start == 0)
    window->priv->actions_start = gtk_toolbar_get_item_index (bar, item);

  for (l = actions; l; l = g_list_next (l))
    {
      GladeWidgetAction *action = l->data;
      GWActionClass     *aclass = glade_widget_action_get_class (action);
      GtkWidget         *image;

      if (!aclass->important || !glade_widget_action_get_visible (action))
        continue;

      if (glade_widget_action_get_children (action))
        {
          g_warning ("Trying to add a group action to the toolbar is unsupported");
          continue;
        }

      image = gtk_image_new_from_icon_name ((aclass->stock) ? aclass->stock : "system-run",
                                            GTK_ICON_SIZE_LARGE_TOOLBAR);
      item = gtk_tool_button_new (image, NULL);
      if (aclass->label)
        gtk_widget_set_tooltip_text (GTK_WIDGET (item), aclass->label);

      g_object_set_data (G_OBJECT (item), "glade-widget", widget);

      /* We use destroy_data to keep track of notify::sensitive callbacks
       * on the action object and disconnect them when the toolbar item
       * gets destroyed.
       */
      g_signal_connect_data (item, "clicked",
                             G_CALLBACK (activate_action),
                             action, action_disconnect, 0);

      gtk_widget_set_sensitive (GTK_WIDGET (item), 
				glade_widget_action_get_sensitive (action));

      g_signal_connect (action, "notify::sensitive",
                        G_CALLBACK (activate_action), GTK_WIDGET (item));

      gtk_toolbar_insert (bar, item, -1);
      gtk_tool_item_set_homogeneous (item, FALSE);
      gtk_widget_show_all (GTK_WIDGET (item));
      n++;
    }

  if (n == 0)
    clean_actions (window);
}

static GladeProject *
get_active_project (GladeWindow *window)
{
  if (GLADE_WINDOW_ACTIVE_VIEW (window))
    return glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  return NULL;
}

static void
project_selection_changed_cb (GladeProject *project, GladeWindow *window)
{
  GladeProject *active_project;
  GladeWidget  *glade_widget = NULL;
  GList        *list;
  gint          num;

  active_project = get_active_project (window);

  /* This is sometimes called with a NULL project (to make the label
   * insensitive with no projects loaded)
   */
  g_return_if_fail (GLADE_IS_WINDOW (window));

  /* Only update the toolbar & workspace if the selection has changed on
   * the currently active project.
   */
  if (project == active_project)
    {
      list = glade_project_selection_get (project);
      num = g_list_length (list);

      if (num == 1 && !GLADE_IS_PLACEHOLDER (list->data))
        {
          glade_widget = glade_widget_get_from_gobject (G_OBJECT (list->data));

          clean_actions (window);
          if (glade_widget_get_actions (glade_widget))
            add_actions (window, glade_widget, glade_widget_get_actions (glade_widget));
        }
    }

  glade_editor_load_widget (window->priv->editor, glade_widget);
}

static gchar *
format_project_list_item_tooltip (GladeProject *project)
{
  gchar *tooltip, *path, *name;

  if (glade_project_get_path (project))
    {
      path =
          glade_utils_replace_home_dir_with_tilde (glade_project_get_path
                                                   (project));

      if (glade_project_get_readonly (project))
        {
          /* translators: referring to the action of activating a file named '%s'.
           *              we also indicate to users that the file may be read-only with
           *              the second '%s' */
          tooltip = g_strdup_printf (_("Activate '%s' %s"),
                                     path, READONLY_INDICATOR);
        }
      else
        {
          /* translators: referring to the action of activating a file named '%s' */
          tooltip = g_strdup_printf (_("Activate '%s'"), path);
        }
      g_free (path);
    }
  else
    {
      name = glade_project_get_name (project);
      /* FIXME add hint for translators */
      tooltip = g_strdup_printf (_("Activate '%s'"), name);
      g_free (name);
    }

  return tooltip;
}

static void
refresh_stack_title_for_project (GladeWindow *window, GladeProject *project)
{
  GList *children, *l;

  children = gtk_container_get_children (GTK_CONTAINER (window->priv->stack));
  for (l = children; l; l = l->next)
    {
      GtkWidget *view = l->data;

      if (project == glade_design_view_get_project (GLADE_DESIGN_VIEW (view)))
        {
          gchar *str = get_formatted_project_name_for_display (project,
                                                               FORMAT_NAME_MARK_UNSAVED |
                                                               FORMAT_NAME_MIDDLE_TRUNCATE);
          gtk_container_child_set (GTK_CONTAINER (window->priv->stack), view,
                                   "title", str, NULL);
          g_free (str);

          break;
        }
    }
  g_list_free (children);
}

static void
project_targets_changed_cb (GladeProject *project, GladeWindow *window)
{
  refresh_stack_title_for_project (window, project);
}

static void
change_menu_label (GtkAction *action,
                   const gchar *action_label,
                   const gchar *action_description)
{
  gchar *text, *tmp_text;

  g_return_if_fail (action_label != NULL);

  if (action_description == NULL)
    text = g_strdup (action_label);
  else
    {
      tmp_text = escape_underscores (action_description, -1);
      text = g_strdup_printf ("%s: %s", action_label, tmp_text);
      g_free (tmp_text);
    }

  gtk_action_set_label (action, text);

  g_free (text);
}

static void
refresh_undo_redo (GladeWindow *window, GladeProject *project)
{
  GladeCommand *undo = NULL, *redo = NULL;
  GladeWindowPrivate *priv = window->priv;
  gchar        *tooltip;

  if (project != NULL)
    {
      undo = glade_project_next_undo_item (project);
      redo = glade_project_next_redo_item (project);
    }

  /* Refresh Undo */
  gtk_action_set_sensitive (priv->undo_action, undo != NULL);

  change_menu_label (priv->undo_action, _("_Undo"),
                     undo ? glade_command_description (undo) : NULL);

  tooltip = g_strdup_printf (_("Undo: %s"),
                             undo ? glade_command_description (undo) : _("the last action"));
  g_object_set (priv->undo_action, "tooltip", tooltip, NULL);
  g_free (tooltip);

  /* Refresh Redo */
  gtk_action_set_sensitive (priv->redo_action, redo != NULL);

  change_menu_label (priv->redo_action, _("_Redo"),
                     redo ? glade_command_description (redo) : NULL);

  tooltip = g_strdup_printf (_("Redo: %s"),
                             redo ? glade_command_description (redo) : _("the last action"));
  g_object_set (priv->redo_action, "tooltip", tooltip, NULL);
  g_free (tooltip);

  /* Refresh menus */
  gtk_menu_button_set_popup (priv->undo_menu_button,
                             glade_project_undo_items (project));
  gtk_menu_button_set_popup (priv->redo_menu_button,
                             glade_project_redo_items (project));
}

static void
cancel_autosave (gpointer data)
{
  guint autosave_id = GPOINTER_TO_UINT (data);

  g_source_remove (autosave_id);
}

static gboolean
autosave_project (gpointer data)
{
  GladeProject *project = (GladeProject *)data;
  GladeWindow *window = GLADE_WINDOW (glade_app_get_window ());
  gchar *display_name;

  display_name = glade_project_get_name (project);

  if (glade_project_autosave (project, NULL))
    glade_util_flash_message (window->priv->statusbar,
			      window->priv->statusbar_actions_context_id,
			      _("Autosaving '%s'"), display_name);
  else
    /* This is problematic, should we be more intrusive and popup a dialog ? */
    glade_util_flash_message (window->priv->statusbar,
			      window->priv->statusbar_actions_context_id,
			      _("Error autosaving '%s'"), display_name);

  g_free (display_name);

  /* This will remove the source id */
  g_object_set_data (G_OBJECT (project), "glade-autosave-id", NULL);
  return FALSE;
}

static void
project_queue_autosave (GladeWindow  *window,
			GladeProject *project)
{
  if (glade_project_get_path (project) != NULL &&
      glade_project_get_modified (project) &&
      glade_preferences_autosave (window->priv->preferences))
    {
      guint autosave_id =
	g_timeout_add_seconds (glade_preferences_autosave_seconds (window->priv->preferences),
			       autosave_project, project);

      g_object_set_data_full (G_OBJECT (project), "glade-autosave-id",
			      GUINT_TO_POINTER (autosave_id), cancel_autosave);
    }
  else
      g_object_set_data (G_OBJECT (project), "glade-autosave-id", NULL);
}

static void
project_cancel_autosave (GladeProject *project)
{
  g_object_set_data (G_OBJECT (project), "glade-autosave-id", NULL);
}

static void
project_changed_cb (GladeProject *project, 
                    GladeCommand *command,
                    gboolean      execute,
                    GladeWindow  *window)
{
  GladeProject *active_project = get_active_project (window);

  if (project == active_project)
    refresh_undo_redo (window, project);

  project_queue_autosave (window, project);
}

static void
refresh_projects_list_item (GladeWindow *window, GladeProject *project)
{
  GtkAction *action;
  gchar *project_name;
  gchar *tooltip;

  /* Get associated action */
  action =
      GTK_ACTION (g_object_get_data
                  (G_OBJECT (project), "project-list-action"));

  /* Set action label */
  project_name = get_formatted_project_name_for_display (project,
                                                         FORMAT_NAME_MARK_UNSAVED
                                                         |
                                                         FORMAT_NAME_ESCAPE_UNDERSCORES
                                                         |
                                                         FORMAT_NAME_MIDDLE_TRUNCATE);

  g_object_set (action, "label", project_name, NULL);

  /* Set action tooltip */
  tooltip = format_project_list_item_tooltip (project);
  g_object_set (action, "tooltip", tooltip, NULL);

  g_free (tooltip);
  g_free (project_name);
}

static void
project_notify_handler_cb (GladeProject *project,
                           GParamSpec *spec,
                           GladeWindow *window)
{
  GladeProject *active_project = get_active_project (window);
  GladeWindowPrivate *priv = window->priv;

  if (strcmp (spec->name, "path") == 0)
    {
      refresh_title (window);
      refresh_stack_title_for_project (window, project);
    }
  else if (strcmp (spec->name, "format") == 0)
    {
      refresh_stack_title_for_project (window, project);
    }
  else if (strcmp (spec->name, "modified") == 0)
    {
      refresh_title (window);
      refresh_projects_list_item (window, project);
      refresh_stack_title_for_project (window, project);
    }
  else if (strcmp (spec->name, "read-only") == 0)
    {
      refresh_stack_title_for_project (window, project);

      gtk_action_set_sensitive (priv->save_action, !glade_project_get_readonly (project));
    }
  else if (strcmp (spec->name, "has-selection") == 0 && (project == active_project))
    {
      gtk_action_set_sensitive (priv->cut_action,
                                glade_project_get_has_selection (project));
      gtk_action_set_sensitive (priv->copy_action,
                                glade_project_get_has_selection (project));
      gtk_action_set_sensitive (priv->delete_action,
                                glade_project_get_has_selection (project));
    }
}

static void
clipboard_notify_handler_cb (GladeClipboard *clipboard,
                             GParamSpec *spec,
                             GladeWindow * window)
{
  if (strcmp (spec->name, "has-selection") == 0)
    {
      gtk_action_set_sensitive (window->priv->paste_action,
                                glade_clipboard_get_has_selection (clipboard));
    }
}

static void
on_pointer_mode_changed (GladeProject *project,
                         GParamSpec   *pspec,
                         GladeWindow  *window)
{
  GladeProject *active_project = get_active_project (window);
  GladeWindowPrivate *priv = window->priv;
  GladePointerMode mode;
  
  if (!active_project)
    {
      gtk_action_group_set_sensitive (priv->pointer_mode_actiongroup, FALSE);
      return;
    }
  else if (active_project != project)
    return;

  mode = glade_project_get_pointer_mode (project);
  if (mode == GLADE_POINTER_ADD_WIDGET) return;

  gtk_action_group_set_sensitive (priv->pointer_mode_actiongroup, TRUE);
  gtk_radio_action_set_current_value (GTK_RADIO_ACTION (priv->selector_radioaction),
                                      mode);
}

static void
set_sensitivity_according_to_project (GladeWindow  *window,
                                      GladeProject *project)
{
  GladeWindowPrivate *priv = window->priv;

  gtk_action_set_sensitive (priv->save_action, !glade_project_get_readonly (project));

  gtk_action_set_sensitive (priv->cut_action, glade_project_get_has_selection (project));

  gtk_action_set_sensitive (priv->copy_action, glade_project_get_has_selection (project));

  gtk_action_set_sensitive (priv->paste_action,
                            glade_clipboard_get_has_selection
                            (glade_app_get_clipboard ()));

  gtk_action_set_sensitive (priv->delete_action, glade_project_get_has_selection (project));
}

static gchar *
get_uri_from_project_path (const gchar *path)
{
  GError *error = NULL;
  gchar *uri = NULL;
  
  if (g_path_is_absolute (path))
    uri = g_filename_to_uri (path, NULL, &error);
  else
    {
      gchar *cwd = g_get_current_dir ();
      gchar *fullpath = g_build_filename (cwd, path, NULL);
      uri = g_filename_to_uri (fullpath, NULL, &error);
      g_free (cwd);
      g_free (fullpath);
    }
    
  if (error)
    {
      g_warning ("Could not convert local path \"%s\" to a uri: %s", path, error->message);
      g_error_free (error);
    }

  return uri;
}

static void
recent_add (GladeWindow *window, const gchar *path)
{
  gchar *uri = get_uri_from_project_path (path);
  GtkRecentData *recent_data;

  if (!uri)
    return;
  
  recent_data = g_slice_new (GtkRecentData);

  recent_data->display_name = NULL;
  recent_data->description = NULL;
  recent_data->mime_type = "application/x-glade";
  recent_data->app_name = (gchar *) g_get_application_name ();
  recent_data->app_exec = g_strjoin (" ", g_get_prgname (), "%u", NULL);
  recent_data->groups = NULL;
  recent_data->is_private = FALSE;

  gtk_recent_manager_add_full (window->priv->recent_manager, uri, recent_data);

  g_free (uri);
  g_free (recent_data->app_exec);
  g_slice_free (GtkRecentData, recent_data);
}

static void
recent_remove (GladeWindow * window, const gchar * path)
{
  gchar *uri = get_uri_from_project_path (path);

  if (!uri)
    return;

  gtk_recent_manager_remove_item (window->priv->recent_manager, uri, NULL);

  g_free (uri);
}

/* switch to a project and check if we need to reload it.
 *
 */
static void
switch_to_project (GladeWindow *window, GladeProject *project)
{
  GladeWindowPrivate *priv = window->priv;
  GtkWidget *view;

  view = GTK_WIDGET (glade_design_view_get_from_project (project));
  gtk_stack_set_visible_child (priv->stack, view);

  check_reload_project (window, project);
}

static void
on_open_action_activate (GtkAction *action, GladeWindow *window)
{
  GtkWidget *filechooser;
  gchar *path = NULL, *default_path;

  filechooser = glade_util_file_dialog_new (_("Open\342\200\246"), NULL,
                                            GTK_WINDOW (window),
                                            GLADE_FILE_DIALOG_ACTION_OPEN);


  default_path = g_strdup (get_default_path (window));
  if (default_path != NULL)
    {
      gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (filechooser),
                                           default_path);
      g_free (default_path);
    }

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_OK)
    path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));

  gtk_widget_destroy (filechooser);

  if (!path)
    return;

  glade_window_open_project (window, path);
  g_free (path);
}

static gboolean
check_loading_project_for_save (GladeProject *project)
{
  if (glade_project_is_loading (project))
    {
      gchar *name = glade_project_get_name (project);

      glade_util_ui_message (glade_app_get_window (),
                             GLADE_UI_INFO, NULL,
                             _("Project %s is still loading."), name);
      g_free (name);
      return TRUE;
    }
  return FALSE;
}

static gboolean
do_save (GladeWindow *window, GladeProject *project, const gchar *path)
{
  GError *error = NULL;
  GladeVerifyFlags verify_flags = 0;
  gchar *display_path = g_strdup (path);

  if (glade_preferences_backup (window->priv->preferences) &&
      !glade_project_backup (project, path, NULL))
    {
      if (!glade_util_ui_message (GTK_WIDGET (window),
				  GLADE_UI_ARE_YOU_SURE, NULL,
				  _("Failed to backup existing file, continue saving?")))
	{
	  g_free (display_path);
	  return FALSE;
	}
    }

  if (glade_preferences_warn_versioning (window->priv->preferences))
    verify_flags |= GLADE_VERIFY_VERSIONS;
  if (glade_preferences_warn_deprecations (window->priv->preferences))
    verify_flags |= GLADE_VERIFY_DEPRECATIONS;
  if (glade_preferences_warn_unrecognized (window->priv->preferences))
    verify_flags |= GLADE_VERIFY_UNRECOGNIZED;

  if (!glade_project_save_verify (project, path, verify_flags, &error))
    {
      if (error)
        {
	  /* Reset path so future saves will prompt the file chooser */
	  glade_project_reset_path (project);

          glade_util_ui_message (GTK_WIDGET (window), GLADE_UI_ERROR, NULL,
                                 _("Failed to save %s: %s"),
                                 display_path, error->message);
          g_error_free (error);
        }
      g_free (display_path);
      return FALSE;
    }

  /* Cancel any queued autosave when explicitly saving */
  project_cancel_autosave (project);

  g_free (display_path);
  return TRUE;
}

static void
save (GladeWindow *window, GladeProject *project, const gchar *path)
{
  gchar *display_name;
  time_t mtime;
  GtkWidget *dialog;
  GtkWidget *button;
  gint response;

  if (check_loading_project_for_save (project))
    return;

  /* check for external modification to the project file */
  if (glade_project_get_path (project))
    {
      mtime = glade_util_get_file_mtime (glade_project_get_path (project), NULL);

      if (mtime > glade_project_get_file_mtime (project))
	{

	  dialog = gtk_message_dialog_new (GTK_WINDOW (window),
					   GTK_DIALOG_MODAL,
					   GTK_MESSAGE_WARNING,
					   GTK_BUTTONS_NONE,
					   _("The file %s has been modified since reading it"),
					   glade_project_get_path (project));

	  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						    _("If you save it, all the external changes could be lost. "
						      "Save it anyway?"));

	  gtk_window_set_title (GTK_WINDOW (dialog), "");

	  button = gtk_button_new_with_mnemonic (_("_Save Anyway"));
	  gtk_button_set_image (GTK_BUTTON (button),
				gtk_image_new_from_icon_name ("document-save",
                                                              GTK_ICON_SIZE_BUTTON));
	  gtk_widget_show (button);

	  gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button,
					GTK_RESPONSE_ACCEPT);
	  gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Don't Save"),
				 GTK_RESPONSE_REJECT);

	  gtk_dialog_set_default_response (GTK_DIALOG (dialog),
					   GTK_RESPONSE_REJECT);

	  response = gtk_dialog_run (GTK_DIALOG (dialog));

	  gtk_widget_destroy (dialog);

	  if (response == GTK_RESPONSE_REJECT)
	    return;
	}
    }

  /* Interestingly; we cannot use `path' after glade_project_reset_path
   * because we are getting called with glade_project_get_path (project) as an argument.
   */
  if (!do_save (window, project, path))
    return;

  /* Get display_name here, it could have changed with "Save As..." */
  display_name = glade_project_get_name (project);

  recent_add (window, glade_project_get_path (project));
  update_default_path (window, glade_project_get_path (project));

  /* refresh names */
  refresh_title (window);
  refresh_projects_list_item (window, project);
  refresh_stack_title_for_project (window, project);

  glade_util_flash_message (window->priv->statusbar,
                            window->priv->statusbar_actions_context_id,
                            _("Project '%s' saved"), display_name);

  g_free (display_name);
}

static gboolean
path_has_extension (const gchar *path)
{
  gchar *basename = g_path_get_basename (path);
  gboolean retval = g_utf8_strrchr (basename, -1, '.') != NULL;
  g_free (basename);
  return retval;
}

static void
save_as (GladeWindow *window)
{
  GladeProject *project, *another_project;
  GtkWidget *filechooser;
  GtkWidget *dialog;
  gchar *path = NULL;
  gchar *project_name;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  if (project == NULL)
    return;

  if (check_loading_project_for_save (project))
    return;

  filechooser = glade_util_file_dialog_new (_("Save As\342\200\246"), project,
                                            GTK_WINDOW (window),
                                            GLADE_FILE_DIALOG_ACTION_SAVE);

  if (glade_project_get_path (project))
    {
      gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (filechooser),
                                     glade_project_get_path (project));
    }
  else
    {
      gchar *default_path = g_strdup (get_default_path (window));
      if (default_path != NULL)
        {
          gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (filechooser),
                                               default_path);
          g_free (default_path);
        }

      project_name = glade_project_get_name (project);
      gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (filechooser),
                                         project_name);
      g_free (project_name);
    }

  while (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_OK)
    {
      path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));

      /* Check if selected filename has an extension or not */
      if (!path_has_extension (path))
        {
          gchar *real_path = g_strconcat (path, ".glade", NULL);

          g_free (path);
          path = real_path;

          /* We added .glade extension!,
           * check if file exist to avoid overwriting a file without asking
           */
          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              /* Set existing filename and let filechooser ask about overwriting */
              gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (filechooser), path);
              g_free (path);
              path = NULL;
              continue;
            }
        }
      break;
    }

  gtk_widget_destroy (filechooser);

  if (!path)
    return;

  /* checks if selected path is actually writable */
  if (glade_util_file_is_writeable (path) == FALSE)
    {
      dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                       GTK_DIALOG_MODAL,
                                       GTK_MESSAGE_ERROR,
                                       GTK_BUTTONS_OK,
                                       _("Could not save the file %s"),
                                       path);

      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						_("You do not have the permissions "
						  "necessary to save the file."));

      gtk_window_set_title (GTK_WINDOW (dialog), "");

      g_signal_connect_swapped (dialog, "response",
                                G_CALLBACK (gtk_widget_destroy), dialog);

      gtk_widget_show (dialog);
      g_free (path);
      return;
    }

  /* checks if another open project is using selected path */
  if ((another_project = glade_app_get_project_by_path (path)) != NULL)
    {
      if (project != another_project)
        {

          glade_util_ui_message (GTK_WIDGET (window),
                                 GLADE_UI_ERROR, NULL,
                                 _
                                 ("Could not save file %s. Another project with that path is open."),
                                 path);

          g_free (path);
          return;
        }

    }

  save (window, project, path);

  g_free (path);
}

static void
on_save_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *project;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  if (project == NULL)
    {
      /* Just in case the menu-item or button is not insensitive */
      glade_util_ui_message (GTK_WIDGET (window), GLADE_UI_WARN, NULL,
                             _("No open projects to save"));
      return;
    }

  if (glade_project_get_path (project) != NULL)
    {
      save (window, project, glade_project_get_path (project));
      return;
    }

  /* If instead we dont have a path yet, fire up a file selector */
  save_as (window);
}

static void
on_save_as_action_activate (GtkAction *action, GladeWindow *window)
{
  save_as (window);
}

static gboolean
confirm_close_project (GladeWindow *window, GladeProject *project)
{
  GtkWidget *dialog;
  gboolean close = FALSE;
  gchar *msg, *project_name = NULL;
  gint ret;

  project_name = glade_project_get_name (project);

  msg = g_strdup_printf (_("Save changes to project \"%s\" before closing?"),
                         project_name);

  dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                   GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_WARNING,
                                   GTK_BUTTONS_NONE, "%s", msg);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                            "%s", _("Your changes will be lost if you don't save them."));
  gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                          _("Close _without Saving"), GTK_RESPONSE_NO,
                          _("_Cancel"), GTK_RESPONSE_CANCEL,
                          _("_Save"), GTK_RESPONSE_YES, NULL);

  gtk_dialog_set_alternative_button_order (GTK_DIALOG (dialog),
                                           GTK_RESPONSE_YES,
                                           GTK_RESPONSE_CANCEL,
                                           GTK_RESPONSE_NO, -1);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_YES);

  ret = gtk_dialog_run (GTK_DIALOG (dialog));
  switch (ret)
    {
      case GTK_RESPONSE_YES:
        /* if YES we save the project: note we cannot use save_cb
         * since it saves the current project, while the modified 
         * project we are saving may be not the current one.
         */
        if (glade_project_get_path (project) != NULL)
          {
            close = do_save (window, project, glade_project_get_path (project));
          }
        else
          {
            GtkWidget *filechooser;
            gchar *path = NULL;
            gchar *default_path;

            filechooser =
                glade_util_file_dialog_new (_("Save\342\200\246"), project,
                                            GTK_WINDOW (window),
                                            GLADE_FILE_DIALOG_ACTION_SAVE);

            default_path = g_strdup (get_default_path (window));
            if (default_path != NULL)
              {
                gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER
                                                     (filechooser),
                                                     default_path);
                g_free (default_path);
              }

            gtk_file_chooser_set_current_name
                (GTK_FILE_CHOOSER (filechooser), project_name);


            if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_OK)
              path =
                  gtk_file_chooser_get_filename (GTK_FILE_CHOOSER
                                                 (filechooser));

            gtk_widget_destroy (filechooser);

            if (!path)
              break;

            save (window, project, path);

            g_free (path);

            close = FALSE;
          }
        break;
      case GTK_RESPONSE_NO:
        close = TRUE;
        break;
      case GTK_RESPONSE_CANCEL:
      case GTK_RESPONSE_DELETE_EVENT:
        close = FALSE;
        break;
      default:
        g_assert_not_reached ();
        close = FALSE;
    }

  g_free (msg);
  g_free (project_name);
  gtk_widget_destroy (dialog);

  return close;
}

static void
close_project (GladeWindow *window, GladeProject *project)
{
  GladeDesignView *view = glade_design_view_get_from_project (project);
  GladeWindowPrivate *priv = window->priv;

  /* Cancel any queued autosave activity */
  project_cancel_autosave (project);

  if (glade_project_is_loading (project))
    {
      glade_project_cancel_load (project);
      return;
    }

  g_signal_handlers_disconnect_by_func (project, project_notify_handler_cb, window);
  g_signal_handlers_disconnect_by_func (project, project_selection_changed_cb, window);
  g_signal_handlers_disconnect_by_func (project, project_targets_changed_cb, window);
  g_signal_handlers_disconnect_by_func (project, project_changed_cb, window);
  g_signal_handlers_disconnect_by_func (project, on_pointer_mode_changed, window);

  /* remove inspector first */
  gtk_container_remove (GTK_CONTAINER (priv->inspectors_stack),
                        g_object_get_data (G_OBJECT (view), "glade-window-view-inspector"));

  /* then the main view */
  gtk_container_remove (GTK_CONTAINER (priv->stack), GTK_WIDGET (view));

  clean_actions (window);

  /* Refresh the editor and some of the actions */
  project_selection_changed_cb (project, window);

  on_pointer_mode_changed (project, NULL, window);

  glade_app_remove_project (project);

  refresh_title (window);

  if (GLADE_WINDOW_ACTIVE_VIEW (window))
    set_sensitivity_according_to_project (window,
                                          glade_design_view_get_project
                                          (GLADE_WINDOW_ACTIVE_VIEW (window)));
  else
    gtk_action_group_set_sensitive (priv->project_actiongroup, FALSE);

  if (!glade_app_get_projects ())
    gtk_widget_hide (priv->center_paned);

  if (GLADE_WINDOW_ACTIVE_VIEW (window) == NULL)
    gtk_widget_hide (GTK_WIDGET (priv->editor));
}

static void
on_close_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeDesignView *view;
  GladeProject *project;
  gboolean close;

  view = GLADE_WINDOW_ACTIVE_VIEW (window);

  project = glade_design_view_get_project (view);

  if (view == NULL)
    return;

  if (glade_project_get_modified (project))
    {
      close = confirm_close_project (window, project);
      if (!close)
        return;
    }
  close_project (window, project);
}

static void
on_copy_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *project;

  if (!GLADE_WINDOW_ACTIVE_VIEW (window))
    return;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  glade_project_copy_selection (project);
}

static void
on_cut_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *project;

  if (!GLADE_WINDOW_ACTIVE_VIEW (window))
    return;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  glade_project_command_cut (project);
}

static void
on_paste_action_activate (GtkAction *action, GladeWindow *window)
{
  GtkWidget *placeholder;
  GladeProject *project;

  if (!GLADE_WINDOW_ACTIVE_VIEW (window))
    return;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));
  placeholder = glade_util_get_placeholder_from_pointer (GTK_CONTAINER (window));

  /* If this action is activated with a key binging (ctrl-v) the widget will be 
   * pasted over the placeholder below the default pointer.
   */
  glade_project_command_paste (project, placeholder ? GLADE_PLACEHOLDER (placeholder) : NULL);
}

static void
on_delete_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *project;

  if (!GLADE_WINDOW_ACTIVE_VIEW (window))
    return;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  glade_project_command_delete (project);
}

static void
stack_visible_child_next_prev (GladeWindow *window, gboolean next)
{
  GladeDesignView *view;
  GList *children, *node;

  if (!(view = GLADE_WINDOW_ACTIVE_VIEW (window)))
    return;

  children = gtk_container_get_children (GTK_CONTAINER (window->priv->stack));

  if ((node = g_list_find (children, view)) && 
      ((next && node->next) || (!next && node->prev)))
    gtk_stack_set_visible_child (window->priv->stack, 
                                 (next) ? node->next->data : node->prev->data);

  g_list_free (children);
}

static void
on_previous_project_action_activate (GtkAction *action, GladeWindow *window)
{
  stack_visible_child_next_prev (window, FALSE);
}

static void
on_next_project_action_activate (GtkAction *action, GladeWindow *window)
{
  stack_visible_child_next_prev (window, TRUE);
}

static void
on_properties_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *project;

  if (!GLADE_WINDOW_ACTIVE_VIEW (window))
    return;

  project = glade_design_view_get_project (GLADE_WINDOW_ACTIVE_VIEW (window));

  glade_project_properties (project);
}

static void
on_undo_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *active_project = get_active_project (window);

  if (!active_project)
    {
      g_warning ("undo should not be sensitive: we don't have a project");
      return;
    }

  glade_project_undo (active_project);
}

static void
on_redo_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeProject *active_project = get_active_project (window);

  if (!active_project)
    {
      g_warning ("redo should not be sensitive: we don't have a project");
      return;
    }

  glade_project_redo (active_project);
}

static void
doc_search_cb (GladeEditor *editor,
               const gchar *book,
               const gchar *page,
               const gchar *search,
               GladeWindow *window)
{
  glade_util_search_devhelp (book, page, search);
}

static void
on_stack_visible_child_notify (GObject    *gobject,
                               GParamSpec *pspec,
                               GladeWindow *window)
{
  GladeDesignView *view = GLADE_WINDOW_ACTIVE_VIEW (window);
  GladeWindowPrivate *priv = window->priv;

  if (view)
    {
      GladeProject *project = glade_design_view_get_project (view);

      /* switch to the project's inspector */
      gtk_stack_set_visible_child (priv->inspectors_stack,
                                   g_object_get_data (G_OBJECT (view), "glade-window-view-inspector"));

      glade_adaptor_chooser_set_project (priv->adaptor_chooser, project);

      set_sensitivity_according_to_project (window, project);

      refresh_undo_redo (window, project);

      /* Refresh the editor and some of the actions */
      project_selection_changed_cb (project, window);

      on_pointer_mode_changed (project, NULL, window);
    }

  refresh_title (window);
}

static void
on_open_recent_action_item_activated (GtkRecentChooser *chooser,
                                      GladeWindow *window)
{
  gchar *uri, *path;
  GError *error = NULL;

  uri = gtk_recent_chooser_get_current_uri (chooser);

  path = g_filename_from_uri (uri, NULL, NULL);
  if (error)
    {
      g_warning ("Could not convert uri \"%s\" to a local path: %s", uri,
                 error->message);
      g_error_free (error);
      return;
    }

  glade_window_open_project (window, path);

  g_free (uri);
  g_free (path);
}

static void
on_reference_action_activate (GtkAction *action, GladeWindow *window)
{
  if (glade_util_have_devhelp ())
    {
      glade_util_search_devhelp ("gladeui", NULL, NULL);
      return;
    }

  /* fallback to displaying online developer manual */
  glade_util_url_show (URL_DEVELOPER_MANUAL);
}

static void
on_preferences_action_activate (GtkAction   *action,
				GladeWindow *window)
{
  gtk_widget_show (GTK_WIDGET (window->priv->preferences));
}

static void
on_about_action_activate (GtkAction *action, GladeWindow *window)
{
  GladeWindowPrivate *priv = window->priv;
  
  gtk_about_dialog_set_version (GTK_ABOUT_DIALOG (priv->about_dialog), PACKAGE_VERSION);
  
  gtk_window_present (priv->about_dialog);
}

static void
menu_item_selected_cb (GtkWidget *item, GladeWindow *window)
{
  gchar *tooltip = gtk_widget_get_tooltip_text (item);

  if (tooltip)
    gtk_statusbar_push (GTK_STATUSBAR (window->priv->statusbar),
                        window->priv->statusbar_menu_context_id, tooltip);

  g_free (tooltip);
}

/* FIXME: GtkItem does not exist anymore? */
static void
menu_item_deselected_cb (gpointer item, GladeWindow * window)
{
  gtk_statusbar_pop (GTK_STATUSBAR (window->priv->statusbar),
                     window->priv->statusbar_menu_context_id);
}

static void
menu_item_connect (GtkWidget *item, GtkAction *action, GladeWindow *window)
{
  if (GTK_IS_MENU_ITEM (item))
    {
      if (action == NULL)
        action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (item));

      if (action)
        {
          gchar *tooltip;
          g_object_get (G_OBJECT (action), "tooltip", &tooltip, NULL);
          gtk_widget_set_tooltip_text (item, tooltip);
          /* yeah dont actually show tooltips, we use the to push them to the status bar */
          gtk_widget_set_has_tooltip (item, FALSE);
          g_free (tooltip);
          
        }

      g_signal_connect (item, "select",
                        G_CALLBACK (menu_item_selected_cb), window);
      g_signal_connect (item, "deselect",
                        G_CALLBACK (menu_item_deselected_cb), window);
    }
}

static void
menu_item_disconnect (GtkWidget *item, GladeWindow *window)
{
  if (GTK_IS_MENU_ITEM (item))
    {
      g_signal_handlers_disconnect_by_func
          (item, G_CALLBACK (menu_item_selected_cb), window);
      g_signal_handlers_disconnect_by_func
          (item, G_CALLBACK (menu_item_deselected_cb), window);
    }
}

static void
on_actiongroup_connect_proxy (GtkActionGroup *action_group,
                              GtkAction *action,
                              GtkWidget *proxy,
                              GladeWindow *window)
{
  menu_item_connect (proxy, action, window);
}

static void
on_actiongroup_disconnect_proxy (GtkActionGroup *action_group,
                                 GtkAction *action,
                                 GtkWidget *proxy,
                                 GladeWindow *window)
{
  menu_item_disconnect (proxy, window);
}

static void
on_recent_menu_insert (GtkMenuShell *menu_shell,
                       GtkWidget    *child,
                       gint          position,
                       GladeWindow *window)
{
  menu_item_connect (child, NULL, window);
}

static void
on_recent_menu_remove (GtkContainer *container,
                       GtkWidget *widget,
                       GladeWindow *window)
{
  menu_item_disconnect (widget, window);
}

static void
recent_menu_setup_callbacks (GtkWidget *menu, GladeWindow *window)
{
  GList *l, *list = gtk_container_get_children (GTK_CONTAINER (menu));

  for (l = list; l; l = g_list_next (l))
    menu_item_connect (l->data, NULL, window);

  g_list_free (list);
}

static void
action_group_setup_callbacks (GtkActionGroup *action_group,
                              GtkAccelGroup *accel_group,
                              GladeWindow *window)
{
  GList *l, *list = gtk_action_group_list_actions (action_group);

  for (l = list; l; l = g_list_next (l))
    {
      GtkAction *action = l->data;
      GSList *p, *proxies = gtk_action_get_proxies (action);
      gboolean is_recent = GTK_IS_RECENT_ACTION (action);

      /* Workaround for gtk+ bug #671786 */
      gtk_accel_group_connect_by_path (accel_group,
                                       gtk_action_get_accel_path (action),
                                       gtk_action_get_accel_closure (action));
      
      for (p = proxies; p; p = g_slist_next (p))
        {
          GtkWidget *submenu, *proxy = p->data;

          gtk_activatable_sync_action_properties (GTK_ACTIVATABLE (proxy),
                                                  action);

          menu_item_connect (proxy, action, window);

          if (is_recent && GTK_IS_MENU_ITEM (proxy) &&
              (submenu = gtk_menu_item_get_submenu (GTK_MENU_ITEM (proxy))))
            recent_menu_setup_callbacks (submenu, window);
        }
    }

  g_list_free (list);
}

enum
{
  TARGET_URI_LIST
};

static GtkTargetEntry drop_types[] = {
  {"text/uri-list", 0, TARGET_URI_LIST}
};

static void
drag_data_received (GtkWidget *widget,
                    GdkDragContext *context,
                    gint x, gint y,
                    GtkSelectionData *selection_data,
                    guint info,
                    guint time,
                    GladeWindow *window)
{
  gchar **uris, **str;
  const guchar *data;

  if (info != TARGET_URI_LIST)
    return;

  data = gtk_selection_data_get_data (selection_data);

  uris = g_uri_list_extract_uris ((gchar *) data);

  for (str = uris; *str; str++)
    {
      GError *error = NULL;
      gchar *path = g_filename_from_uri (*str, NULL, &error);

      if (path)
        {
          glade_window_open_project (window, path);
        }
      else
        {
          g_warning ("Could not convert uri to local path: %s", error->message);

          g_error_free (error);
	}
      g_free (path);
    }
  g_strfreev (uris);
}

static gboolean
delete_event (GtkWindow *w, GdkEvent *event, GladeWindow *window)
{
  gtk_action_activate (window->priv->quit_action);

  /* return TRUE to stop other handlers */
  return TRUE;
}

static void
on_selector_radioaction_changed (GtkRadioAction *action,
                                 GtkRadioAction *current,
                                 GladeWindow *window) 
{
  glade_project_set_pointer_mode (get_active_project (window),
                                  gtk_radio_action_get_current_value (current));
}

static void
add_project (GladeWindow *window, GladeProject *project, gboolean for_file)
{
  GladeWindowPrivate *priv = window->priv;
  GtkWidget *view, *inspector;

  g_return_if_fail (GLADE_IS_PROJECT (project));

  /* Create a new view for project */
  view = glade_design_view_new (project);

  gtk_widget_show (priv->center_paned);
  gtk_widget_show (GTK_WIDGET (priv->editor));

  g_signal_connect (G_OBJECT (project), "notify::modified",
                    G_CALLBACK (project_notify_handler_cb), window);
  g_signal_connect (G_OBJECT (project), "notify::path",
                    G_CALLBACK (project_notify_handler_cb), window);
  g_signal_connect (G_OBJECT (project), "notify::format",
                    G_CALLBACK (project_notify_handler_cb), window);
  g_signal_connect (G_OBJECT (project), "notify::has-selection",
                    G_CALLBACK (project_notify_handler_cb), window);
  g_signal_connect (G_OBJECT (project), "notify::read-only",
                    G_CALLBACK (project_notify_handler_cb), window);
  g_signal_connect (G_OBJECT (project), "notify::pointer-mode",
                    G_CALLBACK (on_pointer_mode_changed), window);
  g_signal_connect (G_OBJECT (project), "selection-changed",
                    G_CALLBACK (project_selection_changed_cb), window);
  g_signal_connect (G_OBJECT (project), "targets-changed",
                    G_CALLBACK (project_targets_changed_cb), window);
  g_signal_connect (G_OBJECT (project), "changed",
                    G_CALLBACK (project_changed_cb), window);

  /* create inspector */
  inspector = glade_inspector_new ();
  g_object_set_data (G_OBJECT (view), "glade-window-view-inspector", inspector);
  glade_inspector_set_project (GLADE_INSPECTOR (inspector), project);
  gtk_container_add (GTK_CONTAINER (priv->inspectors_stack), inspector);
  gtk_widget_show (inspector);

  set_sensitivity_according_to_project (window, project);
  refresh_title (window);

  gtk_action_group_set_sensitive (priv->project_actiongroup, TRUE);

  /* Pass ownership of the project to the app */
  glade_app_add_project (project);
  g_object_unref (project);


  /* Add view to stack */
  gtk_container_add (GTK_CONTAINER (priv->stack), view);
  gtk_widget_show (view);
  gtk_stack_set_visible_child (priv->stack, view);

  refresh_stack_title_for_project (window, project);
}

static void
on_registration_action_activate (GtkAction   *action,
                                 GladeWindow *window)
{
  gtk_window_present (GTK_WINDOW (window->priv->registration));
}

void
glade_window_new_project (GladeWindow *window)
{
  GladeProject *project;

  g_return_if_fail (GLADE_IS_WINDOW (window));

  project = glade_project_new ();
  if (!project)
    {
      glade_util_ui_message (GTK_WIDGET (window),
                             GLADE_UI_ERROR, NULL,
                             _("Could not create a new project."));
      return;
    }
  add_project (window, project, FALSE);
}

static gboolean
open_project (GladeWindow *window, const gchar *path)
{
  GladeProject *project;

  project = glade_project_new ();

  add_project (window, project, TRUE);
  update_default_path (window, path);

  if (!glade_project_load_from_file (project, path))
    {
      close_project (window, project);

      recent_remove (window, path);
      return FALSE;
    }

  /* increase project popularity */
  recent_add (window, glade_project_get_path (project));

  return TRUE;
}

static void
check_reload_project (GladeWindow *window, GladeProject *project)
{
  gchar *path;
  GtkWidget *dialog;
  GtkWidget *button;
  gint response;

  /* Reopen the project if it has external modifications.
   * Prompt for permission to reopen.
   */
  if ((glade_util_get_file_mtime (glade_project_get_path (project), NULL)
       <= glade_project_get_file_mtime (project)))
    {
      return;
    }

  if (glade_project_get_modified (project))
    {
      dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                       GTK_DIALOG_MODAL,
                                       GTK_MESSAGE_WARNING,
                                       GTK_BUTTONS_NONE,
                                       _("The project %s has unsaved changes"),
                                       glade_project_get_path (project));

      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                _
                                                ("If you reload it, all unsaved changes "
                                                 "could be lost. Reload it anyway?"));
    }
  else
    {
      dialog = gtk_message_dialog_new (GTK_WINDOW (window),
                                       GTK_DIALOG_MODAL,
                                       GTK_MESSAGE_WARNING,
                                       GTK_BUTTONS_NONE,
                                       _
                                       ("The project file %s has been externally modified"),
                                       glade_project_get_path (project));

      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
                                                _
                                                ("Do you want to reload the project?"));

    }

  gtk_window_set_title (GTK_WINDOW (dialog), "");

  button = gtk_button_new_with_mnemonic (_("_Reload"));
  gtk_button_set_image (GTK_BUTTON (button),
                        gtk_image_new_from_icon_name ("view-refresh",
                                                      GTK_ICON_SIZE_BUTTON));
  gtk_widget_show (button);

  gtk_dialog_add_button (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_REJECT);
  gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button,
                                GTK_RESPONSE_ACCEPT);
  gtk_dialog_set_alternative_button_order (GTK_DIALOG (dialog),
                                           GTK_RESPONSE_ACCEPT,
                                           GTK_RESPONSE_REJECT, -1);


  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_REJECT);

  response = gtk_dialog_run (GTK_DIALOG (dialog));

  gtk_widget_destroy (dialog);

  if (response == GTK_RESPONSE_REJECT)
    {
      return;
    }

  /* Reopen */
  path = g_strdup (glade_project_get_path (project));

  close_project (window, project);
  open_project (window, path);
  g_free (path);
}

/** 
 * glade_window_open_project: 
 * @window: a #GladeWindow
 * @path: the filesystem path of the project
 *
 * Opens a project file. If the project is already open, switch to that
 * project.
 * 
 * Returns: #TRUE if the project was opened
 */
gboolean
glade_window_open_project (GladeWindow *window, const gchar *path)
{
  GladeProject *project;

  g_return_val_if_fail (GLADE_IS_WINDOW (window), FALSE);
  g_return_val_if_fail (path != NULL, FALSE);

  /* dont allow a project to be opened twice */
  project = glade_app_get_project_by_path (path);
  if (project)
    {
      /* just switch to the project */
      switch_to_project (window, project);
      return TRUE;
    }
  else
    {
      return open_project (window, path);
    }
}

static void
glade_window_dispose (GObject *object)
{
  GladeWindow *window = GLADE_WINDOW (object);

  g_clear_object (&window->priv->app);
  g_clear_object (&window->priv->registration);

  G_OBJECT_CLASS (glade_window_parent_class)->dispose (object);
}

static void
glade_window_finalize (GObject *object)
{
  g_free (GLADE_WINDOW (object)->priv->default_path);

  G_OBJECT_CLASS (glade_window_parent_class)->finalize (object);
}


static gboolean
glade_window_configure_event (GtkWidget *widget, GdkEventConfigure *event)
{
  GladeWindow *window = GLADE_WINDOW (widget);
  gboolean retval;

  gboolean is_maximized;
  GdkWindow *gdk_window = gtk_widget_get_window (GTK_WIDGET (window));
  is_maximized = gdk_window_get_state (gdk_window) & GDK_WINDOW_STATE_MAXIMIZED;

  if (!is_maximized)
    {
      window->priv->position.width = event->width;
      window->priv->position.height = event->height;
    }

  retval =
      GTK_WIDGET_CLASS (glade_window_parent_class)->configure_event (widget,
                                                                     event);

  if (!is_maximized)
    {
      gtk_window_get_position (GTK_WINDOW (widget),
                               &window->priv->position.x,
                               &window->priv->position.y);
    }

  return retval;
}

static void
key_file_set_window_position (GKeyFile *config,
                              GdkRectangle *position,
                              const char *id,
                              gboolean maximized)
{
  char *key_x, *key_y, *key_width, *key_height, *key_maximized;

  key_x = g_strdup_printf ("%s-" CONFIG_KEY_X, id);
  key_y = g_strdup_printf ("%s-" CONFIG_KEY_Y, id);
  key_width = g_strdup_printf ("%s-" CONFIG_KEY_WIDTH, id);
  key_height = g_strdup_printf ("%s-" CONFIG_KEY_HEIGHT, id);
  key_maximized = g_strdup_printf ("%s-" CONFIG_KEY_MAXIMIZED, id);

  if (position->x > G_MININT)
    g_key_file_set_integer (config, CONFIG_GROUP_WINDOWS, key_x, position->x);
  if (position->y > G_MININT)
    g_key_file_set_integer (config, CONFIG_GROUP_WINDOWS, key_y, position->y);

  g_key_file_set_integer (config, CONFIG_GROUP_WINDOWS,
                          key_width, position->width);
  g_key_file_set_integer (config, CONFIG_GROUP_WINDOWS,
                          key_height, position->height);

  g_key_file_set_boolean (config, CONFIG_GROUP_WINDOWS,
                          key_maximized, maximized);


  g_free (key_maximized);
  g_free (key_height);
  g_free (key_width);
  g_free (key_y);
  g_free (key_x);
}

static void
save_windows_config (GladeWindow *window, GKeyFile *config)
{
  GladeWindowPrivate *priv = window->priv;
  GdkWindow *gdk_window;
  gboolean maximized;

  gdk_window = gtk_widget_get_window (GTK_WIDGET (window));
  maximized = gdk_window_get_state (gdk_window) & GDK_WINDOW_STATE_MAXIMIZED;

  key_file_set_window_position (config, &priv->position, "main", maximized);
}

static void
save_paned_position (GKeyFile *config, GtkWidget *paned, const gchar *name)
{
  g_key_file_set_integer (config, name, "position",
                          gtk_paned_get_position (GTK_PANED (paned)));
}

static void
glade_window_config_save (GladeWindow * window)
{
  GKeyFile *config = glade_app_get_config ();

  save_windows_config (window, config);

  /* Save main window paned positions */
  save_paned_position (config, window->priv->center_paned, "center_pane");
  save_paned_position (config, window->priv->left_paned, "left_pane");

  glade_preferences_save (window->priv->preferences, config);

  glade_app_config_save ();
}

static int
key_file_get_int (GKeyFile *config,
                  const char *group,
                  const char *key,
                  int default_value)
{
  if (g_key_file_has_key (config, group, key, NULL))
    return g_key_file_get_integer (config, group, key, NULL);
  else
    return default_value;
}

static void
key_file_get_window_position (GKeyFile *config,
                              const char *id,
                              GdkRectangle *pos,
                              gboolean *maximized)
{
  char *key_x, *key_y, *key_width, *key_height, *key_maximized;

  key_x = g_strdup_printf ("%s-" CONFIG_KEY_X, id);
  key_y = g_strdup_printf ("%s-" CONFIG_KEY_Y, id);
  key_width = g_strdup_printf ("%s-" CONFIG_KEY_WIDTH, id);
  key_height = g_strdup_printf ("%s-" CONFIG_KEY_HEIGHT, id);
  key_maximized = g_strdup_printf ("%s-" CONFIG_KEY_MAXIMIZED, id);

  pos->x = key_file_get_int (config, CONFIG_GROUP_WINDOWS, key_x, pos->x);
  pos->y = key_file_get_int (config, CONFIG_GROUP_WINDOWS, key_y, pos->y);
  pos->width =
      key_file_get_int (config, CONFIG_GROUP_WINDOWS, key_width, pos->width);
  pos->height =
      key_file_get_int (config, CONFIG_GROUP_WINDOWS, key_height, pos->height);

  if (maximized)
    {
      if (g_key_file_has_key
          (config, CONFIG_GROUP_WINDOWS, key_maximized, NULL))
        *maximized =
            g_key_file_get_boolean (config, CONFIG_GROUP_WINDOWS, key_maximized,
                                    NULL);
      else
        *maximized = FALSE;
    }

  g_free (key_x);
  g_free (key_y);
  g_free (key_width);
  g_free (key_height);
  g_free (key_maximized);
}

static void
load_paned_position (GKeyFile *config,
                     GtkWidget *pane,
                     const gchar *name,
                     gint default_position)
{
  gtk_paned_set_position (GTK_PANED (pane),
                          key_file_get_int (config, name, "position",
                                            default_position));
}

static gboolean
fix_paned_positions_idle (GladeWindow *window)
{
  /* When initially maximized/fullscreened we need to deffer this operation 
   */
  GKeyFile *config = glade_app_get_config ();

  load_paned_position (config, window->priv->left_paned, "left_pane", 200);
  load_paned_position (config, window->priv->center_paned, "center_pane", 400);

  return FALSE;
}

static void
glade_window_set_initial_size (GladeWindow *window, GKeyFile *config)
{
  GdkRectangle position = {
    G_MININT, G_MININT, GLADE_WINDOW_DEFAULT_WIDTH, GLADE_WINDOW_DEFAULT_HEIGHT
  };

  gboolean maximized;

  key_file_get_window_position (config, "main", &position, &maximized);
  if (maximized)
    {
      gtk_window_maximize (GTK_WINDOW (window));
      g_timeout_add (200, (GSourceFunc) fix_paned_positions_idle, window);
    }

  if (position.width <= 0 || position.height <= 0)
    {
      position.width = GLADE_WINDOW_DEFAULT_WIDTH;
      position.height = GLADE_WINDOW_DEFAULT_HEIGHT;
    }

  gtk_window_set_default_size (GTK_WINDOW (window), position.width,
                               position.height);

  if (position.x > G_MININT && position.y > G_MININT)
    gtk_window_move (GTK_WINDOW (window), position.x, position.y);
}

static void
glade_window_config_load (GladeWindow *window)
{
  GKeyFile *config = glade_app_get_config ();

  /* Initial main dimensions */
  glade_window_set_initial_size (window, config);

  /* Paned positions */
  load_paned_position (config, window->priv->left_paned, "left_pane", 200);
  load_paned_position (config, window->priv->center_paned, "center_pane", 400);
}

static void
on_quit_action_activate (GtkAction *action, GladeWindow *window)
{
  GList *list, *projects;

  projects = g_list_copy (glade_app_get_projects ());

  for (list = projects; list; list = list->next)
    {
      GladeProject *project = GLADE_PROJECT (list->data);

      if (glade_project_get_modified (project))
        {
          gboolean quit = confirm_close_project (window, project);
          if (!quit)
            {
              g_list_free (projects);
              return;
            }
        }
    }

  for (list = projects; list; list = list->next)
    {
      GladeProject *project = GLADE_PROJECT (glade_app_get_projects ()->data);
      close_project (window, project);
    }

  glade_window_config_save (window);

  g_list_free (projects);

  gtk_main_quit ();
}

static void
glade_window_init (GladeWindow *window)
{
  GladeWindowPrivate *priv;

  window->priv = priv = glade_window_get_instance_private (window);

  priv->default_path = NULL;

  /* Init preferences first, this has to be done before anything initializes
   * the real GladeApp, so that catalog paths are loaded correctly before we
   * continue.
   *
   * This should be fixed so that dynamic addition of catalogs at runtime
   * is supported.
   */
  priv->preferences = (GladePreferences *)glade_preferences_new ();
  glade_preferences_load (window->priv->preferences, glade_app_get_config ());
  
  /* We need this for the icons to be available */
  glade_init ();

  gtk_widget_init_template (GTK_WIDGET (window));

  gtk_box_set_homogeneous (GTK_BOX (priv->open_button_box), FALSE);

  priv->registration = glade_registration_new ();
}

static void
glade_window_constructed (GObject *object)
{
  GladeWindow *window = GLADE_WINDOW (object);
  GladeWindowPrivate *priv = window->priv;

  /* Chain up... */
  G_OBJECT_CLASS (glade_window_parent_class)->constructed (object);

  /* recent files */
  priv->recent_manager = gtk_recent_manager_get_default ();
  
  gtk_window_add_accel_group (GTK_WINDOW (window), priv->accelgroup);

  /* Action groups */
  action_group_setup_callbacks (priv->project_actiongroup, priv->accelgroup, window);
  action_group_setup_callbacks (priv->pointer_mode_actiongroup, priv->accelgroup, window);
  action_group_setup_callbacks (priv->static_actiongroup, priv->accelgroup, window);

  /* status bar */
  priv->statusbar_context_id = gtk_statusbar_get_context_id (GTK_STATUSBAR (priv->statusbar), "general");
  priv->statusbar_menu_context_id = gtk_statusbar_get_context_id (GTK_STATUSBAR (priv->statusbar), "menu");
  priv->statusbar_actions_context_id = gtk_statusbar_get_context_id (GTK_STATUSBAR (priv->statusbar), "actions");

  /* support for opening a file by dragging onto the project window */
  gtk_drag_dest_set (GTK_WIDGET (window),
                     GTK_DEST_DEFAULT_ALL,
                     drop_types, G_N_ELEMENTS (drop_types),
                     GDK_ACTION_COPY | GDK_ACTION_MOVE);

  g_signal_connect (G_OBJECT (window), "drag-data-received",
                    G_CALLBACK (drag_data_received), window);

  g_signal_connect (G_OBJECT (window), "delete-event",
                    G_CALLBACK (delete_event), window);

  /* GtkWindow events */
  g_signal_connect (G_OBJECT (window), "key-press-event",
                    G_CALLBACK (glade_utils_hijack_key_press), window);

  /* Load configuration, we need the list of extra catalog paths before creating
   * the GladeApp
   */
  glade_window_config_load (window);
  
  /* Create GladeApp singleton, this will load all catalogs */
  priv->app = glade_app_new ();
  glade_app_set_window (GTK_WIDGET (window));
  
  /* Clipboard signals */
  g_signal_connect (G_OBJECT (glade_app_get_clipboard ()),
                    "notify::has-selection",
                    G_CALLBACK (clipboard_notify_handler_cb), window);
  
#ifdef MAC_INTEGRATION
  {
    /* Fix up the menubar for MacOSX Quartz builds */
    GtkosxApplication *theApp = gtkosx_application_get ();
    GtkWidget *sep;

    gtk_widget_hide (priv->quit_menuitem);
    gtkosx_application_insert_app_menu_item (theApp, priv->about_menuitem, 0);
    sep = gtk_separator_menu_item_new();
    g_object_ref(sep);
    gtkosx_application_insert_app_menu_item (theApp, sep, 1);

    sep = gtk_separator_menu_item_new();
    g_object_ref(sep);
    gtkosx_application_insert_app_menu_item (theApp, sep, 2);

    g_signal_connect(theApp, "NSApplicationWillTerminate",
                     G_CALLBACK(on_quit_action_activate), window);

    gtkosx_application_ready (theApp);
  }
#endif
}

static void
glade_window_class_init (GladeWindowClass *klass)
{
  GObjectClass *object_class;
  GtkWidgetClass *widget_class;
  GtkCssProvider *provider;

  object_class = G_OBJECT_CLASS (klass);
  widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = glade_window_constructed;
  object_class->dispose = glade_window_dispose;
  object_class->finalize = glade_window_finalize;

  widget_class->configure_event = glade_window_configure_event;

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider,
                                   "@binding-set DisableBindings {\n"
                                   "  unbind \"<Control>s\";\n"
                                   "  unbind \"<Control>p\";\n"
                                   "  unbind \"<Control>w\";\n"
                                   "  unbind \"<Control>z\";\n"
                                   "  unbind \"<Control><shift>z\";\n"
                                   "  unbind \"<Control>x\";\n"
                                   "  unbind \"<Control>c\";\n"
                                   "  unbind \"<Control>v\";\n"
                                   "  unbind \"Delete\";\n"
                                   "  unbind \"<Control>Page_Up\";\n"
                                   "  unbind \"<Control>Page_Down\";\n"
                                   "  unbind \"<Control>Next\";\n"
                                   "  unbind \"<Control>n\";\n"
                                   "  unbind \"<Control>o\";\n"
                                   "  unbind \"<Control>q\";\n"
                                   "  unbind \"F1\";\n"
                                   "  unbind \"<Alt>0\";\n"
                                   "  unbind \"<Alt>1\";\n"
                                   "  unbind \"<Alt>2\";\n"
                                   "  unbind \"<Alt>3\";\n"
                                   "  unbind \"<Alt>4\";\n"
                                   "  unbind \"<Alt>5\";\n"
                                   "  unbind \"<Alt>6\";\n"
                                   "  unbind \"<Alt>7\";\n"
                                   "  unbind \"<Alt>8\";\n"
                                   "  unbind \"<Alt>9\";\n"
                                   "}\n"
                                   "GladeDesignView * {\n"
                                   "  -gtk-key-bindings: DisableBindings;\n"
                                   "}\n"
                                   ".glade-tight-fit {\n"
                                   "  margin: 0;\n"
                                   "  padding: 0;\n"
                                   "}", -1, NULL);

  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/glade/glade.glade");

  /* Internal children */
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, adaptor_chooser);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, headerbar);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, title);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, subtitle);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, project_button);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, about_dialog);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, center_paned);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, left_paned);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, open_button_box);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, stack);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, inspectors_stack);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, editor);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, statusbar);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, toolbar);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, undo_menu_button);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, redo_menu_button);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, accelgroup);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, project_actiongroup);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, pointer_mode_actiongroup);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, static_actiongroup);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, quit_menuitem);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, about_menuitem);

  /* Actions */
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, save_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, quit_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, undo_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, redo_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, cut_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, copy_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, paste_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, delete_action);
  gtk_widget_class_bind_template_child_private (widget_class, GladeWindow, selector_radioaction);

  /* Callbacks */
  gtk_widget_class_bind_template_callback (widget_class, on_open_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_save_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_save_as_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_close_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_copy_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_cut_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_paste_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_delete_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_previous_project_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_next_project_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_properties_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_undo_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_redo_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_quit_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_about_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_reference_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_preferences_action_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_registration_action_activate);

  gtk_widget_class_bind_template_callback (widget_class, on_open_recent_action_item_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_selector_radioaction_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_actiongroup_connect_proxy);
  gtk_widget_class_bind_template_callback (widget_class, on_actiongroup_disconnect_proxy);
  gtk_widget_class_bind_template_callback (widget_class, on_stack_visible_child_notify);
  gtk_widget_class_bind_template_callback (widget_class, on_recent_menu_insert);
  gtk_widget_class_bind_template_callback (widget_class, on_recent_menu_remove);
}


GtkWidget *
glade_window_new (void)
{
  return g_object_new (GLADE_TYPE_WINDOW, NULL);
}

void
glade_window_check_devhelp (GladeWindow *window)
{
  g_return_if_fail (GLADE_IS_WINDOW (window));

  if (glade_util_have_devhelp ())
    g_signal_connect (glade_app_get (), "doc-search", G_CALLBACK (doc_search_cb), window);
}

void
glade_window_registration_notify_user (GladeWindow *window)
{
  gboolean skip_reminder, completed;
  GladeWindowPrivate *priv;

  g_return_if_fail (GLADE_IS_WINDOW (window));
  priv = window->priv;

  g_object_get (priv->registration,
                "completed", &completed,
                "skip-reminder", &skip_reminder,
                NULL);
  
  if (!completed && !skip_reminder)
    {
      GtkWidget *dialog, *check;

      dialog = gtk_message_dialog_new (GTK_WINDOW (glade_app_get_window ()),
                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                       GTK_MESSAGE_QUESTION,
                                       GTK_BUTTONS_YES_NO,
                                       "%s",
                                       /* translators: Primary message of a dialog used to notify the user about the survey */
                                       _("We are conducting a user survey\n would you like to take it now?"));

      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
                                                /* translators: Secondary text of a dialog used to notify the user about the survey */
                                                _("If not, you can always find it in the Help menu."));

      check = gtk_check_button_new_with_mnemonic (_("_Do not show this dialog again"));
      gtk_box_pack_end (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                        check, FALSE, FALSE, 0);
      gtk_widget_set_halign (check, GTK_ALIGN_START);
      gtk_widget_set_margin_start (check, 6);
      gtk_widget_show (check);

      if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_YES)
          gtk_window_present (GTK_WINDOW (priv->registration));
      
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (check)))
        {
          g_object_set (priv->registration, "skip-reminder", TRUE, NULL);
          glade_app_config_save ();
        }

      gtk_widget_destroy (dialog);
    }
  else if (!completed)
    glade_util_flash_message (priv->statusbar, priv->statusbar_context_id, "%s",
                              /* translators: Text to show in the statusbar if the user did not completed the survey and choose not to show the notification dialog again */
                              _("Go to Help -> Registration & User Survey and complete our survey!"));
}
