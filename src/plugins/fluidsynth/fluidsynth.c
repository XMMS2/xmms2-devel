/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2023 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  ---------------------------------
 *
 *  FluidSynth plugin for XMMS2
 *
 *  audio/rawmidi format (this plugin's input data)
 *
 *  This format is standard MIDI track data (as you would find in an MTrk chunk
 *  in a .mid file) but with no headers.  The initial tempo is set in the
 *  track metadata.
 *
 *  This allows support for streaming MIDI data, at the cost of only supporting
 *  a single track.  If you want to play multitrack MIDI data, convert it
 *  to audio/miditracks instead, and the midsquash plugin will take care of
 *  converting that to a single track.
 *
 *  Note that meta-event 0x2F is normally used to end a track.  This event will
 *  cause the song to finish, so you probably don't want to send it in the
 *  middle of a song.  (The midsquash plugin removes these events when
 *  merging multiple tracks into a single track.)
 */

#include <xmms/xmms_xformplugin.h>
#include <xmms/xmms_sample.h>
#include <xmms/xmms_medialib.h>
#include <xmms/xmms_log.h>
#include <fluidsynth.h>

#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>

/* Fill up the buffer with MIDI events every 500 milliseconds */
#define CALLBACK_FREQ 500

/* FluidSynth uses floating point samples internally.  Uncomment this to pass
 * those samples to XMMS2 unchanged.  Comment it out to have FluidSynth convert
 * the samples to signed 16-bit before passing them to XMMS2.
 *
 * Floating point will become the default if/when all xform plugins support it.
 */
/*#define FLUIDSYNTH_USE_FLOAT*/

/*
 * Type definitions
 */
typedef struct xmms_fluidsynth_data_St {
	fluid_synth_t *synth;
	fluid_settings_t *settings;
	fluid_sequencer_t *sequencer;
	short synthseq_id, callback_id;
	GArray *soundfont_id; /* int */
	guint num_soundfonts;
	gboolean end_of_song;
	guint trailing_samples;
	GString *comment;
	gboolean has_title;

	gint32 ticks_per_quarter_note;
	gulong us_per_quarter_note;
	gulong now;     /* Time of last event in microseconds since synth start */

	gulong delay;   /* Current time (in MIDI delta ticks) until next event */

	guchar prev_event; /* Last command (for MIDI "running status") */
} xmms_fluidsynth_data_t;

/*
 * Function prototypes
 */
static gboolean xmms_fluidsynth_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gboolean xmms_fluidsynth_init (xmms_xform_t *xform);
static void xmms_fluidsynth_destroy (xmms_xform_t *xform);
static gint xmms_fluidsynth_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err);
static gint64 xmms_fluidsynth_seek (xmms_xform_t *xform, gint64 samples, xmms_xform_seek_mode_t whence, xmms_error_t *err);
static void xmms_fluidsynth_sf_config_changed (xmms_object_t *obj, xmmsv_t *_value, gpointer udata);
static void xmms_fluidsynth_config_changed (xmms_object_t *obj, xmmsv_t *_value, gpointer udata);
static void xmms_fluidsynth_skip_bytes (xmms_xform_t *xform, guint count);
static gboolean xmms_fluidsynth_get_byte (xmms_xform_t *xform, guchar *val);
static gboolean xmms_fluidsynth_read_midi_num (xmms_xform_t *xform, gulong *val, guchar *firstbyte);
static void xmms_fluidsynth_callback (unsigned int time, fluid_event_t *event, fluid_sequencer_t *seq, void *vdata);
static void xmms_fluidsynth_sched_callback (xmms_fluidsynth_data_t *data, unsigned int callback_date);
static void xmms_fluidsynth_set_metadata (xmms_xform_t *xform, const gchar *metakey, const gchar *text, guint len);

static const struct {
	const gchar *key;
	const gchar *value;
} config_params[] = {
	{"sample-rate", "48000"},
	{"encoding", "ISO8859-1"}
};

/*
 * Plugin header
 */
XMMS_XFORM_PLUGIN_DEFINE ("fluidsynth",
                          "FluidSynth synthesiser ",
                          XMMS_VERSION,
                          "MIDI synthesiser",
                          xmms_fluidsynth_plugin_setup);

static gboolean
xmms_fluidsynth_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;
	xmms_config_property_t *cfgv;
	gchar key[64];
	int i;

	XMMS_XFORM_METHODS_INIT (methods);
	methods.init = xmms_fluidsynth_init;
	methods.destroy = xmms_fluidsynth_destroy;
	methods.read = xmms_fluidsynth_read;
	methods.seek = xmms_fluidsynth_seek;

	xmms_xform_plugin_methods_set (xform_plugin, &methods);

	/*
	xmms_plugin_info_add (plugin, "URL", "http://www.xmms.org/");
	xmms_plugin_info_add (plugin, "Author", "Adam Nielsen <malvineous@shikadi.net>");
	xmms_plugin_info_add (plugin, "License", "LGPL");
	*/

	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/rawmidi",
	                              XMMS_STREAM_TYPE_END);

	for (i = 0; i < G_N_ELEMENTS (config_params); i++) {
		xmms_xform_plugin_config_property_register (xform_plugin,
		                                            config_params[i].key,
		                                            config_params[i].value,
		                                            NULL, NULL);
	}

	/* Set up the soundfont.0 config option and the function to call when its
	 * value is changed.
	 */
	cfgv = xmms_xform_plugin_config_property_register (xform_plugin,
	                                                   "soundfont.0",
	                                                   "/path/to/your-soundfont-goes-here.sf2",
	                                                   xmms_fluidsynth_sf_config_changed,
	                                                   xform_plugin);

	/* Look for >= soundfont.1 (which have been loaded from the config file) and
	 * set up the callback function on those.
	 */
	for (i = 1; ; i++) {
		g_snprintf (key, sizeof (key), "soundfont.%i", i);

		cfgv = xmms_xform_plugin_config_lookup (xform_plugin, key);
		if (!cfgv) {
			break;
		}

		xmms_config_property_callback_set (cfgv,
		                                   xmms_fluidsynth_sf_config_changed,
		                                   xform_plugin);
	}

	/* Make sure the last SoundFont entry is empty so there's always room to add
	 * another SoundFont.
	 */
	xmms_fluidsynth_sf_config_changed (NULL, NULL, xform_plugin);

	return TRUE;
}

static gboolean
xmms_fluidsynth_init (xmms_xform_t *xform)
{
	xmms_fluidsynth_data_t *data;
	xmms_config_property_t *cfgv;
	double samplerate;
	gint i, id;
	gchar key[64];
	const gchar *soundfont_path;

	g_return_val_if_fail (xform, FALSE);

	data = g_new0 (xmms_fluidsynth_data_t, 1);
	g_return_val_if_fail (data, FALSE);
	xmms_xform_private_data_set (xform, data);

	if (!xmms_xform_auxdata_get_int (xform, "tempo", &data->ticks_per_quarter_note)) {
		XMMS_DBG ("xform auxdata value 'tempo' not set (bug in previous xform plugin)");
		g_free (data);
		return FALSE;
	}

	data->settings = new_fluid_settings ();

	/* Set up the current configuration.  This needs to happen *after* the
	 * FluidSynth settings instance has been created because the callback
	 * function uses it directly.  This also has nothing to do with the
	 * dynamic SoundFont options (e.g. soundfont.1)
	 */
	for (i = 0; i < G_N_ELEMENTS (config_params); i++) {
		cfgv = xmms_xform_config_lookup (xform, config_params[i].key);
		xmms_config_property_callback_set (cfgv,
		                                   xmms_fluidsynth_config_changed,
		                                   data);

		xmms_fluidsynth_config_changed (XMMS_OBJECT (cfgv), NULL, data);
	}

	fluid_settings_getnum (data->settings, "synth.sample-rate", &samplerate);

	xmms_xform_outdata_type_add (xform,
	                             XMMS_STREAM_TYPE_MIMETYPE,
	                             "audio/pcm",
	                             XMMS_STREAM_TYPE_FMT_FORMAT,
#ifdef FLUIDSYNTH_USE_FLOAT
	                             XMMS_SAMPLE_FORMAT_FLOAT,
#else
	                             XMMS_SAMPLE_FORMAT_S16,
#endif
	                             XMMS_STREAM_TYPE_FMT_CHANNELS,
	                             2,
	                             XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                             (gint)samplerate,
	                             XMMS_STREAM_TYPE_END);

	data->synth = new_fluid_synth (data->settings);
	data->sequencer = new_fluid_sequencer2 (0);
	data->synthseq_id = fluid_sequencer_register_fluidsynth (data->sequencer,
	                                                         data->synth);
	data->callback_id = fluid_sequencer_register_client (data->sequencer, "xmms2",
	                                                     xmms_fluidsynth_callback,
	                                                     xform);

	/* Scan through all the SoundFonts that have been set and load them all */
	data->soundfont_id = g_array_new (FALSE, FALSE, sizeof (int));
	for (i = 0; ; i++) {
		g_snprintf (key, sizeof (key), "soundfont.%i", i);

		cfgv = xmms_xform_config_lookup (xform, key);
		if (!cfgv) {
			/* No more SoundFonts to load */
			break;
		}

		soundfont_path = xmms_config_property_get_string (cfgv);
		if (!soundfont_path[0]) {
			/* Ignore blank entries (i.e. the last one) */
			continue;
		}

		id = fluid_synth_sfload (data->synth, soundfont_path, 1);
		if (id == FLUID_FAILED) {
			/* Since this is the last thing to allocate, just call the destroy
			 * function instead of duplicating all that code as cleanup here.
			 */
			xmms_fluidsynth_destroy (xform);

			xmms_log_error ("Unable to open SoundFont: %s", soundfont_path);
			return FALSE;
		} else {
			g_array_append_val (data->soundfont_id, id);
		}
	}

	/* So that the song doesn't get cut off early, after the MIDI data runs out
	 * wait for four times the callback interval for the notes to finish playing.
	 * 2x the interval is enough for the last buffered event to be processed,
	 * then another 2x will give about one second on top of that.
	 */
	data->trailing_samples = samplerate * (CALLBACK_FREQ / 250);

	/* Default tempo is 500,000us (0.5 sec) per quarter-note */
	data->us_per_quarter_note = 500000;

	data->comment = NULL;
	data->has_title = FALSE;
	data->end_of_song = FALSE;

	data->now = fluid_sequencer_get_tick (data->sequencer);
	xmms_fluidsynth_sched_callback (data, data->now);

	return TRUE;
}

static void
xmms_fluidsynth_destroy (xmms_xform_t *xform)
{
	xmms_config_property_t *cfgv;
	xmms_fluidsynth_data_t *data;
	gint i;

	g_return_if_fail (xform);

	data = xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	for (i = data->soundfont_id->len; i > 0; i--) {
		fluid_synth_sfunload (data->synth,
		                      g_array_index (data->soundfont_id, int, i - 1),
		                      FALSE);
	}

	g_array_free (data->soundfont_id, TRUE);

	if (data->sequencer)
		delete_fluid_sequencer (data->sequencer);

	if (data->synth)
		delete_fluid_synth (data->synth);

	if (data->comment)
		g_string_free (data->comment, TRUE);

	/* Remove callbacks from the FluidSynth options (sample rate etc.) */
	for (i = 0; i < G_N_ELEMENTS (config_params); i++) {
		cfgv = xmms_xform_config_lookup (xform, config_params[i].key);

		xmms_config_property_callback_remove (cfgv,
		                                      xmms_fluidsynth_config_changed,
		                                      data);
	}

	g_free (data);
}

static gint
xmms_fluidsynth_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err)
{
	xmms_fluidsynth_data_t *data;
	gint status;
	gint sample_size;

#ifdef FLUIDSYNTH_USE_FLOAT
	sample_size = xmms_sample_size_get (XMMS_SAMPLE_FORMAT_FLOAT) * 2;
#else
	sample_size = xmms_sample_size_get (XMMS_SAMPLE_FORMAT_U16) * 2;
#endif

	data = xmms_xform_private_data_get (xform);
	if (data->end_of_song) {
		/* The song is ended, but don't stop too abruptly! */
		if (data->trailing_samples == 0) return 0;
		data->trailing_samples -= MIN (data->trailing_samples, len / sample_size);
	}

#ifdef FLUIDSYNTH_USE_FLOAT
	status = fluid_synth_write_float (
#else
	status = fluid_synth_write_s16 (
#endif
	                                data->synth, len / sample_size,
	                                buf, 0, 2,  /* Left channel (off 0 incr 2) */
	                                buf, 1, 2   /* Right channel (off 1 incr 2) */
	);

	return (status == FLUID_OK) ? len : 0;
}

static gint64
xmms_fluidsynth_seek (xmms_xform_t *xform, gint64 samples,
                  xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	g_return_val_if_fail (xform, FALSE);
	g_return_val_if_fail (whence == XMMS_XFORM_SEEK_SET, -1);

	if (samples == 0) {
		/* Seek to start, we can do that */
		xmms_xform_seek(xform, 0, XMMS_XFORM_SEEK_SET, err);
		return 0;
	}

	return -1;
}

static void
xmms_fluidsynth_sf_config_changed (xmms_object_t *obj, xmmsv_t *_value, gpointer udata)
{
	xmms_config_property_t *cfgv;
	xmms_xform_plugin_t *xform_plugin = (xmms_xform_plugin_t *) udata;
	const gchar *soundfont_path = "";
	gchar key[64];
	gint i;

	g_return_if_fail (xform_plugin);

	/* Figure out how many entries exist, and if the last one is not blank, add
	 * a blank one to allow another SoundFont to be added.
	 */
	for (i = 0; ; i++) {
		g_snprintf (key, sizeof (key), "soundfont.%i", i);

		cfgv = xmms_xform_plugin_config_lookup (xform_plugin, key);
		if (!cfgv) {
			/* No more entries, soundfont_path is the content of the previous entry
			 * (the last one in the list.)
			 */
			if (soundfont_path[0] != 0) {
				/* It had a SoundFont set, so create a new empty one */
				xmms_xform_plugin_config_property_register (xform_plugin,
				                                            key, "",
				                                            xmms_fluidsynth_sf_config_changed,
				                                            xform_plugin);
			}
			break;
		}

		soundfont_path = xmms_config_property_get_string (cfgv);
	}
}

static void
xmms_fluidsynth_config_changed (xmms_object_t *obj, xmmsv_t *_value, gpointer udata)
{
	xmms_fluidsynth_data_t *data;
	const gchar *name;
	const gchar *str_value;
	gint int_value;
	gdouble dbl_value;
	int type;

	data = (xmms_fluidsynth_data_t *) udata;
	g_return_if_fail (data);

	name = xmms_config_property_get_name ((xmms_config_property_t *) obj);

	/* Ignore these non-FluidSynth options, they are looked up as needed */
	if (strcmp (name, "fluidsynth.encoding") == 0) {
		return;
	}

	/* XMMS settings are "fluidsynth.blah" so chop off the first five chars and
	 * they become FluidSynth options like "synth.blah"
	 */
	name = &name[5];

	type = fluid_settings_get_type (data->settings, name);
	switch (type) {
		case FLUID_INT_TYPE:
			int_value = xmms_config_property_get_int ((xmms_config_property_t *) obj);
			fluid_settings_setint (data->settings, name, int_value);
			break;
		case FLUID_STR_TYPE:
			str_value = xmms_config_property_get_string ((xmms_config_property_t *) obj);
			fluid_settings_setstr (data->settings, name, str_value);
			break;
		case FLUID_NUM_TYPE: /* double */
			dbl_value = xmms_config_property_get_float ((xmms_config_property_t *) obj);
			fluid_settings_setnum (data->settings, name, dbl_value);
			break;
		case FLUID_SET_TYPE: /* set of values */
			XMMS_DBG ("Unsupported data type for FluidSynth config value %s", name);
			break;
		case FLUID_NO_TYPE:  /* undefined */
			XMMS_DBG ("Invalid FluidSynth config option %s", name);
			break;
	}

}

static void
xmms_fluidsynth_skip_bytes (xmms_xform_t *xform, guint count)
{
	xmms_error_t error;
	gint ret;
	guchar dummy[16];
	while (count > 0) {
		ret = xmms_xform_read (xform, dummy, MIN (count, sizeof (dummy)), &error);
		if (ret < 1) break;
		count -= ret;
	}
	return;
}

static gboolean
xmms_fluidsynth_get_byte (xmms_xform_t *xform, guchar *val)
{
	xmms_error_t error;
	gint ret;
	ret = xmms_xform_read (xform, val, 1, &error);
	if (ret <= 0) return FALSE;
	return TRUE;
}

/* Read a variable-length MIDI number.  If firstbyte is non-NULL it is used
 * as if it was the next byte read out of the file.
 */
static gboolean
xmms_fluidsynth_read_midi_num (xmms_xform_t *xform, gulong *val,
                               guchar *firstbyte)
{
	gint i;
	guchar buf;

	*val = 0;
	for (i = 0; i < 4; i++) {
		if (firstbyte) {
			buf = *firstbyte;
			firstbyte = NULL;
		} else {
			if (!xmms_fluidsynth_get_byte (xform, &buf))
				return FALSE;
		}
		*val |= buf & 0x7F;
		if (buf & 0x80) *val <<= 7;
		else break;
	}

	return TRUE;
}

/* Callback when the MIDI buffer gets low and needs more notes */
static void
xmms_fluidsynth_callback (unsigned int time, fluid_event_t *event,
                          fluid_sequencer_t *seq, void *vdata)
{
	const gchar *metakey;
	xmms_error_t error;
	gint i;
	guchar midi_event;
	xmms_xform_t *xform;
	xmms_fluidsynth_data_t *data;
	fluid_event_t *evt;
	guchar event_data[2];
	gulong delay;

	xform = (xmms_xform_t *) vdata;
	g_return_if_fail (xform);

	data = xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	evt = new_fluid_event ();
	fluid_event_set_source (evt, -1);
	fluid_event_set_dest (evt, data->synthseq_id);

	while (!(data->end_of_song = !xmms_fluidsynth_read_midi_num (xform, &delay, NULL))) {
		gint chan;

		/* Read MIDI event */
		data->end_of_song = !xmms_fluidsynth_get_byte (xform, &midi_event);
		if (data->end_of_song) break;

		if (!(midi_event & 0x80)) {
			/* This is a running-status byte */
			event_data[0] = midi_event;
			midi_event = data->prev_event;
		} else {
			if ((midi_event & 0xF0) != 0xF0) {
				/* Meta events (0xF0 through 0xFF) can appear in the middle of
				 * running-status data without affecting the running status, so we
				 * only update the 'last event' if this isn't a meta-event.
				 */
				data->prev_event = midi_event;
			}
			if (!xmms_fluidsynth_get_byte (xform, &event_data[0]))
				break;
		}

		chan = midi_event & 0x0F;
		gboolean send_event = FALSE;
		switch (midi_event & 0xF0) {
			case 0x80: /* Note off (two bytes) */
				/* FIXME: Release velocity unused */
				fluid_event_noteoff (evt, chan, event_data[0]);
				xmms_fluidsynth_skip_bytes (xform, 1);
				send_event = TRUE;
				break;
			case 0x90:  /* Note on (two bytes) */
				if (!xmms_fluidsynth_get_byte (xform, &event_data[1]))
					break;
				fluid_event_noteon (evt, chan, event_data[0], event_data[1]);
				send_event = TRUE;
				break;
			case 0xA0:  /* Key pressure (two bytes) */
				/* TODO: Does FluidSynth support this? */
				xmms_fluidsynth_skip_bytes (xform, 1);
				break;
			case 0xB0:  /* Controller change (two bytes) */
				if (!xmms_fluidsynth_get_byte (xform, &event_data[1]))
					break;
				fluid_event_control_change (evt, chan, event_data[0], event_data[1]);
				send_event = TRUE;
				break;
			case 0xC0:  /* Instrument change (one byte) */
				fluid_event_program_change (evt, chan, event_data[0]);
				send_event = TRUE;
				break;
			case 0xD0:  /* Channel pressure (one byte) */
				fluid_event_channel_pressure (evt, chan, event_data[0]);
				send_event = TRUE;
				break;
			case 0xE0:  /* Pitch bend (two bytes) */
				if (!xmms_fluidsynth_get_byte (xform, &event_data[1]))
					break;
				fluid_event_pitch_bend (evt, chan, (event_data[1] << 7) | event_data[0]);
				send_event = TRUE;
				break;
			case 0xF0: {
				if (midi_event == 0xFF) { /* Meta-event */
					gulong len;
					guchar meta_event = event_data[0];
					if (!xmms_fluidsynth_read_midi_num (xform, &len, NULL))
						break;
					switch (meta_event) {
						case 0x01: { /* Text event */
							gint orig_size;
							gboolean first_comment = FALSE;

							if (!data->comment) {
								data->comment = g_string_sized_new (len);
								first_comment = TRUE;
							} else {
								g_string_append (data->comment, ", ");
							}

							orig_size = data->comment->len;
							g_string_set_size (data->comment, orig_size + len);
							xmms_xform_read (xform, &data->comment->str[orig_size], len, &error);

							/* Ensure trailing NULL */
							g_string_set_size (data->comment, orig_size + len);
							XMMS_DBG ("Found text event: %s", &data->comment->str[orig_size]);

							if (first_comment) {
								if (data->comment->len > 0) {
									xmms_fluidsynth_set_metadata (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_TITLE, data->comment->str, data->comment->len);
								} /* else ignore blank fields, but treat them as used */
								data->has_title = TRUE;
							} else {
								xmms_fluidsynth_set_metadata (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_COMMENT, data->comment->str, data->comment->len);
							}
							break;
						}
						case 0x02: { /* Copyright notice */
							GString *cp = g_string_sized_new (len);
							xmms_xform_read (xform, cp->str, len, &error);
							g_string_set_size (cp, len);
							XMMS_DBG ("Found copyright notice: %s", cp->str);

							xmms_fluidsynth_set_metadata (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_COPYRIGHT, cp->str, cp->len);
							break;
						}
						case 0x03: { /* Song title */
							GString *title = g_string_sized_new (len);
							xmms_xform_read (xform, title->str, len, &error);
							/* Ensure trailing NULL */
							g_string_set_size (title, len);
							XMMS_DBG ("Found title event: %s", title->str);

							/* Only set the first event as the title, but because these
							 * events are used for track names it can be overridden by
							 * a text event (meta event 0x01) if there is one.
							 */
							if (!data->has_title) {
								if (title->len > 0) {
									xmms_fluidsynth_set_metadata (xform, XMMS_MEDIALIB_ENTRY_PROPERTY_TITLE, title->str, title->len);
								} /* else ignore blank fields, but treat them as used */
								data->has_title = TRUE;
							}
							break;
						}
						case 0x51: { /* Set tempo */
							gulong bpm;

							data->us_per_quarter_note = 0;
							for (i = 0; i < len; i++) {
								if (!xmms_fluidsynth_get_byte (xform, &event_data[1]))
									break;
								data->us_per_quarter_note <<= 8;
								data->us_per_quarter_note |= event_data[1];
							}
							if (data->us_per_quarter_note == 0) {
								/* Invalid time, will cause divide by zero */
								data->us_per_quarter_note = 500000;
								XMMS_DBG ("Invalid tempo change to 0 BPM, using 120 BPM");
							}
							bpm = 60000000 / data->us_per_quarter_note;
							XMMS_DBG ("Tempo change to %lu BPM", bpm);
							metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_BPM;
							xmms_xform_metadata_set_int (xform, metakey, bpm);
							break;
						}
						case 0x2F: /* End of track */
							data->end_of_song = TRUE;
							break;
						default: /* Unknown, skip over the data */
							XMMS_DBG ("Ignoring unknown meta event %02X", meta_event);
							xmms_fluidsynth_skip_bytes (xform, len);
							break;
					}
				} else { /* Sysex */
					gulong len;
					/* We have to pass event_data[0] here as it's the first byte of
					 * the length field.
					 */
					if (!xmms_fluidsynth_read_midi_num (xform, &len, &event_data[0]))
						break;
					XMMS_DBG ("MIDI: Ignoring sysex (%lu bytes)", len);
					xmms_fluidsynth_skip_bytes (xform, len);
				}
				break;
			}
			default:
				XMMS_DBG ("Corrupted MIDI file (invalid event 0x%02X)", midi_event);
				data->end_of_song = TRUE;
				break;
		}

		data->now += delay * (data->us_per_quarter_note / data->ticks_per_quarter_note);
		gulong event_time = data->now / 1000;
		if (send_event) {
			fluid_sequencer_send_at (data->sequencer, evt, event_time, 1);
		} /* else no event was generated from the MIDI data */

		/* Break out if we've got enough notes to play until the next callback */
		if (time + CALLBACK_FREQ * 2 < event_time) break;
	}

	if (!data->end_of_song) {
		/* Once we get here we've got enough notes in the buffer waiting to be
		 * played until the next callback, so schedule it now.
		 */
		xmms_fluidsynth_sched_callback (data, time + CALLBACK_FREQ);
	} else {
		metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_DURATION;
		xmms_xform_metadata_set_int (xform, metakey, data->now / 1000);
	}
	delete_fluid_event (evt);
}

static void
xmms_fluidsynth_sched_callback (xmms_fluidsynth_data_t *data,
                                unsigned int callback_date)
{
	fluid_event_t *evt;
	int fluid_res;

	evt = new_fluid_event ();
	fluid_event_set_source (evt, -1);
	fluid_event_set_dest (evt, data->callback_id);
	fluid_event_timer (evt, NULL);

	fluid_res = fluid_sequencer_send_at (data->sequencer, evt, callback_date, 1);
	if (fluid_res != FLUID_OK) data->end_of_song = TRUE;

	delete_fluid_event (evt);
}


static void
xmms_fluidsynth_set_metadata (xmms_xform_t *xform, const gchar *metakey,
                              const gchar *text, guint len)
{
	gsize readsize,writsize;
	GError *err = NULL;
	gchar *tmp;
	const gchar *encoding;
	xmms_config_property_t *config;

	/* Find out what encoding we're currently using */
	config = xmms_xform_config_lookup (xform, "encoding");
	if (config) {
		encoding = xmms_config_property_get_string (config);
	} else {
		encoding = (const gchar *)"ISO8859-1";
	}

	tmp = g_convert ((const char *)text, len, "UTF-8", encoding, &readsize,
	                 &writsize, &err);

	if (!tmp) {
		xmms_log_info ("Converting text '%s' failed (check fluidsynth.encoding property): %s",
		               text, err ? err->message : "Error not set");
		err = NULL;
		tmp = g_convert ((const char *)text, len, "UTF-8", "ISO8859-1", &readsize, &writsize, &err);
	}

	if (tmp) {
		g_strstrip (tmp);
		if (tmp[0] != '\0') {
			xmms_xform_metadata_set_str (xform, metakey, tmp);
		}
		g_free (tmp);
	}

	return;
}
