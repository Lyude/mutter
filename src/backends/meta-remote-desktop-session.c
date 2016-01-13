/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2015 Red Hat
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
 *
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include "backends/meta-remote-desktop-session.h"

#include <cogl/cogl.h>
#include <gst/gst.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

#include "backends/native/meta-backend-native.h"
#include "backends/x11/meta-backend-x11.h"
#include "backends/meta-remote-desktop-src.h"
#include "meta/meta-backend.h"
#include "meta/errors.h"
#include "meta-dbus-remote-desktop.h"

#define META_REMOTE_DESKTOP_SESSION_DBUS_PATH "/org/gnome/Mutter/RemoteDesktop/Session"

#define DEFAULT_FRAMERATE 30

enum
{
  STOPPED,

  LAST_SIGNAL
};

guint signals[LAST_SIGNAL];

typedef struct _MetaRemoteDesktopPipeline
{
  MetaRemoteDesktopSession *session;
  GstElement *pipeline;
} MetaRemoteDesktopPipeline;

struct _MetaRemoteDesktopSession
{
  MetaDBusRemoteDesktopSessionSkeleton parent;

  MetaRemoteDesktop *rd;

  MetaRemoteDesktopPipeline *pipeline;
  GstElement *src;
  char *stream_id;

  char *object_path;

  ClutterActor *stage;
  int width;
  int height;

  GstClockTime last_frame_time;

  struct {
    GHashTable *pressed_keysyms;

    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
  } keyboard;

  struct {
    int button_state;
  } pointer;
};

static void
meta_remote_desktop_session_init_iface (MetaDBusRemoteDesktopSessionIface *iface);

static void
meta_remote_desktop_pipeline_free (MetaRemoteDesktopPipeline *pipeline);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MetaRemoteDesktopPipeline,
                               meta_remote_desktop_pipeline_free);

G_DEFINE_TYPE_WITH_CODE (MetaRemoteDesktopSession,
                         meta_remote_desktop_session,
                         META_DBUS_TYPE_REMOTE_DESKTOP_SESSION_SKELETON,
                         G_IMPLEMENT_INTERFACE (META_DBUS_TYPE_REMOTE_DESKTOP_SESSION,
                                                meta_remote_desktop_session_init_iface));

static void
meta_remote_desktop_pipeline_free (MetaRemoteDesktopPipeline *pipeline)
{
  gst_object_unref (pipeline->pipeline);
  g_free (pipeline);
}

static void
meta_remote_desktop_session_pipeline_closed (MetaRemoteDesktopPipeline *pipeline)
{
  gst_element_set_state (pipeline->pipeline, GST_STATE_NULL);

  if (pipeline->session)
    pipeline->session->pipeline = NULL;

  meta_remote_desktop_pipeline_free (pipeline);
}

static gboolean
meta_remote_desktop_session_pipeline_bus_watch (GstBus     *bus,
                                                GstMessage *message,
                                                gpointer    data)
{
  MetaRemoteDesktopPipeline *pipeline = data;
  MetaRemoteDesktopSession *session;
  GError *error = NULL;

  switch (message->type)
    {
    case GST_MESSAGE_EOS:
      g_assert (!pipeline->session);

      meta_remote_desktop_session_pipeline_closed (pipeline);

      return FALSE;

    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      g_warning ("Error in remote desktop video pipeline: %s\n",
                 error->message);
      g_error_free (error);

      session = pipeline->session;
      meta_remote_desktop_session_pipeline_closed (pipeline);

      if (session)
        meta_remote_desktop_session_stop (session);

      return FALSE;

    default:
      break;
    }

  return TRUE;
}

static gboolean
meta_remote_desktop_session_add_source (MetaRemoteDesktopSession  *session,
                                        MetaRemoteDesktopPipeline *pipeline)
{
  g_autoptr(GstPad) sink_pad = NULL;
  g_autoptr(GstPad) src_pad = NULL;
  MetaRemoteDesktopSrc *src;

  sink_pad = gst_bin_find_unlinked_pad (GST_BIN (pipeline->pipeline),
                                        GST_PAD_SINK);
  if (sink_pad == NULL)
    {
      meta_warning ("MetaRemoteDesktop: pipeline has no unlinked sink pad\n");
      return FALSE;
    }

  src = meta_remote_desktop_src_new (DEFAULT_FRAMERATE,
                                     session->width,
                                     session->height);
  if (src == NULL)
    {
      meta_warning ("MetaRemoteDesktop: Can't create source element\n");
      return FALSE;
    }
  gst_bin_add (GST_BIN (pipeline->pipeline), GST_ELEMENT (src));

  src_pad = gst_element_get_static_pad (GST_ELEMENT (src), "src");
  if (!src_pad)
    {
      meta_warning ("MetaRemoteDesktop: can't get src pad link into pipeline\n");
      return FALSE;
    }

  if (gst_pad_link (src_pad, sink_pad) != GST_PAD_LINK_OK)
    {
      meta_warning ("MetaRemoteDesktop: can't link to sink pad\n");
      return FALSE;
    }

  session->src = GST_ELEMENT (src);

  return TRUE;
}

static gboolean
meta_remote_desktop_session_open_pipeline (MetaRemoteDesktopSession *session)
{
  g_autoptr(MetaRemoteDesktopPipeline) pipeline = NULL;
  GstBus *bus;
  GError *error = NULL;
  g_autofree char *stream_id = NULL;
  GstStructure *stream_properties;
  static unsigned int global_stream_id = 0;
  MetaDBusRemoteDesktopSession *dbus_session;

  pipeline = g_new0 (MetaRemoteDesktopPipeline, 1);

  pipeline->session = session;
  pipeline->pipeline = gst_pipeline_new (NULL);
  if (!pipeline->pipeline)
    {
      meta_warning ("MetaRemoteDesktop: Couldn't start pinos sink: %s\n",
                    error->message);
      return FALSE;
    }

  GstElement *pinossink = gst_element_factory_make ("pinossink", NULL);
  gst_bin_add (GST_BIN (pipeline->pipeline), pinossink);

  stream_id = g_strdup_printf ("%u", ++global_stream_id);
  stream_properties =
    gst_structure_new ("mutter/remote-desktop",
                       "gnome.remote_desktop.stream_id", G_TYPE_STRING, stream_id,
                       NULL);
  g_object_set (pinossink, "stream-properties", stream_properties, NULL);
  gst_structure_free (stream_properties);

  if (!meta_remote_desktop_session_add_source (session, pipeline))
    {
      meta_warning ("MetaRemoteDesktop: Couldn't add video source\n");
      return FALSE;
    }

  dbus_session = META_DBUS_REMOTE_DESKTOP_SESSION (session);
  meta_dbus_remote_desktop_session_set_pinos_stream_id (dbus_session,
                                                        stream_id);

  gst_element_set_state (pipeline->pipeline, GST_STATE_PLAYING);
  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline->pipeline));
  gst_bus_add_watch (bus,
                     meta_remote_desktop_session_pipeline_bus_watch,
                     pipeline);
  gst_object_unref (bus);

  session->pipeline = g_steal_pointer (&pipeline);
  session->stream_id = g_steal_pointer (&stream_id);

  return TRUE;
}

static void
meta_remote_desktop_session_close_pipeline (MetaRemoteDesktopSession *session)
{
  MetaRemoteDesktopPipeline *pipeline = session->pipeline;

  if (pipeline)
    {
      pipeline->session = NULL;
      session->pipeline = NULL;

      gst_element_send_event (pipeline->pipeline, gst_event_new_eos ());
    }
}

static void
meta_remote_desktop_session_record_frame (MetaRemoteDesktopSession *session,
                                          GstClockTime              now)
{
  GstBuffer *buffer;
  GstMapInfo map_info;
  size_t size;

  size = session->width * session->height * 4;

  /* TODO: Disable using hw planes if we rely on read_pixels. */

  buffer = meta_remote_desktop_try_create_tmpfile_gst_buffer (session->rd,
                                                              size);
  if (!buffer)
    {
      uint8_t *data;

      data = g_malloc (size);
      buffer = gst_buffer_new ();
      gst_buffer_insert_memory (buffer, -1,
                                gst_memory_new_wrapped (0, data, size, 0,
                                                        size, data, g_free));
    }

  gst_buffer_map (buffer, &map_info, GST_MAP_WRITE);

  cogl_framebuffer_read_pixels (cogl_get_draw_framebuffer (),
                                0,
                                0,
                                session->width,
                                session->height,
                                CLUTTER_CAIRO_FORMAT_ARGB32,
                                map_info.data);

  gst_buffer_unmap (buffer, &map_info);

  meta_remote_desktop_src_add_buffer (META_REMOTE_DESKTOP_SRC (session->src),
                                      buffer);
  gst_buffer_unref (buffer);
}

static void
meta_remote_desktop_session_on_stage_paint (ClutterActor             *actor,
                                            MetaRemoteDesktopSession *session)
{
  GstClock *clock;
  GstClockTime now;
  GstClockTime interval_threshold;

  g_return_if_fail (session->pipeline);

  clock = gst_element_get_clock (GST_ELEMENT (session->src));
  if (!clock)
    return;

  now = gst_clock_get_time (clock);

  /* Drop frames if the interval since the last frame is less than 75% of
   * the desired frame interval.
   */
  interval_threshold = gst_util_uint64_scale_int (GST_SECOND,
                                                  3,
                                                  4 * DEFAULT_FRAMERATE);
  if (GST_CLOCK_TIME_IS_VALID (session->last_frame_time) &&
      now - session->last_frame_time < interval_threshold)
    return;

  meta_remote_desktop_session_record_frame (session, now);
}

gboolean
meta_remote_desktop_session_start (MetaRemoteDesktopSession *session)
{
  if (!meta_remote_desktop_session_open_pipeline (session))
    return FALSE;

  g_signal_connect_after (session->stage, "paint",
                          G_CALLBACK (meta_remote_desktop_session_on_stage_paint),
                          session);
  clutter_actor_queue_redraw (CLUTTER_ACTOR (session->stage));

  return TRUE;
}

void
meta_remote_desktop_session_stop (MetaRemoteDesktopSession *session)
{
  meta_remote_desktop_session_close_pipeline (session);

  g_signal_handlers_disconnect_by_func (session->stage,
                                        (gpointer) meta_remote_desktop_session_on_stage_paint,
                                        session);

  g_signal_emit (session, signals[STOPPED], 0);
}

gboolean
meta_remote_desktop_session_is_running (MetaRemoteDesktopSession *session)
{
  return session->pipeline != NULL;
}

const char *
meta_remote_desktop_session_get_object_path (MetaRemoteDesktopSession *session)
{
  return session->object_path;
}

MetaRemoteDesktopSession *
meta_remote_desktop_session_new (MetaRemoteDesktop *rd)
{
  MetaRemoteDesktopSession *session;
  GDBusConnection *connection;
  GError *error = NULL;
  static unsigned int global_session_number = 0;

  session = g_object_new (META_TYPE_REMOTE_DESKTOP_SESSION, NULL);
  session->rd = rd;
  session->object_path =
    g_strdup_printf (META_REMOTE_DESKTOP_SESSION_DBUS_PATH "/u%u",
                     ++global_session_number);

  connection =
    g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (rd));
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session),
                                         connection,
                                         session->object_path,
                                         &error))
    {
      meta_warning ("Failed to export session object: %s\n", error->message);
      return NULL;
    }

  return session;
}

static gboolean
handle_stop (MetaDBusRemoteDesktopSession *skeleton,
             GDBusMethodInvocation        *invocation)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);

  fprintf (stderr, "RD: stop\n");

  if (meta_remote_desktop_session_is_running (session))
    meta_remote_desktop_session_stop (session);

  meta_dbus_remote_desktop_session_complete_stop (skeleton, invocation);

  return TRUE;
}

static void
force_pick_actor_for_event (ClutterEvent *event,
                            ClutterStage *stage,
                            double        x,
                            double        y)
{
  ClutterActor *actor;

  /* The compositor may not be active; and as such may not have a stage assigned
   * to the core devices. This makes the clutter event processing to not pick
   * an actor for the event, which in effect causes the event to be dropped.
   */
  actor = clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_REACTIVE, x, y);
  clutter_event_set_source (event, actor);
}

static guint32
get_current_device_time (MetaBackend *backend)
{
  /* Determine the expected time space the input events are expected to be in.
   * This depends on the clutter backend, and the clutter backend depends on the
   * mutter backend, so lets check that.
   */
  if (META_IS_BACKEND_NATIVE (backend))
    {
      guint64 time_us = g_get_monotonic_time ();
      return (guint32) (time_us / 1000);
    }
  else if (META_IS_BACKEND_X11 (backend))
    {
      return meta_display_get_current_time_roundtrip (meta_get_display ());
    }

  g_assert_not_reached ();
}

static void
destroy_keyboard_state (MetaRemoteDesktopSession *session)
{
  g_clear_pointer (&session->keyboard.xkb_state, xkb_state_unref);
}

static void
init_keyboard_state (MetaRemoteDesktopSession *session)
{
  MetaBackend *backend = meta_get_backend ();

  session->keyboard.pressed_keysyms = g_hash_table_new_full (g_str_hash,
                                                             g_str_equal,
                                                             g_free,
                                                             NULL);
  session->keyboard.xkb_keymap = meta_backend_get_keymap (backend);
  session->keyboard.xkb_state = xkb_state_new (session->keyboard.xkb_keymap);
}

typedef struct
{
  xkb_keysym_t in_keysym;
  struct xkb_state *in_xkb_state;

  xkb_keycode_t out_keycode;
} FindKeycodeData;

static void
find_keycode_iter (struct xkb_keymap *keymap,
                   xkb_keycode_t      keycode,
                   void              *data)
{
  FindKeycodeData *find_data = data;
  const xkb_keysym_t *syms = NULL;
  int num_syms, i;

  if (find_data->out_keycode != XKB_KEYCODE_INVALID)
    return;

  num_syms = xkb_state_key_get_syms (find_data->in_xkb_state, keycode, &syms);
  for (i = 0; i < num_syms; i++)
    {
      if (syms[i] == find_data->in_keysym)
        {
          find_data->out_keycode = keycode;
          break;
        }
    }
}

static void
notify_key_event (MetaRemoteDesktopSession *session,
                  xkb_keycode_t             keycode,
                  xkb_keysym_t              keysym,
                  enum xkb_key_direction    direction)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterDeviceManager *device_manager = clutter_device_manager_get_default ();
  ClutterInputDevice *device;
  ClutterStage *stage;
  ClutterEvent *event;
  ClutterPoint point = { 0 };
  char buffer[8];
  int n;

  stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  device = clutter_device_manager_get_core_device (device_manager,
                                                   CLUTTER_KEYBOARD_DEVICE);

  if (direction == XKB_KEY_DOWN)
    event = clutter_event_new (CLUTTER_KEY_PRESS);
  else
    event = clutter_event_new (CLUTTER_KEY_RELEASE);

  event->key.device = device;
  event->key.stage = stage;
  event->key.time = get_current_device_time (backend);
  event->key.hardware_keycode = keycode;
  event->key.keyval = keysym;

  n = xkb_keysym_to_utf8 (keysym, buffer, sizeof buffer);
  if (n == 0)
    {
      event->key.unicode_value = (gunichar) '\0';
    }
  else
    {
      event->key.unicode_value = g_utf8_get_char_validated (buffer, n);
      if (event->key.unicode_value == (gunichar) -1 ||
          event->key.unicode_value == (gunichar) -2)
        event->key.unicode_value = (gunichar) '\0';
    }

  /* FIXME: We can't currently set the full state of the event, including
   * the current modifier and button state. This causes grabs in mutter to
   * fail, meaning moving/resizing windows for example won't work.
   */

  clutter_input_device_get_coords (device, NULL, &point);
  force_pick_actor_for_event (event, stage, point.x, point.y);

  clutter_do_event (event);
  clutter_event_free (event);
}

static gboolean
handle_notify_keyboard_keysym (MetaDBusRemoteDesktopSession *skeleton,
                               GDBusMethodInvocation        *invocation,
                               guint                         keysym,
                               gboolean                      state)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);
  xkb_keycode_t keycode;
  enum xkb_key_direction direction;
  FindKeycodeData find_data;

  /* FIXME: This event will mess up key tracking if keys from the remote
   * session are pressed at the same time as keys from the real clutter backend.
   * Needs new compositor specific clutter API to fix.
   */

  find_data = (FindKeycodeData) {
    .in_keysym = keysym,
    .in_xkb_state = session->keyboard.xkb_state,

    .out_keycode = XKB_KEYCODE_INVALID,
  };
  xkb_keymap_key_for_each (session->keyboard.xkb_keymap,
                           find_keycode_iter,
                           &find_data);
  keycode = find_data.out_keycode;

  if (keycode == XKB_KEYCODE_INVALID)
    {
      char keysym_name[255];

      xkb_keysym_get_name (keysym, keysym_name, sizeof keysym_name);
      meta_warning ("MetaRemoteDesktop: Didn't find keycode for keysym '%s'\n",
                    keysym_name);
      return TRUE;
    }

  direction = state ? XKB_KEY_DOWN : XKB_KEY_UP;
  xkb_state_update_key (session->keyboard.xkb_state, keycode, direction);

  notify_key_event (session, keycode, keysym, direction);

  meta_dbus_remote_desktop_session_complete_notify_keyboard_keysym (skeleton,
                                                                    invocation);
  return TRUE;
}

/* Translation taken from the clutter evdev backend. */
static gint
translate_to_clutter_button (gint button)
{
  switch (button)
    {
    case BTN_LEFT:
      return CLUTTER_BUTTON_PRIMARY;
    case BTN_RIGHT:
      return CLUTTER_BUTTON_SECONDARY;
    case BTN_MIDDLE:
      return CLUTTER_BUTTON_MIDDLE;
    default:
      /* For compatibility reasons, all additional buttons go after the old 4-7
       * scroll ones.
       */
      return button - (BTN_LEFT - 1) + 4;
    }
}

static gboolean
handle_notify_pointer_button (MetaDBusRemoteDesktopSession *skeleton,
                              GDBusMethodInvocation        *invocation,
                              gint                          button,
                              gboolean                      state)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (skeleton);
  MetaBackend *backend = meta_get_backend ();
  ClutterDeviceManager *device_manager = clutter_device_manager_get_default ();
  ClutterInputDevice *device;
  ClutterEvent *event;
  ClutterStage *stage;
  ClutterPoint point = { 0 };
  static gint maskmap[8] =
    {
      CLUTTER_BUTTON1_MASK, CLUTTER_BUTTON3_MASK, CLUTTER_BUTTON2_MASK,
      CLUTTER_BUTTON4_MASK, CLUTTER_BUTTON5_MASK, 0, 0, 0
    };

  /* FIXME: This event is incomplete, and will cause issues, as it misses the
   * XKB state, will mess up button count assumptions that the clutter evdev
   * backend otherwise takes care of, and will not know about the actual pointer
   * position. New compositor specific clutter API is needed for this to work
   * properly.
   */

  stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  device = clutter_device_manager_get_core_device (device_manager,
                                                   CLUTTER_POINTER_DEVICE);

  if (state)
    event = clutter_event_new (CLUTTER_BUTTON_PRESS);
  else
    event = clutter_event_new (CLUTTER_BUTTON_RELEASE);

  event->button.time = get_current_device_time (backend);
  event->button.stage = stage;
  event->button.button = translate_to_clutter_button (button);
  clutter_event_set_device (event, device);

  if (button >= BTN_LEFT && button < BTN_LEFT + (int) G_N_ELEMENTS (maskmap))
    {
      if (state)
        session->pointer.button_state |= maskmap[button - BTN_LEFT];
      else
        session->pointer.button_state &= ~maskmap[button - BTN_LEFT];
    }

  /* FIXME: We don't know the internal device state, so lets pretend it's the
   * one exposed by ClutterInputDevice. Needs compositor specific clutter API to
   * be fixed. */
  clutter_input_device_get_coords (device, NULL, &point);
  event->button.x = point.x;
  event->button.y = point.y;

  /* FIXME: We can't currently set the full state of the event, including
   * the current modifier and button state. This causes grabs in mutter to
   * fail, meaning moving/resizing windows for example won't work.
   */

  force_pick_actor_for_event (event, stage, point.x, point.y);

  clutter_do_event (event);
  clutter_event_free (event);

  meta_dbus_remote_desktop_session_complete_notify_pointer_button (skeleton,
                                                                   invocation);

  return TRUE;
}

static ClutterScrollDirection
discrete_steps_to_scroll_direction (guint axis,
                                    gint  steps)
{
  if (axis == 0 && steps < 0)
    return CLUTTER_SCROLL_UP;
  if (axis == 0 && steps > 0)
    return CLUTTER_SCROLL_DOWN;
  if (axis == 1 && steps < 0)
    return CLUTTER_SCROLL_LEFT;
  if (axis == 1 && steps > 0)
    return CLUTTER_SCROLL_RIGHT;

  g_assert_not_reached ();
}

static gboolean
handle_notify_pointer_axis_discrete (MetaDBusRemoteDesktopSession *skeleton,
                                     GDBusMethodInvocation        *invocation,
                                     guint                         axis,
                                     gint                          steps)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterDeviceManager *device_manager = clutter_device_manager_get_default ();
  ClutterInputDevice *device;
  ClutterEvent *event;
  ClutterStage *stage;
  ClutterPoint point = { 0 };

  if (axis <= 1)
    {
      meta_warning ("MetaRemoteDesktop: Invalid pointer axis\n");
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid axis value");
      return TRUE;
    }

  if (steps == 0)
    {
      meta_warning ("MetaRemoteDesktop: Invalid axis steps value\n");
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Invalid axis steps value");
      return TRUE;
    }

  if (steps != -1 || steps != 1)
    meta_warning ("Multiple steps at the same time not yet implemented, treating as one.\n");

  /* FIXME: This event is incomplete, and will cause issues, as it misses the
   * XKB state doesn't know the actual pointer position. New compositor specific
   * clutter API is needed for this to work properly.
   */

  stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  device = clutter_device_manager_get_core_device (device_manager,
                                                   CLUTTER_POINTER_DEVICE);

  event = clutter_event_new (CLUTTER_SCROLL);

  event->scroll.time = get_current_device_time (backend);
  event->scroll.stage = stage;
  event->scroll.direction = discrete_steps_to_scroll_direction (axis, steps);
  clutter_event_set_device (event, device);

  /* FIXME: We don't know the internal device state, so lets pretend it's the
   * one exposed by ClutterInputDevice. Needs compositor specific clutter API to
   * be fixed. */
  clutter_input_device_get_coords (device, NULL, &point);
  event->scroll.x = point.x;
  event->scroll.y = point.y;

  force_pick_actor_for_event (event, stage, point.x, point.y);

  clutter_do_event (event);
  clutter_event_free (event);

  meta_dbus_remote_desktop_session_complete_notify_pointer_axis_discrete (skeleton,
                                                                          invocation);

  return TRUE;
}

static gboolean
handle_notify_pointer_motion_absolute (MetaDBusRemoteDesktopSession *skeleton,
                                       GDBusMethodInvocation        *invocation,
                                       gdouble                       x,
                                       gdouble                       y)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterDeviceManager *device_manager = clutter_device_manager_get_default ();
  ClutterInputDevice *device;
  ClutterEvent *event;
  ClutterStage *stage;

  /* FIXME: This event is incomplete, as it misses the XKB state. New compositor
   * specific clutter API is needed for this to work properly.
   */

  stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  device = clutter_device_manager_get_core_device (device_manager,
                                                   CLUTTER_POINTER_DEVICE);

  event = clutter_event_new (CLUTTER_MOTION);
  event->motion.time = get_current_device_time (backend);
  event->motion.stage = stage;
  event->motion.x = x;
  event->motion.y = y;
  clutter_event_set_device (event, device);

  force_pick_actor_for_event (event, stage, x, y);

  clutter_do_event (event);
  clutter_event_free (event);

  meta_dbus_remote_desktop_session_complete_notify_pointer_motion_absolute (skeleton,
                                                                            invocation);

  return TRUE;
}

static void
meta_remote_desktop_session_init_iface (MetaDBusRemoteDesktopSessionIface *iface)
{
  iface->handle_stop = handle_stop;
  iface->handle_notify_keyboard_keysym = handle_notify_keyboard_keysym;
  iface->handle_notify_pointer_button = handle_notify_pointer_button;
  iface->handle_notify_pointer_axis_discrete = handle_notify_pointer_axis_discrete;
  iface->handle_notify_pointer_motion_absolute = handle_notify_pointer_motion_absolute;
}

static void
on_keymap_changed (MetaBackend *backend,
                   gpointer     data)
{
  MetaRemoteDesktopSession *session = data;

  xkb_keymap_unref (session->keyboard.xkb_keymap);
  session->keyboard.xkb_keymap =
    xkb_keymap_ref (meta_backend_get_keymap (backend));
}

static void
on_keymap_layout_group_changed (MetaBackend *backend,
                                guint        idx,
                                gpointer     data)
{
  MetaRemoteDesktopSession *session = data;
  xkb_mod_mask_t depressed_mods;
  xkb_mod_mask_t latched_mods;
  xkb_mod_mask_t locked_mods;
  struct xkb_state *state;

  state = session->keyboard.xkb_state;

  depressed_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_DEPRESSED);
  latched_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_LATCHED);
  locked_mods = xkb_state_serialize_mods (state, XKB_STATE_MODS_LOCKED);

  xkb_state_update_mask (state,
                         depressed_mods, latched_mods, locked_mods,
                         0, 0, idx);
}

static void
meta_remote_desktop_session_init (MetaRemoteDesktopSession *session)
{
  MetaBackend *backend = meta_get_backend ();
  ClutterActor *stage = meta_backend_get_stage (backend);
  ClutterActorBox allocation;

  session->stage = stage;
  clutter_actor_get_allocation_box (session->stage, &allocation);
  session->width = (int)(0.5 + allocation.x2 - allocation.x1);
  session->height = (int)(0.5 + allocation.y2 - allocation.y1);

  session->last_frame_time = GST_CLOCK_TIME_NONE;

  init_keyboard_state (session);

  g_signal_connect_object (backend, "keymap-changed",
                           G_CALLBACK (on_keymap_changed), session,
                           0);
  g_signal_connect_object (backend, "keymap-layout-group-changed",
                           G_CALLBACK (on_keymap_layout_group_changed), session,
                           0);
}

static void
meta_remote_desktop_session_finalize (GObject *object)
{
  MetaRemoteDesktopSession *session = META_REMOTE_DESKTOP_SESSION (object);

  g_assert (!meta_remote_desktop_session_is_running (session));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session));

  g_free (session->stream_id);
  g_free (session->object_path);

  destroy_keyboard_state (session);

  G_OBJECT_CLASS (meta_remote_desktop_session_parent_class)->finalize (object);
}

static void
meta_remote_desktop_session_class_init (MetaRemoteDesktopSessionClass *klass)
{

  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_remote_desktop_session_finalize;

  signals[STOPPED] = g_signal_new ("stopped",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}
