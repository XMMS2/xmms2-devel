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
 */

/**
 * @file
 * xforms
 */

#include <string.h>

#include <xmmspriv/xmms_plugin.h>
#include <xmmspriv/xmms_xform.h>
#include <xmmspriv/xmms_streamtype.h>
#include <xmmspriv/xmms_medialib.h>
#include <xmmspriv/xmms_utils.h>
#include <xmmspriv/xmms_xform_plugin.h>
#include <xmms/xmms_ipc.h>
#include <xmms/xmms_log.h>
#include <xmms/xmms_object.h>

struct xmms_xform_St {
	xmms_object_t obj;
	struct xmms_xform_St *prev;

	xmms_xform_plugin_t *plugin;
	xmms_medialib_entry_t entry;

	xmms_medialib_t *medialib;

	gboolean inited;

	void *priv;

	xmms_stream_type_t *out_type;

	GList *goal_hints;

	gboolean eos;
	gboolean error;

	char *buffer;
	gint buffered;
	gint buffersize;

	gboolean metadata_collected;

	gboolean metadata_changed;
	GHashTable *metadata;

	GHashTable *privdata;
	GQueue *hotspots;

	xmmsv_t *browse_list;
	xmmsv_t *browse_dict;
	gint browse_index;

	/** used for line reading */
	struct {
		gchar buf[XMMS_XFORM_MAX_LINE_SIZE];
		gchar *bufend;
	} lr;
};

typedef struct xmms_xform_hotspot_St {
	guint pos;
	gchar *key;
	xmmsv_t *obj;
} xmms_xform_hotspot_t;

#define READ_CHUNK 4096


static gboolean xmms_xform_match (xmms_plugin_t *plugin, gpointer user_data);
xmms_xform_t *xmms_xform_find (xmms_xform_t *prev, xmms_medialib_entry_t entry,
                               GList *goal_hints, gboolean want_chain);
static gboolean has_goalformat (xmms_xform_t *xform, GList *goal_formats);
const char *xmms_xform_shortname (xmms_xform_t *xform);
static xmms_xform_t *add_effects (xmms_xform_t *last,
                                  xmms_medialib_entry_t entry,
                                  GList *goal_formats);
static xmms_xform_t *xmms_xform_new_effect (xmms_xform_t* last,
                                            xmms_medialib_entry_t entry,
                                            GList *goal_formats,
                                            const gchar *name);
static void xmms_xform_destroy (xmms_object_t *object);
static xmms_stream_type_t *xmms_xform_get_out_stream_type (xmms_xform_t *xform);

void
xmms_xform_browse_add_entry_property_str (xmms_xform_t *xform,
                                          const gchar *key,
                                          const gchar *value)
{
	xmmsv_t *val = xmmsv_new_string (value);
	xmms_xform_browse_add_entry_property (xform, key, val);
	xmmsv_unref (val);
}


void
xmms_xform_browse_add_entry_property_int (xmms_xform_t *xform,
                                          const gchar *key,
                                          gint value)
{
	xmmsv_t *val = xmmsv_new_int (value);
	xmms_xform_browse_add_entry_property (xform, key, val);
	xmmsv_unref (val);
}

void
xmms_xform_browse_add_symlink_args (xmms_xform_t *xform, const gchar *basename,
                                    const gchar *url, gint nargs, gchar **args)
{
	GString *s;
	gchar *eurl;
	gchar bname[32];
	gint i;

	if (!basename) {
		g_snprintf (bname, sizeof (bname), "%d", xform->browse_index++);
		basename = bname;
	}

	xmms_xform_browse_add_entry (xform, basename, 0);
	eurl = xmms_medialib_url_encode (url);
	s = g_string_new (eurl);

	for (i = 0; i < nargs; i++) {
		g_string_append_c (s, i == 0 ? '?' : '&');
		g_string_append (s, args[i]);
	}

	xmms_xform_browse_add_entry_property_str (xform, "realpath", s->str);

	g_free (eurl);
	g_string_free (s, TRUE);
}

void
xmms_xform_browse_add_symlink (xmms_xform_t *xform, const gchar *basename,
                               const gchar *url)
{
	xmms_xform_browse_add_symlink_args (xform, basename, url, 0, NULL);
}

void
xmms_xform_browse_add_entry_property (xmms_xform_t *xform, const gchar *key,
                                      xmmsv_t *val)
{
	g_return_if_fail (xform);
	g_return_if_fail (xform->browse_dict);
	g_return_if_fail (key);
	g_return_if_fail (val);

	xmmsv_dict_set (xform->browse_dict, key, val);
}

void
xmms_xform_browse_add_entry (xmms_xform_t *xform, const gchar *filename,
                             guint32 flags)
{
	const gchar *url;
	gchar *efile, *eurl, *t;
	gint l, isdir;

	g_return_if_fail (filename);

	t = strchr (filename, '/');
	g_return_if_fail (!t); /* filenames can't contain '/', can they? */

	url = xmms_xform_get_url (xform);
	g_return_if_fail (url);

	xform->browse_dict = xmmsv_new_dict ();

	eurl = xmms_medialib_url_encode (url);
	efile = xmms_medialib_url_encode (filename);

	/* can't use g_build_filename as we need to preserve
	   slashes stuff like file:/// */
	l = strlen (url);
	if (l && url[l - 1] == '/') {
		t = g_strdup_printf ("%s%s", eurl, efile);
	} else {
		t = g_strdup_printf ("%s/%s", eurl, efile);
	}

	isdir = !!(flags & XMMS_XFORM_BROWSE_FLAG_DIR);
	xmms_xform_browse_add_entry_property_str (xform, "path", t);
	xmms_xform_browse_add_entry_property_int (xform, "isdir", isdir);

	if (xform->browse_list == NULL) {
		xform->browse_list = xmmsv_new_list ();
	}
	xmmsv_list_append (xform->browse_list, xform->browse_dict);
	xmmsv_unref (xform->browse_dict);

	g_free (t);
	g_free (efile);
	g_free (eurl);
}

static gint
xmms_browse_list_sortfunc (xmmsv_t **a, xmmsv_t **b)
{
	xmmsv_t *val1, *val2;
	const gchar *s1, *s2;
	int r1, r2;

	g_return_val_if_fail (xmmsv_is_type (*a, XMMSV_TYPE_DICT), 0);
	g_return_val_if_fail (xmmsv_is_type (*b, XMMSV_TYPE_DICT), 0);

	r1 = xmmsv_dict_get (*a, "intsort", &val1);
	r2 = xmmsv_dict_get (*b, "intsort", &val2);

	if (r1 && r2) {
		gint i1, i2;

		if (!xmmsv_get_int (val1, &i1))
			return 0;
		if (!xmmsv_get_int (val2, &i2))
			return 0;
		return i1 > i2;
	}

	if (!xmmsv_dict_get (*a, "path", &val1))
		return 0;
	if (!xmmsv_dict_get (*b, "path", &val2))
		return 0;

	if (!xmmsv_get_string (val1, &s1))
		return 0;
	if (!xmmsv_get_string (val2, &s2))
		return 0;

	return xmms_natcmp (s1, s2);
}

xmmsv_t *
xmms_xform_browse_method (xmms_xform_t *xform, const gchar *url,
                          xmms_error_t *error)
{
	xmmsv_t *list = NULL;

	if (xmms_xform_plugin_can_browse (xform->plugin)) {
		xform->browse_list = xmmsv_new_list ();
		if (!xmms_xform_plugin_browse (xform->plugin, xform, url, error)) {
			return NULL;
		}
		list = xform->browse_list;
		xform->browse_list = NULL;
		xmmsv_list_sort (list, xmms_browse_list_sortfunc);
	} else {
		xmms_error_set (error, XMMS_ERROR_GENERIC, "Couldn't handle that URL");
	}

	return list;
}

xmmsv_t *
xmms_xform_browse (const gchar *url, xmms_error_t *error)
{
	xmmsv_t *list = NULL;
	gchar *durl;
	xmms_xform_t *xform = NULL;
	xmms_xform_t *xform2 = NULL;

	xform = xmms_xform_new (NULL, NULL, NULL, 0, NULL);

	durl = g_strdup (url);
	xmms_medialib_decode_url (durl);
	XMMS_DBG ("url = %s", durl);

	xmms_xform_outdata_type_add (xform,
	                             XMMS_STREAM_TYPE_MIMETYPE,
	                             "application/x-url",
	                             XMMS_STREAM_TYPE_URL,
	                             durl,
	                             XMMS_STREAM_TYPE_END);

	xform2 = xmms_xform_find (xform, 0, NULL, FALSE);
	if (xform2) {
		XMMS_DBG ("found xform %s", xmms_xform_shortname (xform2));
	} else {
		xmms_error_set (error, XMMS_ERROR_GENERIC, "Couldn't handle that URL");
		xmms_object_unref (xform);
		g_free (durl);
		return NULL;
	}

	list = xmms_xform_browse_method (xform2, durl, error);

	xmms_object_unref (xform);
	xmms_object_unref (xform2);

	g_free (durl);

	return list;
}

static void
xmms_xform_destroy (xmms_object_t *object)
{
	xmms_xform_t *xform = (xmms_xform_t *)object;

	XMMS_DBG ("Freeing xform '%s'", xmms_xform_shortname (xform));

	/* The 'destroy' method is not mandatory */
	if (xform->plugin && xform->inited) {
		if (xmms_xform_plugin_can_destroy (xform->plugin)) {
			xmms_xform_plugin_destroy (xform->plugin, xform);
		}
	}

	g_hash_table_destroy (xform->metadata);

	g_hash_table_destroy (xform->privdata);
	g_queue_free (xform->hotspots);

	g_free (xform->buffer);

	if (xform->out_type) {
		xmms_object_unref (xform->out_type);
	}

	if (xform->plugin) {
		xmms_object_unref (xform->plugin);
	}

	if (xform->prev) {
		xmms_object_unref (xform->prev);
	}

}

xmms_xform_t *
xmms_xform_new (xmms_xform_plugin_t *plugin, xmms_xform_t *prev,
                xmms_medialib_t *medialib, xmms_medialib_entry_t entry,
                GList *goal_hints)
{
	xmms_xform_t *xform;

	xform = xmms_object_new (xmms_xform_t, xmms_xform_destroy);

	xform->plugin = plugin ? xmms_object_ref (plugin) : NULL;
	xform->entry = entry;
	xform->medialib = medialib;
	xform->goal_hints = goal_hints;
	xform->lr.bufend = &xform->lr.buf[0];

	if (prev) {
		xmms_object_ref (prev);
		xform->prev = prev;
	}

	xform->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                         g_free,
	                                         (GDestroyNotify) xmmsv_unref);

	xform->privdata = g_hash_table_new_full (g_str_hash, g_str_equal,
	                                         g_free,
	                                         (GDestroyNotify) xmmsv_unref);
	xform->hotspots = g_queue_new ();

	if (plugin && entry) {
		if (!xmms_xform_plugin_init (xform->plugin, xform)) {
			xmms_object_unref (xform);
			return NULL;
		}
		xform->inited = TRUE;
		g_return_val_if_fail (xmms_xform_get_out_stream_type (xform), NULL);
	}

	xform->buffer = g_malloc (READ_CHUNK);
	xform->buffersize = READ_CHUNK;

	return xform;
}

xmms_medialib_entry_t
xmms_xform_entry_get (xmms_xform_t *xform)
{
	return xform->entry;
}

gpointer
xmms_xform_private_data_get (xmms_xform_t *xform)
{
	return xform->priv;
}

void
xmms_xform_private_data_set (xmms_xform_t *xform, gpointer data)
{
	xform->priv = data;
}

void
xmms_xform_outdata_type_add (xmms_xform_t *xform, ...)
{
	va_list ap;
	va_start (ap, xform);
	xform->out_type = xmms_stream_type_parse (ap);
	va_end (ap);
}

void
xmms_xform_outdata_type_set (xmms_xform_t *xform, xmms_stream_type_t *type)
{
	g_return_if_fail (xform->out_type == NULL);
	xform->out_type = xmms_object_ref (type);
}

void
xmms_xform_outdata_type_copy (xmms_xform_t *xform)
{
	xmms_stream_type_t *st = xmms_xform_get_out_stream_type (xform->prev);
	xmms_xform_outdata_type_set (xform, st);
}

const char *
xmms_xform_indata_find_str (xmms_xform_t *xform, xmms_stream_type_key_t key)
{
	const gchar *r = xmms_xform_indata_get_str (xform, key);
	if (r) {
		return r;
	} else if (xform->prev) {
		return xmms_xform_indata_find_str (xform->prev, key);
	}
	return NULL;
}

const char *
xmms_xform_indata_get_str (xmms_xform_t *xform, xmms_stream_type_key_t key)
{
	return xmms_xform_outtype_get_str (xform->prev, key);
}

gint
xmms_xform_indata_get_int (xmms_xform_t *xform, xmms_stream_type_key_t key)
{
	return xmms_xform_outtype_get_int (xform->prev, key);
}

xmms_stream_type_t *
xmms_xform_outtype_get (xmms_xform_t *xform)
{
	return xform->out_type;
}

xmms_stream_type_t *
xmms_xform_intype_get (xmms_xform_t *xform)
{
	return xmms_xform_outtype_get (xform->prev);
}

const char *
xmms_xform_outtype_get_str (xmms_xform_t *xform, xmms_stream_type_key_t key)
{
	const xmms_stream_type_t *st = xmms_xform_get_out_stream_type (xform);
	return xmms_stream_type_get_str (st, key);
}

gint
xmms_xform_outtype_get_int (xmms_xform_t *xform, xmms_stream_type_key_t key)
{
	const xmms_stream_type_t *st = xmms_xform_get_out_stream_type (xform);
	return xmms_stream_type_get_int (st, key);
}

gboolean
xmms_xform_metadata_mapper_match (xmms_xform_t *xform, const gchar *key, const gchar *value, gsize length)
{
	return xmms_xform_plugin_metadata_mapper_match (xform->plugin, xform, key, value, length);
}

gboolean
xmms_xform_metadata_set_int (xmms_xform_t *xform, const char *key, int val)
{
	g_hash_table_insert (xform->metadata, g_strdup (key),
	                     xmmsv_new_int (val));
	xform->metadata_changed = TRUE;
	return TRUE;
}

gboolean
xmms_xform_metadata_set_str (xmms_xform_t *xform, const char *key,
                             const char *val)
{
	const char *old;

	if (!g_utf8_validate (val, -1, NULL)) {
		xmms_log_error ("xform '%s' tried to set property '%s' to a NON UTF-8 string!", xmms_xform_shortname (xform), key);
		return FALSE;
	}

	if (xmms_xform_metadata_get_str (xform, key, &old)) {
		if (strcmp (old, val) == 0) {
			return TRUE;
		}
	}

	g_hash_table_insert (xform->metadata, g_strdup (key),
	                     xmmsv_new_string (val));

	xform->metadata_changed = TRUE;

	return TRUE;
}

static const xmmsv_t *
xmms_xform_metadata_get_val (xmms_xform_t *xform, const char *key)
{
	xmmsv_t *val = NULL;

	for (; xform; xform = xform->prev) {
		val = g_hash_table_lookup (xform->metadata, key);
		if (val) {
			break;
		}
	}

	return val;
}

gboolean
xmms_xform_metadata_has_val (xmms_xform_t *xform, const gchar *key)
{
	return !!xmms_xform_metadata_get_val (xform, key);
}

gboolean
xmms_xform_metadata_get_int (xmms_xform_t *xform, const char *key,
                             gint32 *val)
{
	const xmmsv_t *obj;
	gboolean ret = FALSE;

	obj = xmms_xform_metadata_get_val (xform, key);
	if (obj && xmmsv_get_type (obj) == XMMSV_TYPE_INT32) {
		xmmsv_get_int (obj, val);
		ret = TRUE;
	}

	return ret;
}

gboolean
xmms_xform_metadata_get_str (xmms_xform_t *xform, const char *key,
                             const gchar **val)
{
	const xmmsv_t *obj;
	gboolean ret = FALSE;

	obj = xmms_xform_metadata_get_val (xform, key);
	if (obj && xmmsv_get_type (obj) == XMMSV_TYPE_STRING) {
		xmmsv_get_string (obj, val);
		ret = TRUE;
	}

	return ret;
}

typedef struct {
	xmms_medialib_entry_t entry;
	xmms_medialib_session_t *session;
	gchar *source;
} metadata_festate_t;

static void
add_metadatum (gpointer _key, gpointer _value, gpointer user_data)
{
	xmmsv_t *value = (xmmsv_t *) _value;
	gchar *key = (gchar *) _key;
	metadata_festate_t *st = (metadata_festate_t *) user_data;

	if (xmmsv_get_type (value) == XMMSV_TYPE_STRING) {
		const gchar *s;
		xmmsv_get_string (value, &s);
		xmms_medialib_entry_property_set_str_source (st->session,
		                                             st->entry,
		                                             key,
		                                             s,
		                                             st->source);
	} else if (xmmsv_get_type (value) == XMMSV_TYPE_INT32) {
		gint i;
		xmmsv_get_int (value, &i);
		xmms_medialib_entry_property_set_int_source (st->session,
		                                             st->entry,
		                                             key,
		                                             i,
		                                             st->source);
	} else {
		XMMS_DBG ("Unknown type?!?");
	}
}

static void
xmms_xform_metadata_collect_one (xmms_xform_t *xform, metadata_festate_t *info)
{
	gchar src[XMMS_PLUGIN_SHORTNAME_MAX_LEN + 8];

	XMMS_DBG ("Collecting metadata from %s", xmms_xform_shortname (xform));

	g_snprintf (src, sizeof (src), "plugin/%s",
	            xmms_xform_shortname (xform));

	info->source = src;
	g_hash_table_foreach (xform->metadata, add_metadatum, info);
	info->source = NULL;

	xform->metadata_changed = FALSE;
}

static void
xmms_xform_metadata_collect_r (xmms_xform_t *xform, metadata_festate_t *info,
                               GString *namestr)
{
	if (xform->prev) {
		xmms_xform_metadata_collect_r (xform->prev, info, namestr);
	}

	if (xform->plugin) {
		if (namestr->len) {
			g_string_append_c (namestr, ':');
		}
		g_string_append (namestr, xmms_xform_shortname (xform));
	}

	if (xform->metadata_changed) {
		xmms_xform_metadata_collect_one (xform, info);
	}

	xform->metadata_collected = TRUE;
}

static void
xmms_xform_metadata_collect (xmms_medialib_session_t *session,
                             xmms_xform_t *start, GString *namestr,
                             gboolean rehashing)
{
	metadata_festate_t info;
	gint times_played;
	gint last_started;
	GTimeVal now;

	info.entry = start->entry;

	info.session = session;
	times_played = xmms_medialib_entry_property_get_int (session, info.entry,
	                                                     XMMS_MEDIALIB_ENTRY_PROPERTY_TIMESPLAYED);

	/* times_played == -1 if we haven't played this entry yet. so after initial
	 * metadata collection the mlib would have timesplayed = -1 if we didn't do
	 * the following */
	if (times_played < 0) {
		times_played = 0;
	}

	last_started = xmms_medialib_entry_property_get_int (session, info.entry,
	                                                     XMMS_MEDIALIB_ENTRY_PROPERTY_LASTSTARTED);

	xmms_medialib_entry_cleanup (session, info.entry);

	xmms_xform_metadata_collect_r (start, &info, namestr);

	xmms_medialib_entry_property_set_str (session, info.entry,
	                                      XMMS_MEDIALIB_ENTRY_PROPERTY_CHAIN,
	                                      namestr->str);

	xmms_medialib_entry_property_set_int (session, info.entry,
	                                      XMMS_MEDIALIB_ENTRY_PROPERTY_TIMESPLAYED,
	                                      times_played + (rehashing ? 0 : 1));

	if (!rehashing || (rehashing && last_started)) {
		g_get_current_time (&now);

		xmms_medialib_entry_property_set_int (session, info.entry,
		                                      XMMS_MEDIALIB_ENTRY_PROPERTY_LASTSTARTED,
		                                      (rehashing ? last_started : now.tv_sec));
	}

	xmms_medialib_entry_status_set (session, info.entry,
	                                XMMS_MEDIALIB_ENTRY_STATUS_OK);
}

static void
xmms_xform_metadata_update (xmms_xform_t *xform)
{
	xmms_medialib_session_t *session;
	metadata_festate_t info;

	g_return_if_fail (xform->medialib);

	do {
		session = xmms_medialib_session_begin (xform->medialib);

		info.entry = xform->entry;
		info.session = session;

		xmms_xform_metadata_collect_one (xform, &info);
	} while (!xmms_medialib_session_commit (session));
}

static void
xmms_xform_auxdata_set_val (xmms_xform_t *xform, char *key, xmmsv_t *val)
{
	xmms_xform_hotspot_t *hs;

	hs = g_new0 (xmms_xform_hotspot_t, 1);
	hs->pos = xform->buffered;
	hs->key = key;
	hs->obj = val;

	g_queue_push_tail (xform->hotspots, hs);
}

void
xmms_xform_auxdata_barrier (xmms_xform_t *xform)
{
	xmmsv_t *val = xmmsv_new_none ();
	xmms_xform_auxdata_set_val (xform, NULL, val);
}

void
xmms_xform_auxdata_set_int (xmms_xform_t *xform, const char *key, gint64 intval)
{
	xmmsv_t *val = xmmsv_new_int (intval);
	xmms_xform_auxdata_set_val (xform, g_strdup (key), val);
}

void
xmms_xform_auxdata_set_str (xmms_xform_t *xform, const gchar *key,
                            const gchar *strval)
{
	xmmsv_t *val;
	const char *old;

	if (xmms_xform_auxdata_get_str (xform, key, &old)) {
		if (strcmp (old, strval) == 0) {
			return;
		}
	}

	val = xmmsv_new_string (strval);
	xmms_xform_auxdata_set_val (xform, g_strdup (key), val);
}

void
xmms_xform_auxdata_set_bin (xmms_xform_t *xform, const gchar *key,
                            gpointer data, gssize len)
{
	xmmsv_t *val;

	val = xmmsv_new_bin (data, len);
	xmms_xform_auxdata_set_val (xform, g_strdup (key), val);
}

static const xmmsv_t *
xmms_xform_auxdata_get_val (xmms_xform_t *xform, const gchar *key)
{
	guint i;
	xmms_xform_hotspot_t *hs;
	xmmsv_t *val = NULL;

	/* privdata is always got from the previous xform */
	xform = xform->prev;

	/* check if we have unhandled current (pos 0) hotspots for this key */
	for (i=0; (hs = g_queue_peek_nth (xform->hotspots, i)) != NULL; i++) {
		if (hs->pos != 0) {
			break;
		} else if (hs->key && !strcmp (key, hs->key)) {
			val = hs->obj;
		}
	}

	if (!val) {
		val = g_hash_table_lookup (xform->privdata, key);
	}

	return val;
}

gboolean
xmms_xform_auxdata_has_val (xmms_xform_t *xform, const gchar *key)
{
	return !!xmms_xform_auxdata_get_val (xform, key);
}

/* macro-magically define auxdata extractors */
#define GEN_AUXDATA_EXTRACTOR_FUNC(typename, xmmsvtypename, type) \
	gboolean \
	xmms_xform_auxdata_get_##typename (xmms_xform_t *xform, const gchar *key, \
	                                   type *val) \
	{ \
		const xmmsv_t *obj; \
		obj = xmms_xform_auxdata_get_val (xform, key); \
		return obj && xmmsv_get_##xmmsvtypename (obj, val); \
	}

GEN_AUXDATA_EXTRACTOR_FUNC (int32, int32, gint32);
GEN_AUXDATA_EXTRACTOR_FUNC (int64, int64, gint64);
GEN_AUXDATA_EXTRACTOR_FUNC (str, string, const gchar *);

gboolean
xmms_xform_auxdata_get_bin (xmms_xform_t *xform, const gchar *key,
                            const guchar **data, gsize *datalen)
{
	const xmmsv_t *obj;

	obj = xmms_xform_auxdata_get_val (xform, key);
	if (obj && xmmsv_get_type (obj) == XMMSV_TYPE_BIN) {
		xmmsv_get_bin (obj, data, (guint *) datalen);
		return TRUE;
	}

	return FALSE;
}

const char *
xmms_xform_shortname (xmms_xform_t *xform)
{
	return (xform->plugin)
	       ? xmms_plugin_shortname_get ((xmms_plugin_t *) xform->plugin)
	       : "unknown";
}

static gint
xmms_xform_this_peek (xmms_xform_t *xform, gpointer buf, gint siz,
                      xmms_error_t *err)
{
	while (xform->buffered < siz) {
		gint res;

		if (xform->buffered + READ_CHUNK > xform->buffersize) {
			xform->buffersize *= 2;
			xform->buffer = g_realloc (xform->buffer, xform->buffersize);
		}

		res = xmms_xform_plugin_read (xform->plugin, xform,
		                              &xform->buffer[xform->buffered],
		                              READ_CHUNK, err);

		if (res < -1) {
			XMMS_DBG ("Read method of %s returned bad value (%d) - BUG IN PLUGIN",
			          xmms_xform_shortname (xform), res);
			res = -1;
		}

		if (res == 0) {
			xform->eos = TRUE;
			break;
		} else if (res == -1) {
			xform->error = TRUE;
			return -1;
		} else {
			xform->buffered += res;
		}
	}

	/* might have eosed */
	siz = MIN (siz, xform->buffered);
	memcpy (buf, xform->buffer, siz);
	return siz;
}

static void
xmms_xform_hotspot_callback (gpointer data, gpointer user_data)
{
	xmms_xform_hotspot_t *hs = data;
	gint *read = user_data;

	hs->pos -= *read;
}

static gint
xmms_xform_hotspots_update (xmms_xform_t *xform)
{
	xmms_xform_hotspot_t *hs;
	gint ret = -1;

	hs = g_queue_peek_head (xform->hotspots);
	while (hs != NULL && hs->pos == 0) {
		g_queue_pop_head (xform->hotspots);
		if (hs->key) {
			g_hash_table_insert (xform->privdata, hs->key, hs->obj);
		}
		hs = g_queue_peek_head (xform->hotspots);
	}

	if (hs != NULL) {
		ret = hs->pos;
	}

	return ret;
}

gint
xmms_xform_this_read (xmms_xform_t *xform, gpointer buf, gint siz,
                      xmms_error_t *err)
{
	gint read = 0;
	gint nexths;

	if (xform->error) {
		xmms_error_set (err, XMMS_ERROR_GENERIC, "Read on errored xform");
		return -1;
	}

	/* update hotspots */
	nexths = xmms_xform_hotspots_update (xform);
	if (nexths >= 0) {
		siz = MIN (siz, nexths);
	}

	if (xform->buffered) {
		read = MIN (siz, xform->buffered);
		memcpy (buf, xform->buffer, read);
		xform->buffered -= read;

		/* buffer edited, update hotspot positions */
		g_queue_foreach (xform->hotspots, &xmms_xform_hotspot_callback, &read);

		if (xform->buffered) {
			/* unless we are _peek:ing often
			   this should be fine */
			memmove (xform->buffer, &xform->buffer[read], xform->buffered);
		}
	}

	if (xform->eos) {
		return read;
	}

	while (read < siz) {
		gint res;

		res = xmms_xform_plugin_read (xform->plugin, xform, buf + read, siz - read, err);
		if (xform->metadata_collected && xform->metadata_changed)
			xmms_xform_metadata_update (xform);

		if (res < -1) {
			XMMS_DBG ("Read method of %s returned bad value (%d) - BUG IN PLUGIN", xmms_xform_shortname (xform), res);
			res = -1;
		}

		if (res == 0) {
			xform->eos = TRUE;
			break;
		} else if (res == -1) {
			xform->error = TRUE;
			return -1;
		} else {
			if (read == 0)
				xmms_xform_hotspots_update (xform);

			if (!g_queue_is_empty (xform->hotspots)) {
				if (xform->buffered + res > xform->buffersize) {
					xform->buffersize = MAX (xform->buffersize * 2,
					                         xform->buffersize + res);
					xform->buffer = g_realloc (xform->buffer,
					                           xform->buffersize);
				}

				memmove (xform->buffer + xform->buffered, buf + read, res);
				xform->buffered += res;
				break;
			}
			read += res;
		}
	}

	return read;
}

gint64
xmms_xform_this_seek (xmms_xform_t *xform, gint64 offset,
                      xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	gint64 res;

	if (xform->error) {
		xmms_error_set (err, XMMS_ERROR_GENERIC, "Seek on errored xform");
		return -1;
	}

	if (!xmms_xform_plugin_can_seek (xform->plugin)) {
		XMMS_DBG ("Seek not implemented in '%s'", xmms_xform_shortname (xform));
		xmms_error_set (err, XMMS_ERROR_GENERIC, "Seek not implemented");
		return -1;
	}

	if (xform->buffered && whence == XMMS_XFORM_SEEK_CUR) {
		offset -= xform->buffered;
	}

	res = xmms_xform_plugin_seek (xform->plugin, xform, offset, whence, err);
	if (res != -1) {
		xmms_xform_hotspot_t *hs;

		xform->eos = FALSE;
		xform->buffered = 0;

		/* flush the hotspot queue on seek */
		while ((hs = g_queue_pop_head (xform->hotspots)) != NULL) {
			g_free (hs->key);
			xmmsv_unref (hs->obj);
			g_free (hs);
		}
	}

	return res;
}

gint
xmms_xform_peek (xmms_xform_t *xform, gpointer buf, gint siz,
                 xmms_error_t *err)
{
	g_return_val_if_fail (xform->prev, -1);
	return xmms_xform_this_peek (xform->prev, buf, siz, err);
}

gchar *
xmms_xform_read_line (xmms_xform_t *xform, gchar *line, xmms_error_t *err)
{
	gchar *p;

	g_return_val_if_fail (xform, NULL);
	g_return_val_if_fail (line, NULL);

	p = strchr (xform->lr.buf, '\n');

	if (!p) {
		gint l, r;

		l = (XMMS_XFORM_MAX_LINE_SIZE - 1) - (xform->lr.bufend - xform->lr.buf);
		if (l) {
			r = xmms_xform_read (xform, xform->lr.bufend, l, err);
			if (r < 0) {
				return NULL;
			}
			xform->lr.bufend += r;
		}
		if (xform->lr.bufend <= xform->lr.buf)
			return NULL;

		*(xform->lr.bufend) = '\0';
		p = strchr (xform->lr.buf, '\n');
		if (!p) {
			p = xform->lr.bufend;
		}
	}

	if (p > xform->lr.buf && *(p-1) == '\r') {
		*(p-1) = '\0';
	} else {
		*p = '\0';
	}

	strcpy (line, xform->lr.buf);
	memmove (xform->lr.buf, p + 1, xform->lr.bufend - p);
	xform->lr.bufend -= (p - xform->lr.buf) + 1;
	*xform->lr.bufend = '\0';

	return line;
}

gint
xmms_xform_read (xmms_xform_t *xform, gpointer buf, gint siz, xmms_error_t *err)
{
	g_return_val_if_fail (xform->prev, -1);
	return xmms_xform_this_read (xform->prev, buf, siz, err);
}

gint64
xmms_xform_seek (xmms_xform_t *xform, gint64 offset,
                 xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	g_return_val_if_fail (xform->prev, -1);
	return xmms_xform_this_seek (xform->prev, offset, whence, err);
}

const gchar *
xmms_xform_get_url (xmms_xform_t *xform)
{
	const gchar *url = NULL;
	xmms_xform_t *x;
	x = xform;

	while (!url && x) {
		url = xmms_xform_indata_get_str (x, XMMS_STREAM_TYPE_URL);
		x = x->prev;
	}

	return url;
}

/* State information for outer loop */
typedef struct match_state_St {
	const xmms_stream_type_t *out_type;
	xmms_xform_t *final_xform; /* Goal xform if found */
	gint final_xform_priority; /* Priority of goal xform */
	xmms_xform_t *prev_xform;  /* Parent xform in the chain we are building */
	gint prev_xform_priority;  /* Parent xform's priority */
	xmms_medialib_entry_t entry;
	GList *goal_hints;
	const gchar *chain_title;  /* Name of partial xform chain being built */
	gint depth;                /* Maximum recursion depth */
	gboolean want_chain;       /* FALSE to get just one xform, not a full chain */
} match_state_t;

/* State information for inner loop */
typedef struct match_substate_St {
	match_state_t *state;
	gint largest_priority; /* Largest priority found so far, within the limit */
	gint limit_priority;   /* Looking for plugins at or below this priority */
	gint index;            /* Current index within the loop of all plugins */
	gint start;            /* Number of plugins to skip at loop start */
	gboolean matched;      /* TRUE if an xform matched in this loop */
} match_substate_t;


static gboolean
xmms_xform_submatch (xmms_plugin_t *plugin, gpointer user_data)
{
	xmms_xform_plugin_t *xform_plugin = (xmms_xform_plugin_t *) plugin;
	match_substate_t *substate = (match_substate_t *) user_data;
	gint this_priority = 0;
	xmms_xform_t *xform_test;
	const char *plugin_name, *out_type_str;

	g_assert (plugin->type == XMMS_PLUGIN_TYPE_XFORM);

	/* Skip the first few items if we're nesting a search in the middle of the
	   plugin list */
	substate->index++;
	if (substate->start > substate->index) return TRUE;

/*
	XMMS_DBG ("[chain %s] Trying plugin '%s'",
	          substate->state->chain_title,
	          xmms_plugin_shortname_get (plugin));
*/
	if (!xmms_xform_plugin_supports (xform_plugin, substate->state->out_type, &this_priority)) {
		return TRUE;
	}

	plugin_name = xmms_plugin_shortname_get (plugin);

	if (this_priority == 0) {
		XMMS_DBG ("[chain %s] Plugin '%s' ignored (priority is 0)",
		          substate->state->chain_title,
		          plugin_name);
		return TRUE;
	}

	/* See if this plugin already appears in this chain */
	/* Can't do this because MP3 files first match magic:id3v2 then magic:mad
	xmms_xform_t *p = substate->state->prev_xform;
	while (p) {
		if (p->plugin) {
			const char *prev_name = xmms_plugin_shortname_get ((xmms_plugin_t *) p->plugin);
			if (strcmp(prev_name, plugin_name) == 0) {
				XMMS_DBG ("[chain %s] Skipping plugin '%s' as it's already in the chain",
				          substate->state->chain_title,
				          plugin_name);
				return TRUE;
			}
		}
		p = p->prev;
	}
	*/

	if (
		xmms_xform_plugin_is_effect (xform_plugin)
		&& !xmms_xform_plugin_is_chain_support_effect (xform_plugin)
	) {
		/* Effect plugins aren't needed to build a chain, they get added in the
		   user's preferred order once the chain has been built */
		XMMS_DBG ("[chain %s] Skipping effect plugin '%s'",
			          substate->state->chain_title,
			          plugin_name);
		return TRUE;
	}

	/* Note that the following check only defers priorities *lower than* the
	   current plugin.  Those that are *equal* priority to the current plugin
	   will be treated the same as if they were higher priority.  This is a
	   required part of the algorithm, as it ensures that if a plugin cannot
	   handle a file for some reason, other plugins of the *same priority* will
	   also be tried before falling back to lower priority ones. */
	if (this_priority < substate->largest_priority) {
		/* The priority is too low to use in this run.  This function will be
		   called again later with a lower "largest priority" so we'll be checking
		   this plugin then. */
		XMMS_DBG ("[chain %s] Plugin '%s' deferred (priority %d too low)",
		          substate->state->chain_title,
		          plugin_name,
		          this_priority);
		return TRUE;
	}

	if (this_priority > substate->limit_priority) {
		/* Priority too high (which means we already checked this plugin in a
		   recursive call to this function that ran earlier.) */
		XMMS_DBG ("[chain %s] Plugin '%s' ignored (already tried earlier)",
		          substate->state->chain_title,
		          plugin_name,
		          this_priority);
		return TRUE;
	}

	/* This is the largest priority value we've seen this run, so keep searching
	   for equal or higher priorities, but in a nested loop. */
	substate->largest_priority = this_priority;

	match_substate_t rec;
	rec.state = substate->state;
	rec.largest_priority = substate->largest_priority + 1;
	rec.limit_priority = substate->limit_priority;
	rec.index = 0;
	rec.start = substate->index + 1;
	rec.matched = FALSE;

	XMMS_DBG ("[chain %s] Plugin '%s' matched (priority %d) but will check for "
	          "other priority %d..%d plugins first",
	          substate->state->chain_title,
	          plugin_name,
	          this_priority,
	          substate->largest_priority,
	          substate->limit_priority);

	/* Search the next plugin onwards for any higher priority ones */
	xmms_plugin_foreach (XMMS_PLUGIN_TYPE_XFORM, xmms_xform_submatch, &rec);

	/* If we just handled a plugin with a preferred priority (>100) don't process
	   any further plugins */
	if (rec.matched && (rec.largest_priority > 100)) {
		XMMS_DBG ("Was going to check '%s' but already found >100 priority so "
		          "discontinuing search",
		          plugin_name);
		return FALSE;
	}

	/* At this point we have tested the rest of the plugins for anything that's
	   higher priority than us, which means the only plugins left in the list
	   are those that are lower or equal priority to us.  So we limit ourselves
	   to only looking at the same priority level as this plugin for the rest of
	   the loop.  The lower priority plugins will be checked in the next loop. */
	substate->limit_priority = this_priority;

	out_type_str = xmms_stream_type_get_str(substate->state->out_type,
	                                        XMMS_STREAM_TYPE_MIMETYPE);
	XMMS_DBG ("[chain %s] No higher priority plugins found for '%s', going "
	          "with '%s'",
	          substate->state->chain_title,
	          out_type_str,
	          plugin_name);

	/* Seek back to the start of the file we're checking, otherwise if one plugin
	   reads some data, the next plugin won't see the beginning of the file.
	 */
	xmms_error_t err;
	if (substate->state->prev_xform && substate->state->prev_xform->plugin) {
		/* Move any auxdata from hotspots into privdata.  If we don't do this the
		   auxdata will be lost when the hotspots are cleared during the seek. */
		xmms_xform_hotspots_update (substate->state->prev_xform);

		xmms_xform_this_seek(substate->state->prev_xform, 0, XMMS_XFORM_SEEK_SET, &err);
	}

	/* Init this xform and see if it can handle this format */
	xform_test = xmms_xform_new (xform_plugin,
		substate->state->prev_xform, substate->state->prev_xform->medialib,
		substate->state->entry, substate->state->goal_hints);

	/* If this is a chain support plugin (like 'converter') then it typically
	   comes last in the chain, but we want to use the priority of the xform
	   actually handling the file format.  So for any chain support plugins, we
	   respect the priority when ordering the xforms but when we move on to build
	   the next chain segment (or accept this chain as a goal), we use the
	   priority of the previous (non-chain-support) xform. */
	if (xmms_xform_plugin_is_chain_support_effect (xform_plugin)) {
		this_priority = substate->state->prev_xform_priority;
		XMMS_DBG("Using prev xform priority of %d as '%s' is a chain support plugin",
		         this_priority,
		         plugin_name);
	}

	if (xform_test) {
		/* Got an xform that can handle this format */
		substate->matched = TRUE;

		if (!substate->state->want_chain) {
			/* Only a single xform is wanted, so don't build a whole chain */
			substate->state->final_xform = xform_test;
			substate->state->final_xform_priority = this_priority;
			return FALSE;
		}

		if (!has_goalformat (xform_test, substate->state->goal_hints)) {
			GString *chain_title;
			match_state_t nextState;

			/* Found an xform but it's not the final one, build a chain */
			XMMS_DBG ("[chain %s] Success with '%s', but non-goal so building xform "
			          "chain upon this",
			          substate->state->chain_title,
			          plugin_name);

			if (substate->state->depth > 20) {
				/* Make sure we don't ever get stuck in endless recursion */
				xmms_log_error ("Chain length of %d too long, aborting",
				                substate->state->depth);
				return TRUE;
			}

			/* Build the partial chain string for debug messages */
			chain_title = g_string_new (substate->state->chain_title);
			g_string_append (chain_title, plugin_name);
			g_string_append_c (chain_title, ':');

			/* Now recurse and start the whole process again, this time with a new
			   independent 'state' variable.  We do much the same as happens in
			   xmms_xform_find() except here we carry across some of the state. */
			nextState.out_type = xmms_xform_get_out_stream_type (xform_test);
			nextState.final_xform_priority = substate->state->final_xform_priority;
			nextState.final_xform = NULL;
			nextState.prev_xform = xform_test;
			nextState.prev_xform_priority = this_priority;
			nextState.entry = substate->state->entry;
			nextState.goal_hints = substate->state->goal_hints;
			nextState.chain_title = chain_title->str;
			nextState.depth = substate->state->depth + 1;
			nextState.want_chain = substate->state->want_chain;

			xmms_xform_match(NULL, &nextState);

			g_string_free (chain_title, TRUE);

			if (nextState.final_xform) {
				/* Got a goal chain of sufficient priority */
				if (substate->state->final_xform) {
					XMMS_DBG ("[chain %s] Releasing previous chain %s",
					          substate->state->chain_title,
					          xmms_plugin_shortname_get ((xmms_plugin_t *) substate->state->final_xform->plugin));
					xmms_object_unref (substate->state->final_xform);
				}
				substate->state->final_xform = nextState.final_xform;
				substate->state->final_xform_priority = nextState.final_xform_priority;
			}
			xmms_object_unref (xform_test);

			/* At this point the below 'else' condition will have already executed
			   during recursion for the goal class if there is one, or NULL will be
			   returned and we have to keep searching.  So either way we don't need
			   to do anything further here. */
		} else {
			/* Got a goal format, this is now a fully usable xform chain */
			xmms_log_info ("Got full chain as '%s%s' (final priority %d for '%s')",
			          substate->state->chain_title,
			          plugin_name,
			          this_priority,
			          out_type_str);
			if (this_priority > substate->state->final_xform_priority) {
				/* This final xform is higher priority than the final xform on the
				   current chain, so replace that with this one. */
				XMMS_DBG ("[chain %s%s] New chain is highest priority yet (%d > %d), "
				          "keeping but continuing search",
				          substate->state->chain_title,
				          plugin_name,
				          this_priority,
				          substate->state->final_xform_priority);
				if (substate->state->final_xform) {
					/* This will only happen if there are two xform plugins at the same
					   point in the chain, both of which offer goal formats (e.g. two
					   different plugins set up to match the same 'magic' string with the
					   same MIME type.) */
					XMMS_DBG ("[chain %s%s] Releasing previous goal xform %s",
					          substate->state->chain_title,
					          plugin_name,
					          xmms_plugin_shortname_get ((xmms_plugin_t *) substate->state->final_xform->plugin));
					xmms_object_unref (substate->state->final_xform);
				}
				substate->state->final_xform = xform_test;
				substate->state->final_xform_priority = this_priority;
			} else {
				XMMS_DBG ("[chain %s%s] Not using new chain, priority (%d) isn't "
				          "higher than existing chain (%d)",
				          substate->state->chain_title,
				          plugin_name,
				          this_priority,
				          substate->state->final_xform_priority);
				xmms_object_unref (xform_test);
			}
		}
		/* We have to keep searching here, otherwise any lower priority xforms
		   will get skipped.  We need to check the lower priority xforms as well,
		   because they might end up providing a high priority final xform that
		   we should use.  This can happen when two different chains support the
		   same file format, and it's the final xform priority that we should
		   respect overall, rather than the priority of the individual xforms
		   that make up the chain. */

	} else {
		/* At this point, no plugins could be found that can read the previous
		   plugin's output, so there's nowhere to go with this chain. */
		XMMS_DBG ("Dead end chain '%s%s', resuming search",
		          substate->state->chain_title,
		          plugin_name);
	}

	/* Check the next plugin in the list */
	XMMS_DBG ("[chain %s] Resuming search after '%s', but only for priority == %d",
	          substate->state->chain_title,
	          plugin_name,
	          substate->limit_priority);

	return TRUE;
}

static gboolean
xmms_xform_match (xmms_plugin_t *plugin, gpointer user_data)
{
	match_state_t *state = (match_state_t *) user_data;
	match_substate_t substate;
	int initial_limit_priority;
	const char *out_type_str;

	if (state->out_type == NULL) {
		xmms_log_error ("[chain %s] Asked to find xform for NULL target type",
		                state->chain_title);
		out_type_str = "<any>";
	} else {
		out_type_str = xmms_stream_type_get_str(state->out_type, XMMS_STREAM_TYPE_MIMETYPE);
	}

	XMMS_DBG ("[chain %s] Looking for xform with priority >= %d, that can handle '%s'",
	          state->chain_title,
	          state->final_xform_priority,
	          out_type_str);

	substate.state = state;
	substate.limit_priority = 200;
	do {

		substate.largest_priority = 0;
		substate.index = 0;
		substate.start = 0;
		substate.matched = FALSE;

		initial_limit_priority = substate.limit_priority;
		XMMS_DBG ("[chain %s] Searching all plugins for '%s' handler with "
		          "priority <= %d",
		          state->chain_title,
		          out_type_str,
		          initial_limit_priority);
		xmms_plugin_foreach (XMMS_PLUGIN_TYPE_XFORM, xmms_xform_submatch, &substate);

		XMMS_DBG ("[chain %s] Finished search for '%s' plugin with "
		          "priority %d..%d%s",
		          state->chain_title,
		          out_type_str,
		          substate.largest_priority,
		          initial_limit_priority,
		          substate.largest_priority > 0 ? "" : ", no matches");

		/* If we just handled a plugin with a preferred priority (>100) don't try
		   and further plugins (they will be lower priority anyway) */
		if (substate.matched && (substate.largest_priority > 100)) {
			XMMS_DBG ("[chain %s] Got a preferred plugin that can handle '%s', "
			          "discontinuing search",
			          state->chain_title,
			          out_type_str);
			return FALSE;
		}

		/* If we checked at least one plugin this run, substate.largest_priority
		   will have been changed.  So if this happens, run another loop through the
		   plugins looking for anything lower priority. */
		substate.limit_priority = substate.largest_priority - 1;
	} while (substate.largest_priority > 0);

	XMMS_DBG ("[chain %s] Finished looking for xform with priority >= %d, that can handle '%s'",
	          state->chain_title,
	          state->final_xform_priority,
	          out_type_str);

	return TRUE;
}

xmms_xform_t *
xmms_xform_find (xmms_xform_t *prev, xmms_medialib_entry_t entry,
                 GList *goal_hints, gboolean want_chain)
{
	match_state_t state;

	state.out_type = xmms_xform_get_out_stream_type (prev);
	state.final_xform = NULL;
	state.final_xform_priority = 0;
	state.prev_xform = prev;
	state.prev_xform_priority = 0;
	state.entry = entry;
	state.goal_hints = goal_hints;
	state.chain_title = "";
	state.depth = 0;
	state.want_chain = want_chain;

	xmms_xform_match(NULL, &state);

	if (!state.final_xform) {
		XMMS_DBG ("Found no matching plugin...");
	} else {
		/* Seek back to the start of the file, otherwise we may have lost the first
		   few bytes if some plugins checked the file content after the chain we
		   ended up selecting.
		 */
		xmms_error_t err;
		xmms_xform_this_seek(state.final_xform, 0, XMMS_XFORM_SEEK_SET, &err);
	}
	return state.final_xform;
}

gboolean
xmms_xform_iseos (xmms_xform_t *xform)
{
	gboolean ret = TRUE;

	if (xform->prev) {
		ret = xform->prev->eos;
	}

	return ret;
}

static xmms_stream_type_t *
xmms_xform_get_out_stream_type (xmms_xform_t *xform)
{
	if (!xform->out_type)
		return xmms_xform_plugin_get_out_stream_type (xform->plugin);
	return xform->out_type;
}

const GList *
xmms_xform_goal_hints_get (xmms_xform_t *xform)
{
	return xform->goal_hints;
}

static gboolean
has_goalformat (xmms_xform_t *xform, GList *goal_formats)
{
	const xmms_stream_type_t *current;
	gboolean ret = FALSE;
	GList *n;

	current = xmms_xform_get_out_stream_type (xform);

	for (n = goal_formats; n; n = g_list_next (n)) {
		xmms_stream_type_t *goal_type = n->data;
		if (xmms_stream_type_match (goal_type, current)) {
			ret = TRUE;
			break;
		}

	}

	if (!ret) {
		XMMS_DBG ("Not in one of %d goal-types", g_list_length (goal_formats));
	}

	return ret;
}

static void
outdata_type_metadata_collect (xmms_xform_t *xform)
{
	gint val;
	const char *mime;
	const xmms_stream_type_t *type;

	type = xmms_xform_get_out_stream_type (xform);
	mime = xmms_stream_type_get_str (type, XMMS_STREAM_TYPE_MIMETYPE);
	if (strcmp (mime, "audio/pcm") != 0) {
		return;
	}

	val = xmms_stream_type_get_int (type, XMMS_STREAM_TYPE_FMT_FORMAT);
	if (val != -1) {
		const gchar *name = xmms_sample_name_get ((xmms_sample_format_t) val);
		xmms_xform_metadata_set_str (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_SAMPLE_FMT,
		                             name);
	}

	val = xmms_stream_type_get_int (type, XMMS_STREAM_TYPE_FMT_SAMPLERATE);
	if (val != -1) {
		xmms_xform_metadata_set_int (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_SAMPLERATE,
		                             val);
	}

	val = xmms_stream_type_get_int (type, XMMS_STREAM_TYPE_FMT_CHANNELS);
	if (val != -1) {
		xmms_xform_metadata_set_int (xform,
		                             XMMS_MEDIALIB_ENTRY_PROPERTY_CHANNELS,
		                             val);
	}
}

static xmms_xform_t *
chain_setup (xmms_medialib_t *medialib, xmms_medialib_entry_t entry,
             const gchar *url, GList *goal_formats)
{
	xmms_xform_t *xform, *last;
	gchar *durl, *args;

	if (!entry) {
		entry = 1; /* FIXME: this is soooo ugly, don't do this */
	}

	xform = xmms_xform_new (NULL, NULL, medialib , 0, goal_formats);

	durl = g_strdup (url);

	args = strchr (durl, '?');
	if (args) {
		gchar **params;
		gint i;
		*args = 0;
		args++;
		xmms_medialib_decode_url (args);

		params = g_strsplit (args, "&", 0);

		for (i = 0; params && params[i]; i++) {
			gchar *v;
			v = strchr (params[i], '=');
			if (v) {
				*v = 0;
				v++;
				xmms_xform_metadata_set_str (xform, params[i], v);
			} else {
				xmms_xform_metadata_set_int (xform, params[i], 1);
			}
		}
		g_strfreev (params);
	}
	xmms_medialib_decode_url (durl);

	xmms_xform_outdata_type_add (xform, XMMS_STREAM_TYPE_MIMETYPE,
	                             "application/x-url", XMMS_STREAM_TYPE_URL,
	                             durl, XMMS_STREAM_TYPE_END);


	last = xform;
	xform = xmms_xform_find (last, entry, goal_formats, TRUE);
	xmms_object_unref (last);
	if (!xform) {
		xmms_log_error ("Couldn't set up chain for '%s' (%d)",
		                durl, entry);

		g_free (durl);
		return NULL;
	}
	g_free (durl);

	outdata_type_metadata_collect (xform);

	return xform;
}

static void
chain_finalize (xmms_medialib_session_t *session,
                xmms_xform_t *xform, xmms_medialib_entry_t entry,
                const gchar *url, gboolean rehashing)
{
	GString *namestr;
	gchar *durl;

	durl = g_strdup (url);
	xmms_medialib_decode_url (durl);

	namestr = g_string_new ("");
	xmms_xform_metadata_collect (session, xform, namestr, rehashing);
	xmms_log_info ("Successfully set up chain for '%s' (%d) containing %s",
	               durl, entry, namestr->str);

	g_string_free (namestr, TRUE);
	g_free (durl);
}

static gchar *
get_url_for_entry (xmms_medialib_session_t *session, xmms_medialib_entry_t entry)
{
	gchar *url = NULL;

	url = xmms_medialib_entry_property_get_str (session, entry, XMMS_MEDIALIB_ENTRY_PROPERTY_URL);

	if (!url) {
		xmms_log_error ("Couldn't get url for entry (%d)", entry);
	}

	return url;
}

xmms_xform_t *
xmms_xform_chain_setup (xmms_medialib_t *medialib, xmms_medialib_entry_t entry,
                        GList *goal_formats, gboolean rehash)
{
	xmms_medialib_session_t *session;
	xmms_xform_t *ret = NULL;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (ret != NULL)
			xmms_object_unref (ret);
		ret = xmms_xform_chain_setup_session (medialib, session, entry, goal_formats, rehash);
	} while (!xmms_medialib_session_commit (session));

	return ret;
}

xmms_xform_t *
xmms_xform_chain_setup_session (xmms_medialib_t *medialib,
                                xmms_medialib_session_t *session,
                                xmms_medialib_entry_t entry,
                                GList *goal_formats, gboolean rehash)
{
	gchar *url;
	xmms_xform_t *xform;

	if (!(url = get_url_for_entry (session, entry))) {
		return NULL;
	}

	xform = xmms_xform_chain_setup_url_session (medialib, session, entry,
	                                            url, goal_formats, rehash);
	g_free (url);

	return xform;
}

xmms_xform_t *
xmms_xform_chain_setup_url_session (xmms_medialib_t *medialib,
                                    xmms_medialib_session_t *session,
                                    xmms_medialib_entry_t entry, const gchar *url,
                                    GList *goal_formats, gboolean rehash)
{
	xmms_xform_t *last;
	xmms_plugin_t *plugin;
	xmms_xform_plugin_t *xform_plugin;
	gboolean add_segment = FALSE;
	gint priority;

	last = chain_setup (medialib, entry, url, goal_formats);
	if (!last) {
		return NULL;
	}

	/* first check that segment plugin is available in the system */
	plugin = xmms_plugin_find (XMMS_PLUGIN_TYPE_XFORM, "segment");
	xform_plugin = (xmms_xform_plugin_t *) plugin;

	/* if segment plugin input is the same as current output, include it
	 * for collecting additional duration metadata on audio entries */
	if (xform_plugin) {
		const xmms_stream_type_t *st = xmms_xform_get_out_stream_type (last);
		add_segment = xmms_xform_plugin_supports (xform_plugin, st,
		                                          &priority);
		xmms_object_unref (plugin);
	}

	/* add segment plugin to the chain if it can be added */
	if (add_segment) {
		last = xmms_xform_new_effect (last, entry, goal_formats, "segment");
		if (!last) {
			return NULL;
		}
	}

	/* if not rehashing, also initialize all the effect plugins */
	if (!rehash) {
		last = add_effects (last, entry, goal_formats);
		if (!last) {
			return NULL;
		}
	}

	chain_finalize (session, last, entry, url, rehash);
	return last;
}

xmms_xform_t *
xmms_xform_chain_setup_url (xmms_medialib_t *medialib,
                            xmms_medialib_entry_t entry, const gchar *url,
                            GList *goal_formats, gboolean rehash)
{
	xmms_medialib_session_t *session;
	xmms_xform_t *ret = NULL;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (ret != NULL)
			xmms_object_unref (ret);
		ret = xmms_xform_chain_setup_url_session (medialib, session, entry, url,
		                                          goal_formats, rehash);
	} while (!xmms_medialib_session_commit (session));

	return ret;
}

xmms_config_property_t *
xmms_xform_config_lookup (xmms_xform_t *xform, const gchar *path)
{
	g_return_val_if_fail (xform->plugin, NULL);

	return xmms_plugin_config_lookup ((xmms_plugin_t *) xform->plugin, path);
}

static xmms_xform_t *
add_effects (xmms_xform_t *last, xmms_medialib_entry_t entry,
             GList *goal_formats)
{
	gint effect_no;

	for (effect_no = 0; TRUE; effect_no++) {
		xmms_config_property_t *cfg;
		gchar key[64];
		const gchar *name;

		g_snprintf (key, sizeof (key), "effect.order.%i", effect_no);

		cfg = xmms_config_lookup (key);
		if (!cfg) {
			break;
		}

		name = xmms_config_property_get_string (cfg);

		if (!name[0]) {
			continue;
		}

		last = xmms_xform_new_effect (last, entry, goal_formats, name);
	}

	return last;
}

static xmms_xform_t *
xmms_xform_new_effect (xmms_xform_t *last, xmms_medialib_entry_t entry,
                       GList *goal_formats, const gchar *name)
{
	xmms_plugin_t *plugin;
	xmms_xform_plugin_t *xform_plugin;
	xmms_xform_t *xform;
	gint priority;

	plugin = xmms_plugin_find (XMMS_PLUGIN_TYPE_XFORM, name);
	if (!plugin) {
		xmms_log_error ("Couldn't find any effect named '%s'", name);
		return last;
	}

	xform_plugin = (xmms_xform_plugin_t *) plugin;
	if (!xmms_xform_plugin_supports (xform_plugin, last->out_type, &priority)) {
		xmms_log_info ("Effect '%s' doesn't support format, skipping",
		               xmms_plugin_shortname_get (plugin));
		xmms_object_unref (plugin);
		return last;
	}

	xform = xmms_xform_new (xform_plugin, last, last->medialib, entry, goal_formats);

	if (xform) {
		xmms_object_unref (last);
		last = xform;
	} else {
		xmms_log_info ("Effect '%s' failed to initialize, skipping",
		               xmms_plugin_shortname_get (plugin));
	}
	xmms_xform_plugin_config_property_register (xform_plugin,
	                                            "enabled", "0",
	                                            NULL, NULL);
	xmms_object_unref (plugin);
	return last;
}
