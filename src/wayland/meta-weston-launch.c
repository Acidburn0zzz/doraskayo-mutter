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
#include <clutter/egl/clutter-egl.h>
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

#include "meta-wayland-private.h"
#include "meta-cursor-tracker-private.h"
#include "meta-weston-launch.h"

struct _MetaLauncher
{
  GSocket *weston_launch;
  GSource *weston_launch_source;

  gboolean vt_switched;
};

static void handle_request_vt_switch (MetaLauncher *self);

static gboolean
request_vt_switch_idle (gpointer user_data)
{
  handle_request_vt_switch (user_data);

  return FALSE;
}

static gboolean
send_message_to_wl (MetaLauncher           *self,
		    void                   *message,
		    gsize                   size,
		    GSocketControlMessage  *out_cmsg,
		    GSocketControlMessage **in_cmsg,
		    GError                **error)
{
  struct weston_launcher_reply reply;
  GInputVector in_iov = { &reply, sizeof (reply) };
  GOutputVector out_iov = { message, size };
  GSocketControlMessage *out_all_cmsg[2];
  GSocketControlMessage **in_all_cmsg;
  int flags = 0;
  int i;

  out_all_cmsg[0] = out_cmsg;
  out_all_cmsg[1] = NULL;
  if (g_socket_send_message (self->weston_launch, NULL,
			     &out_iov, 1,
			     out_all_cmsg, -1,
			     flags, NULL, error) != (gssize)size)
    return FALSE;

  if (g_socket_receive_message (self->weston_launch, NULL,
				&in_iov, 1,
			        &in_all_cmsg, NULL,
				&flags, NULL, error) != sizeof (reply))
    return FALSE;

  while (reply.header.opcode != ((struct weston_launcher_message*)message)->opcode)
    {
      /* There were events queued */
      g_assert ((reply.header.opcode & WESTON_LAUNCHER_EVENT) == WESTON_LAUNCHER_EVENT);

      /* This can never happen, because the only time mutter-launch can queue
         this event is after confirming a VT switch, and we don't make requests
         during that time.

         Note that getting this event would be really bad, because we would be
         in the wrong loop/context.
      */
      g_assert (reply.header.opcode != WESTON_LAUNCHER_SERVER_VT_ENTER);

      switch (reply.header.opcode)
	{
	case WESTON_LAUNCHER_SERVER_REQUEST_VT_SWITCH:
	  g_idle_add (request_vt_switch_idle, self);
	  break;

	default:
	  g_assert_not_reached ();
	}

      if (g_socket_receive_message (self->weston_launch, NULL,
				    &in_iov, 1,
				    NULL, NULL,
				    &flags, NULL, error) != sizeof (reply))
	return FALSE;
    }

  if (reply.ret != 0)
    {
      if (reply.ret == -1)
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		     "Got failure from weston-launch");
      else
	g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-reply.ret),
		     "Got failure from weston-launch: %s", strerror (-reply.ret));

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

static int
meta_launcher_open_device (MetaLauncher  *self,
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

  ok = send_message_to_wl (self, message, size, NULL, &cmsg, error);

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
meta_launcher_enter (MetaLauncher *launcher)
{
  ClutterBackend *backend;
  CoglContext *cogl_context;
  CoglDisplay *cogl_display;

  backend = clutter_get_default_backend ();
  cogl_context = clutter_backend_get_cogl_context (backend);
  cogl_display = cogl_context_get_display (cogl_context);
  cogl_kms_display_queue_modes_reset (cogl_display);

  clutter_evdev_reclaim_devices ();

  {
    MetaWaylandCompositor *compositor = meta_wayland_compositor_get_default ();

    /* When we mode-switch back, we need to immediately queue a redraw
     * in case nothing else queued one for us, and force the cursor to
     * update. */

    clutter_actor_queue_redraw (compositor->stage);
    meta_cursor_tracker_force_update (compositor->seat->cursor_tracker);
  }
}

static void
meta_launcher_leave (MetaLauncher *launcher)
{
  clutter_evdev_release_devices ();
}

static int
on_evdev_device_open (const char  *path,
		      int          flags,
		      gpointer     user_data,
		      GError     **error)
{
  MetaLauncher *launcher = user_data;

  return meta_launcher_open_device (launcher, path, flags, error);
}

static void
on_evdev_device_close (int          fd,
                       gpointer     user_data)
{
  close (fd);
}

static void
handle_vt_enter (MetaLauncher *launcher)
{
  g_assert (launcher->vt_switched);
  launcher->vt_switched = FALSE;

  meta_launcher_enter (launcher);
}

static void
handle_request_vt_switch (MetaLauncher *launcher)
{
  struct weston_launcher_message message;
  GError *error;
  gboolean ok;

  meta_launcher_leave (launcher);

  message.opcode = WESTON_LAUNCHER_CONFIRM_VT_SWITCH;

  error = NULL;
  ok = send_message_to_wl (launcher, &message, sizeof (message), NULL, NULL, &error);
  if (!ok) {
    g_warning ("Failed to acknowledge VT switch: %s", error->message);
    g_error_free (error);
    return;
  }

  g_assert (!launcher->vt_switched);
  launcher->vt_switched = TRUE;
}

static gboolean
on_socket_readable (GSocket      *socket,
		    GIOCondition  condition,
		    gpointer      user_data)
{
  MetaLauncher *launcher = user_data;
  struct weston_launcher_event event;
  gssize read;
  GError *error;

  if ((condition & G_IO_IN) == 0)
    return TRUE;

  error = NULL;
  read = g_socket_receive (socket, (char*)&event, sizeof(event), NULL, &error);
  if (read < (gssize)sizeof(event))
    {
      g_warning ("Error reading from weston-launcher socket: %s", error->message);
      g_error_free (error);
      return TRUE;
    }

  switch (event.header.opcode)
    {
    case WESTON_LAUNCHER_SERVER_REQUEST_VT_SWITCH:
      handle_request_vt_switch (launcher);
      break;

    case WESTON_LAUNCHER_SERVER_VT_ENTER:
      handle_vt_enter (launcher);
      break;
    }

  return TRUE;
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

MetaLauncher *
meta_launcher_new (void)
{
  MetaLauncher *self = g_slice_new0 (MetaLauncher);
  int launch_fd;

  launch_fd = env_get_fd ("WESTON_LAUNCHER_SOCK");
  if (launch_fd < 0)
    g_error ("Invalid mutter-launch socket");

  self->weston_launch = g_socket_new_from_fd (launch_fd, NULL);

  self->weston_launch_source = g_socket_create_source (self->weston_launch, G_IO_IN, NULL);
  g_source_set_callback (self->weston_launch_source, (GSourceFunc)on_socket_readable, self, NULL);
  g_source_attach (self->weston_launch_source, NULL);
  g_source_unref (self->weston_launch_source);

  clutter_evdev_set_device_callbacks (on_evdev_device_open,
                                      on_evdev_device_close,
                                      self);

#if defined(CLUTTER_WINDOWING_EGL)
  if (clutter_check_windowing_backend (CLUTTER_WINDOWING_EGL))
    {
      GError *error = NULL;
      int fd = meta_launcher_open_device (self, "/dev/dri/card0", O_RDWR, &error);
      if (error)
        g_error ("Failed to open /dev/dri/card0: %s", error->message);

      clutter_egl_set_kms_fd (fd);
    }
#endif

  return self;
}

void
meta_launcher_free (MetaLauncher *launcher)
{
  g_source_destroy (launcher->weston_launch_source);
  g_object_unref (launcher->weston_launch);
  g_slice_free (MetaLauncher, launcher);
}

gboolean
meta_launcher_activate_vt (MetaLauncher  *launcher,
			   signed char    vt,
			   GError       **error)
{
  struct weston_launcher_activate_vt message;

  message.header.opcode = WESTON_LAUNCHER_ACTIVATE_VT;
  message.vt = vt;

  return send_message_to_wl (launcher, &message, sizeof (message), NULL, NULL, error);
}

