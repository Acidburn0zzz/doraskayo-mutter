/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <config.h>

#include <gio/gio.h>
#include <gio/gunixfdmessage.h>

#include <clutter/clutter.h>
#include <clutter/evdev/clutter-evdev.h>

#include <glib.h>
#include <sys/time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "meta-weston-launch.h"

struct _MetaLauncherClass
{
  GObjectClass parent_class;

  void (*enter) (MetaLauncher *);
  void (*leave) (MetaLauncher *);
};

struct _MetaLauncher
{
  GObject parent;

  GSocket *weston_launch;
  int drm_fd;

  gboolean vt_switched;

  GMainContext *nested_context;
  GMainLoop *nested_loop;

  GSource *inner_source;
  GSource *outer_source;
};

enum {
  SIGNAL_ENTER,
  SIGNAL_LEAVE,
  SIGNAL_LAST
};

static int signals[SIGNAL_LAST];

G_DEFINE_TYPE (MetaLauncher, meta_launcher, G_TYPE_OBJECT);

static gboolean
send_message_to_wl (GSocket                *weston_launch,
		    void                   *message,
		    gsize                   size,
		    GSocketControlMessage  *out_cmsg,
		    GSocketControlMessage **in_cmsg,
		    GError                **error)
{
  int ok;
  GInputVector in_iov = { &ok, sizeof (int) };
  GOutputVector out_iov = { message, size };
  GSocketControlMessage *out_all_cmsg[2];
  GSocketControlMessage **in_all_cmsg;
  int flags = 0;
  int i;

  out_all_cmsg[0] = out_cmsg;
  out_all_cmsg[1] = NULL;
  if (g_socket_send_message (weston_launch, NULL,
			     &out_iov, 1,
			     out_all_cmsg, -1,
			     flags, NULL, error) != (gssize)size)
    return FALSE;

  if (g_socket_receive_message (weston_launch, NULL,
				&in_iov, 1,
				&in_all_cmsg, NULL,
				&flags, NULL, error) != sizeof (int))
    return FALSE;

  if (ok != 0)
    {
      if (ok == -1)
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		     "Got failure from weston-launch");
      else
	g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ok),
		     "Got failure from weston-launch: %s", strerror (-ok));

      for (i = 0; in_all_cmsg && in_all_cmsg[i]; i++)
	g_object_unref (in_all_cmsg[i]);
      g_free (in_all_cmsg);

      return FALSE;
    }

  if (in_all_cmsg && in_all_cmsg[0])
    {
      for (i = 1; in_all_cmsg[i]; i++)
	g_object_unref (in_all_cmsg[i]);
      *in_cmsg = in_all_cmsg[0];
    }

  g_free (in_all_cmsg);
  return TRUE;
}	    

gboolean
meta_launcher_set_master (MetaLauncher  *self,
			  gboolean       master,
			  GError       **error)
{
  struct weston_launcher_set_master message;
  GSocketControlMessage *cmsg;
  gboolean ok;

  message.header.opcode = WESTON_LAUNCHER_DRM_SET_MASTER;
  message.set_master = master;

  cmsg = g_unix_fd_message_new ();
  if (g_unix_fd_message_append_fd (G_UNIX_FD_MESSAGE (cmsg),
				   self->drm_fd, error) == FALSE)
    {
      g_object_unref (cmsg);
      return FALSE;
    }

  ok = send_message_to_wl (self->weston_launch, &message,
			   sizeof message, cmsg, NULL, error);

  g_object_unref (cmsg);
  return ok;
}

int
meta_launcher_open_input_device (MetaLauncher  *self,
				 const char    *name,
				 int            flags,
				 GError       **error)
{
  struct weston_launcher_open *message;
  GSocketControlMessage *cmsg;
  gboolean ok;
  gsize size;
  int *fds, n_fd;
  int ret;

  size = sizeof (struct weston_launcher_open) + strlen (name) + 1;
  message = g_malloc (size);
  message->header.opcode = WESTON_LAUNCHER_OPEN;
  message->flags = flags;
  strcpy (message->path, name);
  message->path[strlen(name)] = 0;

  ok = send_message_to_wl (self->weston_launch, message, size,
			   NULL, &cmsg, error);

  if (ok)
    {
      g_assert (G_IS_UNIX_FD_MESSAGE (cmsg));

      fds = g_unix_fd_message_steal_fds (G_UNIX_FD_MESSAGE (cmsg), &n_fd);
      g_assert (n_fd == 1);

      ret = fds[0];
      g_free (fds);
      g_object_unref (cmsg);
    }
  else
    ret = -1;

  g_free (message);
  return ret;
}

static void
meta_launcher_finalize (GObject *object)
{
  MetaLauncher *launcher = META_LAUNCHER (object);

  g_source_destroy (launcher->outer_source);
  g_source_destroy (launcher->inner_source);

  g_main_loop_unref (launcher->nested_loop);
  g_main_context_unref (launcher->nested_context);

  g_object_unref (launcher->weston_launch);

  G_OBJECT_CLASS (meta_launcher_parent_class)->finalize (object);
}

static void
meta_launcher_enter (MetaLauncher *launcher)
{
  GError *error;

  error = NULL;
  if (!meta_launcher_set_master (launcher, TRUE, &error))
    {
      g_critical ("Failed to become DRM master: %s", error->message);
      g_error_free (error);
    }

  clutter_evdev_reclaim_devices ();
}

static void
meta_launcher_leave (MetaLauncher *launcher)
{
  GError *error;

  error = NULL;
  if (!meta_launcher_set_master (launcher, FALSE, &error))
    {
      g_critical ("Failed to release DRM master: %s", error->message);
      g_error_free (error);
    }

  clutter_evdev_release_devices ();
}


static int
on_evdev_device_open (const char  *path,
		      int          flags,
		      gpointer     user_data,
		      GError     **error)
{
  MetaLauncher *launcher = user_data;

  return meta_launcher_open_input_device (launcher, path, flags, error);
}

static void
handle_vt_enter (MetaLauncher *launcher)
{
  g_assert (launcher->vt_switched);

  g_main_loop_quit (launcher->nested_loop);
}

static void
handle_request_vt_switch (MetaLauncher *launcher)
{
  struct weston_launcher_message message;
  GError *error;
  gboolean ok;

  g_signal_emit (launcher, signals[SIGNAL_LEAVE], 0);

  message.opcode = WESTON_LAUNCHER_CONFIRM_VT_SWITCH;

  error = NULL;
  ok = send_message_to_wl (launcher->weston_launch, &message,
			   sizeof (message), NULL, NULL, &error);
  if (!ok) {
    g_warning ("Failed to acknowledge VT switch: %s", error->message);
    g_error_free (error);

    return;
  }

  g_assert (!launcher->vt_switched);
  launcher->vt_switched = TRUE;

  /* We can't do anything at this point, because we don't
     have input devices and we don't have the DRM master,
     so let's run a nested busy loop until the VT is reentered */
  g_main_loop_run (launcher->nested_loop);

  g_assert (launcher->vt_switched);
  launcher->vt_switched = FALSE;

  g_signal_emit (launcher, signals[SIGNAL_ENTER], 0);
}

static void
on_socket_readable (GSocket      *socket,
		    GIOCondition  condition,
		    gpointer      user_data)
{
  MetaLauncher *launcher = user_data;
  struct weston_launcher_message header;
  gssize read;
  GError *error;

  if ((condition & G_IO_IN) == 0)
    return;

  error = NULL;
  read = g_socket_receive (socket, (char*)&header, sizeof(header), NULL, &error);
  if (read < (gssize)sizeof(header))
    {
      g_warning ("Error reading from weston-launcher socket: %s", error->message);
      g_error_free (error);
      return;
    }

  switch (header.opcode)
    {
    case WESTON_LAUNCHER_SERVER_REQUEST_VT_SWITCH:
      handle_request_vt_switch (launcher);
      break;

    case WESTON_LAUNCHER_SERVER_VT_ENTER:
      handle_vt_enter (launcher);
      break;
    }
}

static int
env_get_fd (const char *env)
{
  const char *value;

  value = g_getenv (env);

  if (value == NULL)
    return -1;
  else
    return g_ascii_strtoll (value, NULL, 10);
}

static void
meta_launcher_init (MetaLauncher *self)
{
  int launch_fd;

  launch_fd = env_get_fd ("WESTON_LAUNCHER_SOCK");
  if (launch_fd < 0)
    g_error ("Invalid mutter-launch socket");

  self->weston_launch = g_socket_new_from_fd (launch_fd, NULL);

  clutter_evdev_set_open_callback (on_evdev_device_open, self);

  self->nested_context = g_main_context_new ();
  self->nested_loop = g_main_loop_new (self->nested_context, FALSE);

  self->outer_source = g_socket_create_source (self->weston_launch, G_IO_IN, NULL);
  g_source_set_callback (self->outer_source, (GSourceFunc)on_socket_readable, self, NULL);
  g_source_attach (self->outer_source, NULL);
  g_source_unref (self->outer_source);

  self->inner_source = g_socket_create_source (self->weston_launch, G_IO_IN, NULL);
  g_source_set_callback (self->inner_source, (GSourceFunc)on_socket_readable, self, NULL);
  g_source_attach (self->inner_source, self->nested_context);
  g_source_unref (self->inner_source);
}

static void
meta_launcher_class_init (MetaLauncherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_launcher_finalize;

  klass->enter = meta_launcher_enter;
  klass->leave = meta_launcher_leave;

  signals[SIGNAL_ENTER] = g_signal_new ("enter",
					G_TYPE_FROM_CLASS (klass),
					G_SIGNAL_RUN_FIRST,
					G_STRUCT_OFFSET (MetaLauncherClass, enter),
					NULL, NULL, /* accumulator */
					g_cclosure_marshal_VOID__VOID,
					G_TYPE_NONE, 0);

  signals[SIGNAL_LEAVE] = g_signal_new ("leave",
					G_TYPE_FROM_CLASS (klass),
					G_SIGNAL_RUN_FIRST,
					G_STRUCT_OFFSET (MetaLauncherClass, leave),
					NULL, NULL, /* accumulator */
					g_cclosure_marshal_VOID__VOID,
					G_TYPE_NONE, 0);
}

MetaLauncher *
meta_launcher_new (void)
{
  return g_object_new (META_TYPE_LAUNCHER, NULL);
}

gboolean
meta_launcher_activate_vt (MetaLauncher  *launcher,
			   int            vt,
			   GError       **error)
{
  struct weston_launcher_activate_vt message;

  message.header.opcode = WESTON_LAUNCHER_ACTIVATE_VT;
  message.vt = vt;

  return send_message_to_wl (launcher->weston_launch, &message,
			     sizeof (message), NULL, NULL, error);
}

void
meta_launcher_set_drm_fd (MetaLauncher *launcher,
			  int           drm_fd)
{
  launcher->drm_fd = drm_fd;
}
