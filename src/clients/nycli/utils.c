/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2013 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 */

#include <fnmatch.h>

#include "utils.h"
#include "status.h"
#include "currently_playing.h"
#include "compat.h"

#include "cli_infos.h"
#include "cli_cache.h"
#include "column_display.h"
#include "xmmscall.h"

static void coll_int_attribute_set (xmmsv_t *coll, const char *key, gint value);
static xmmsv_t *coll_make_reference (const char *name, xmmsc_coll_namespace_t ns);
static void coll_print_attributes (const char *key, xmmsv_t *val, void *udata);

static gint compare_uint (gconstpointer a, gconstpointer b, gpointer userdata);

typedef enum {
	IDLIST_CMD_NONE = 0,
	IDLIST_CMD_REHASH,
	IDLIST_CMD_REMOVE
} idlist_command_t;

typedef struct {
	cli_infos_t *infos;
	column_display_t *coldisp;
	const gchar *playlist;
	GArray *entries;
	gint inc;
	gint pos;
} pl_pos_udata_t;

/* Dumps a propdict on stdout */
static void
dict_dump (const gchar *source, xmmsv_t *val, void *udata)
{
	xmmsv_type_t type;

	const gchar **keyfilter = (const gchar **) udata;
	const gchar *key = (const gchar *) keyfilter[0];
	const gchar *filter = (const gchar *) keyfilter[1];

	if (filter && strcmp (filter, source) != 0) {
		return;
	}

	type = xmmsv_get_type (val);

	switch (type) {
	case XMMSV_TYPE_INT32:
	{
		gint value;
		xmmsv_get_int (val, &value);
		g_printf (_("[%s] %s = %u\n"), source, key, value);
		break;
	}
	case XMMSV_TYPE_STRING:
	{
		const gchar *value;
		xmmsv_get_string (val, &value);
		/* FIXME: special handling for url, guess charset, see common.c:print_entry */
		g_printf (_("[%s] %s = %s\n"), source, key, value);
		break;
	}
	case XMMSV_TYPE_LIST:
		g_printf (_("[%s] %s = <list>\n"), source, key);
		break;
	case XMMSV_TYPE_DICT:
		g_printf (_("[%s] %s = <dict>\n"), source, key);
		break;
	case XMMSV_TYPE_COLL:
		g_printf (_("[%s] %s = <coll>\n"), source, key);
		break;
	case XMMSV_TYPE_BIN:
		g_printf (_("[%s] %s = <bin>\n"), source, key);
		break;
	case XMMSV_TYPE_END:
		g_printf (_("[%s] %s = <end>\n"), source, key);
		break;
	case XMMSV_TYPE_NONE:
		g_printf (_("[%s] %s = <none>\n"), source, key);
		break;
	case XMMSV_TYPE_ERROR:
		g_printf (_("[%s] %s = <error>\n"), source, key);
		break;
	default:
		break;
	}
}

static void
propdict_dump (const gchar *key, xmmsv_t *src_dict, void *udata)
{
	const gchar *keyfilter[] = {key, udata};

	xmmsv_dict_foreach (src_dict, dict_dump, (void *) keyfilter);
}

/* Compare two uint like strcmp */
static gint
compare_uint (gconstpointer a, gconstpointer b, gpointer userdata)
{
	guint va = *((guint *)a);
	guint vb = *((guint *)b);

	if (va < vb) {
		return -1;
	} else if (va > vb) {
		return 1;
	} else {
		return 0;
	}
}

void
print_stats (xmmsv_t *val)
{
	const gchar *version;
	gint uptime;

	xmmsv_dict_entry_get_string (val, "version", &version);
	xmmsv_dict_entry_get_int (val, "uptime", &uptime);

	g_printf ("uptime = %d\n"
	          "version = %s\n", uptime, version);
}

static void
print_config_entry (const gchar *confname, xmmsv_t *val, void *udata)
{
	xmmsv_type_t type;

	type = xmmsv_get_type (val);

	switch (type) {
	case XMMSV_TYPE_STRING:
	{
		const gchar *confval;
		xmmsv_get_string (val, &confval);
		g_printf ("%s = %s\n", confname, confval);
		break;
	}
	case XMMSV_TYPE_INT32:
	{
		int confval;
		xmmsv_get_int (val, &confval);
		g_printf ("%s = %d\n", confname, confval);
		break;
	}
	default:
		break;
	}
}

void
print_config (cli_infos_t *infos, const gchar *confname)
{
	xmmsc_result_t *res;
	xmmsv_t *config;

	res = xmmsc_config_list_values (infos->sync);
	xmmsc_result_wait (res);
	config = xmmsc_result_get_value (res);

	if (confname) {
		/* Filter out keys that don't match the config name after
		 * shell wildcard expansion.  */

		xmmsv_dict_iter_t *it;
		const gchar *key;
		xmmsv_t *val;

		xmmsv_get_dict_iter (config, &it);
		while (xmmsv_dict_iter_pair (it, &key, &val)) {
			if (fnmatch (confname, key, 0)) {
				xmmsv_dict_iter_remove (it);
			} else {
				xmmsv_dict_iter_next (it);
			}
		}
	}

	xmmsv_dict_foreach (config, print_config_entry, NULL);
	xmmsc_result_unref (res);

	return;
}

void
print_property (cli_infos_t *infos, xmmsv_t *dict, guint id,
                const gchar *source, const gchar *property)
{
	if (property == NULL) {
		xmmsv_dict_foreach (dict, propdict_dump, (void *) source);
	} else {
		/* FIXME(g): print if given an specific property */
	}
}

/* Apply operation to an idlist */
static void
apply_ids (cli_infos_t *infos, xmmsv_t *val, idlist_command_t cmd)
{
	xmmsv_list_iter_t *it;
	gint32 id;

	xmmsv_get_list_iter (val, &it);

	while (xmmsv_list_iter_entry_int (it, &id)) {
		switch (cmd) {
			case IDLIST_CMD_REHASH:
				XMMS_CALL (xmmsc_medialib_rehash, infos->sync, id);
				break;
			case IDLIST_CMD_REMOVE:
				XMMS_CALL (xmmsc_medialib_remove_entry, infos->sync, id);
				break;
			default:
				break;
		}
		xmmsv_list_iter_next (it);
	}
}

void
remove_ids (cli_infos_t *infos, xmmsv_t *list)
{
	apply_ids (infos, list, IDLIST_CMD_REMOVE);
}

static void
pos_remove_cb (gint pos, void *userdata)
{
	pl_pos_udata_t *pack = (pl_pos_udata_t *) userdata;
	XMMS_CALL (xmmsc_playlist_remove_entry, pack->infos->sync, pack->playlist, pos);
}

void
positions_remove (cli_infos_t *infos, const gchar *playlist,
                  playlist_positions_t *positions)
{
	pl_pos_udata_t udata = { infos, NULL, playlist, NULL, 0, 0 };
	playlist_positions_foreach (positions, pos_remove_cb, FALSE, &udata);
}

void
rehash_ids (cli_infos_t *infos, xmmsv_t *list)
{
	apply_ids (infos, list, IDLIST_CMD_REHASH);
}

static void
print_volume_entry (const gchar *key, xmmsv_t *val, void *udata)
{
	const gchar *channel = udata;
	gint32 value;

	if (!udata || !strcmp (key, channel)) {
		xmmsv_get_int (val, &value);
		g_printf (_("%s = %u\n"), key, value);
	}
}

void
print_volume (xmmsv_t *dict, const gchar *channel)
{
	xmmsv_dict_foreach (dict, print_volume_entry, (void *) channel);
}

static void
dict_keys (const gchar *key, xmmsv_t *val, void *udata)
{
	GList **list = udata;

	*list = g_list_prepend (*list, g_strdup (key));
}

void
adjust_volume (cli_infos_t *infos, const gchar *channel, gint relative)
{
	xmmsc_result_t *res;
	xmmsv_t *val;
	xmmsv_dict_iter_t *it;
	const gchar *innerchan;
	const gchar *err;

	gint volume;

	res = xmmsc_playback_volume_get (infos->sync);
	xmmsc_result_wait (res);
	val = xmmsc_result_get_value (res);

	if (xmmsv_get_error (val, &err)) {
		g_printf (_("Server error: %s\n"), err);
		xmmsc_result_unref (res);
		return;
	}

	xmmsv_get_dict_iter (val, &it);

	while (xmmsv_dict_iter_pair_int (it, &innerchan, &volume)) {
		if (channel && strcmp (channel, innerchan) == 0) {
			volume += relative;
			if (volume > 100) {
				volume = 100;
			} else if (volume < 0) {
				volume = 0;
			}

			XMMS_CALL (xmmsc_playback_volume_set, infos->sync, innerchan, volume);
		}
		xmmsv_dict_iter_next (it);
	}

	xmmsc_result_unref (res);
}

void
set_volume (cli_infos_t *infos, const gchar *channel, gint volume)
{
	xmmsc_result_t *res;
	xmmsv_t *val;
	GList *it, *channels = NULL;

	if (!channel) {
		/* get all channels */
		res = xmmsc_playback_volume_get (infos->sync);
		xmmsc_result_wait (res);
		val = xmmsc_result_get_value (res);
		xmmsv_dict_foreach (val, dict_keys, &channels);
		xmmsc_result_unref (res);
	} else {
		channels = g_list_prepend (channels, g_strdup (channel));
	}

	/* set volumes for channels in list */
	for (it = g_list_first (channels); it != NULL; it = g_list_next (it)) {
		XMMS_CALL (xmmsc_playback_volume_set, infos->sync, it->data, volume);
	}

	g_list_free_full (channels, g_free);
}

void
currently_playing_mode (cli_infos_t *infos, const gchar *format, gint refresh)
{
	status_entry_t *status;

	status = currently_playing_init (format, refresh);

	if (refresh > 0) {
		cli_infos_status_mode (infos, status);
	} else {
		status_refresh (infos, status, TRUE, TRUE);
		status_free (status);
	}
}

void
list_print_info (xmmsv_t *val, cli_infos_t *infos)
{
	xmmsv_list_iter_t *it;
	gboolean first = TRUE;
	gint32 id;

	xmmsv_get_list_iter (val, &it);
	while (xmmsv_list_iter_entry_int (it, &id)) {
		if (!first) {
			g_printf ("\n");
		} else {
			first = FALSE;
		}

		XMMS_CALL_CHAIN (XMMS_CALL_P (xmmsc_medialib_get_info, infos->sync, id),
		                 FUNC_CALL_P (xmmsv_dict_foreach, XMMS_PREV_VALUE, propdict_dump, NULL));

		xmmsv_list_iter_next (it);
	}
}

static void
pos_print_info_cb (gint pos, void *userdata)
{
	pl_pos_udata_t *pack = (pl_pos_udata_t *) userdata;
	guint id;

	/* Skip if outside of playlist */
	if (pos >= pack->infos->cache->active_playlist->len) {
		return;
	}

	id = g_array_index (pack->infos->cache->active_playlist, guint, pos);

	/* Do not prepend newline before the first entry */
	if (pack->inc > 0) {
		g_printf ("\n");
	} else {
		pack->inc++;
	}

	XMMS_CALL_CHAIN (XMMS_CALL_P (xmmsc_medialib_get_info, pack->infos->sync, id),
	                 FUNC_CALL_P (xmmsv_dict_foreach, XMMS_PREV_VALUE, propdict_dump, NULL));
}

void
positions_print_info (cli_infos_t *infos, playlist_positions_t *positions)
{
	pl_pos_udata_t udata = { infos, NULL, NULL, NULL, 0, 0 };
	playlist_positions_foreach (positions, pos_print_info_cb, TRUE, &udata);
}

void
enrich_mediainfo (xmmsv_t *val)
{
	if (!xmmsv_dict_has_key (val, "title") && xmmsv_dict_has_key (val, "url")) {
		/* First decode the URL encoding */
		xmmsv_t *tmp, *v, *urlv;
		gchar *url = NULL;
		const gchar *filename = NULL;
		const unsigned char *burl;
		unsigned int blen;

		xmmsv_dict_get (val, "url", &v);

		tmp = xmmsv_decode_url (v);
		if (tmp && xmmsv_get_bin (tmp, &burl, &blen)) {
			url = g_malloc (blen + 1);
			memcpy (url, burl, blen);
			url[blen] = 0;
			xmmsv_unref (tmp);
			filename = strrchr (url, '/');
			if (!filename || !filename[1]) {
				filename = url;
			} else {
				filename = filename + 1;
			}
		}

		/* Let's see if the result is valid utf-8. This must be done
		 * since we don't know the charset of the binary string */
		if (filename && g_utf8_validate (filename, -1, NULL)) {
			/* If it's valid utf-8 we don't have any problem just
			 * printing it to the screen
			 */
			urlv = xmmsv_new_string (filename);
		} else if (filename) {
			/* Not valid utf-8 :-( We make a valid guess here that
			 * the string when it was encoded with URL it was in the
			 * same charset as we have on the terminal now.
			 *
			 * THIS MIGHT BE WRONG since different clients can have
			 * different charsets and DIFFERENT computers most likely
			 * have it.
			 */
			gchar *tmp2 = g_locale_to_utf8 (filename, -1, NULL, NULL, NULL);
			urlv = xmmsv_new_string (tmp2);
			g_free (tmp2);
		} else {
			/* Decoding the URL failed for some reason. That's not good. */
			urlv = xmmsv_new_string (_("?"));
		}

		xmmsv_dict_set (val, "title", urlv);
		xmmsv_unref (urlv);
		g_free (url);
	}
}

static void
id_coldisp_print_info (cli_infos_t *infos, column_display_t *coldisp, guint id)
{
	xmmsc_result_t *infores;
	xmmsv_t *info;

	infores = xmmsc_medialib_get_info (infos->sync, id);
	xmmsc_result_wait (infores);
	info = xmmsv_propdict_to_dict (xmmsc_result_get_value (infores), NULL);
	enrich_mediainfo (info);
	column_display_print (coldisp, info);

	xmmsc_result_unref (infores);
	xmmsv_unref (info);
}

static void
pos_print_row_cb (gint pos, void *userdata)
{
	pl_pos_udata_t *pack = (pl_pos_udata_t *) userdata;
	guint id;

	if (pos >= pack->entries->len) {
		return;
	}

	id = g_array_index (pack->entries, guint, pos);
	column_display_set_position (pack->coldisp, pos);
	id_coldisp_print_info (pack->infos, pack->coldisp, id);
}

void
positions_print_list (xmmsv_t *val, playlist_positions_t *positions,
                      column_display_t *coldisp, gboolean is_search)
{
	cli_infos_t *infos = column_display_infos_get (coldisp);
	pl_pos_udata_t udata = { infos, coldisp, NULL, NULL, 0, 0};
	xmmsv_list_iter_t *it;
	gint32 id;

	/* FIXME: separate function or merge
	   with list_print_row (lot of if(positions))? */

	column_display_prepare (coldisp);

	if (is_search) {
		column_display_print_header (coldisp);
	}

	udata.entries = g_array_sized_new (FALSE, FALSE, sizeof (guint),
	                                   xmmsv_list_get_size (val));

	xmmsv_get_list_iter (val, &it);

	while (xmmsv_list_iter_entry_int (it, &id)) {
		g_array_append_val (udata.entries, id);
		xmmsv_list_iter_next (it);
	}

	playlist_positions_foreach (positions, pos_print_row_cb, TRUE, &udata);

	g_array_free (udata.entries, TRUE);

	if (is_search) {
		column_display_print_footer (coldisp);
	} else {
		g_printf ("\n");
		column_display_print_footer_totaltime (coldisp);
	}

	column_display_free (coldisp);
}

/* Returned tree must be freed by the caller */
static GTree *
matching_ids_tree (xmmsv_t *matching)
{
	xmmsv_list_iter_t *it;
	xmmsv_t *val;
	gint32 id;
	GTree *list = NULL;

	list = g_tree_new_full (compare_uint, NULL, g_free, NULL);

	xmmsv_get_list_iter (matching, &it);
	while (xmmsv_list_iter_entry_int (it, &id)) {
		guint *tid = g_new (guint, 1);
		*tid = id;
		g_tree_insert (list, tid, tid);
		xmmsv_list_iter_next (it);
	}

	return list;
}

void
list_print_row (xmmsv_t *val, xmmsv_t *filter,
                column_display_t *coldisp, gboolean is_search,
                gboolean result_is_infos)
{
	/* FIXME: w00t at code copy-paste, please modularize */
	cli_infos_t *infos = column_display_infos_get (coldisp);
	xmmsv_list_iter_t *it;
	xmmsv_t *entry;
	GTree *list = NULL;

	gint id;
	gint i = 0;

	column_display_prepare (coldisp);

	if (filter != NULL) {
		xmmsc_result_t *filres;
		xmmsv_t *filval;
		filres = xmmsc_coll_query_ids (infos->sync, filter, NULL, 0, 0);
		xmmsc_result_wait (filres);
		filval = xmmsc_result_get_value (filres);
		if ((list = matching_ids_tree (filval)) == NULL) {
			goto finish;
		}
	}

	if (is_search) {
		column_display_print_header (coldisp);
	}

	xmmsv_get_list_iter (val, &it);
	while (xmmsv_list_iter_entry (it, &entry)) {
		column_display_set_position (coldisp, i);

		if (result_is_infos) {
			enrich_mediainfo (entry);
			column_display_print (coldisp, entry);
		} else {
			if (xmmsv_get_int (entry, &id) &&
			    (!list || g_tree_lookup (list, &id) != NULL)) {
				id_coldisp_print_info (infos, coldisp, id);
			}
		}
		xmmsv_list_iter_next (it);
		i++;
	}

	if (is_search) {
		column_display_print_footer (coldisp);
	} else {
		g_printf ("\n");
		column_display_print_footer_totaltime (coldisp);
	}

finish:
	if (list) {
		g_tree_destroy (list);
	}
	column_display_free (coldisp);
}

void
coll_save (cli_infos_t *infos, xmmsv_t *coll,
           xmmsc_coll_namespace_t ns, const gchar *name, gboolean force)
{
	xmmsc_result_t *res;
	xmmsv_t *val;
	gboolean save = TRUE;

	if (!force) {
		res = xmmsc_coll_get (infos->sync, name, ns);
		xmmsc_result_wait (res);
		val = xmmsc_result_get_value (res);
		if (xmmsv_is_type (val, XMMSV_TYPE_COLL)) {
			g_printf (_("Error: A collection already exists "
			            "with the target name!\n"));
			save = FALSE;
		}
		xmmsc_result_unref (res);
	}

	if (save) {
		XMMS_CALL (xmmsc_coll_save, infos->sync, coll, name, ns);
	}
}

/* (from src/clients/cli/common.c) */
static void
print_info (const gchar *fmt, ...)
{
	gchar buf[8096];
	va_list ap;

	va_start (ap, fmt);
	g_vsnprintf (buf, 8096, fmt, ap);
	va_end (ap);

	g_printf ("%s\n", buf);
}

/* Produce a GString from the idlist of the collection.
   (must be freed manually!)
   (from src/clients/cli/cmd_coll.c) */
static GString *
coll_idlist_to_string (xmmsv_t *coll)
{
	xmmsv_list_iter_t *it;
	gint32 entry;
	gboolean first = TRUE;
	GString *s;

	s = g_string_new ("(");

	xmmsv_get_list_iter (xmmsv_coll_idlist_get (coll), &it);
	while (xmmsv_list_iter_entry_int (it, &entry)) {
		if (first) {
			first = FALSE;
		} else {
			g_string_append (s, ", ");
		}

		xmmsv_list_iter_entry_int (it, &entry);
		g_string_append_printf (s, "%d", entry);

		xmmsv_list_iter_next (it);
	}
	xmmsv_list_iter_explicit_destroy (it);

	g_string_append_c (s, ')');

	return s;
}

/* (from src/clients/cli/cmd_coll.c) */
static void
coll_dump_list (xmmsv_t *list, unsigned int level)
{
	xmmsv_list_iter_t *it;
	xmmsv_t *v;

	xmmsv_get_list_iter (list, &it);
	while (xmmsv_list_iter_entry (it, &v)) {
		coll_dump (v, level);
		xmmsv_list_iter_next (it);
	}

}

static void
coll_write_attribute (const char *key, xmmsv_t *val, gboolean *first)
{
	const char *str;
	if (xmmsv_get_string (val, &str)) {
		if (*first) {
			g_printf ("%s: %s", key, str);
			*first = FALSE;
		} else {
			g_printf (", %s: %s", key, str);
		}
	}
}

static void
coll_dump_attributes (xmmsv_t *attr, gchar *indent)
{
	gboolean first = TRUE;
	if (xmmsv_dict_get_size (attr) > 0) {
		g_printf ("%sAttributes: (", indent);
		xmmsv_dict_foreach (attr, (xmmsv_dict_foreach_func)coll_write_attribute, &first);
		g_printf (")\n");
	}
}

/* Dump the structure of the collection as a string
   (from src/clients/cli/cmd_coll.c) */
void
coll_dump (xmmsv_t *coll, guint level)
{
	gint i;
	gchar *indent;

	const gchar *type;
	GString *idlist_str;

	indent = g_malloc ((level * 2) + 1);
	for (i = 0; i < level * 2; ++i) {
		indent[i] = ' ';
	}
	indent[i] = '\0';

	/* type */
	switch (xmmsv_coll_get_type (coll)) {
	case XMMS_COLLECTION_TYPE_REFERENCE:
		type = "Reference";
		break;

	case XMMS_COLLECTION_TYPE_UNIVERSE:
		type = "Universe";
		break;

	case XMMS_COLLECTION_TYPE_UNION:
		type = "Union";
		break;

	case XMMS_COLLECTION_TYPE_INTERSECTION:
		type = "Intersection";
		break;

	case XMMS_COLLECTION_TYPE_COMPLEMENT:
		type = "Complement";
		break;

	case XMMS_COLLECTION_TYPE_HAS:
		type = "Has";
		break;

	case XMMS_COLLECTION_TYPE_MATCH:
		type = "Match";
		break;

	case XMMS_COLLECTION_TYPE_TOKEN:
		type = "Token";
		break;

	case XMMS_COLLECTION_TYPE_EQUALS:
		type = "Equals";
		break;

	case XMMS_COLLECTION_TYPE_NOTEQUAL:
		type = "Not-equal";
		break;

	case XMMS_COLLECTION_TYPE_SMALLER:
		type = "Smaller";
		break;

	case XMMS_COLLECTION_TYPE_SMALLEREQ:
		type = "Smaller-equal";
		break;

	case XMMS_COLLECTION_TYPE_GREATER:
		type = "Greater";
		break;

	case XMMS_COLLECTION_TYPE_GREATEREQ:
		type = "Greater-equal";
		break;

	case XMMS_COLLECTION_TYPE_IDLIST:
		type = "Idlist";
		break;

	default:
		type = "Unknown Operator!";
		break;
	}

	print_info ("%sType: %s", indent, type);

	/* Idlist */
	idlist_str = coll_idlist_to_string (coll);
	if (strcmp (idlist_str->str, "()") != 0) {
		print_info ("%sIDs: %s", indent, idlist_str->str);
	}
	g_string_free (idlist_str, TRUE);

	/* Attributes */
	coll_dump_attributes (xmmsv_coll_attributes_get (coll), indent);

	/* Operands */
	coll_dump_list (xmmsv_coll_operands_get (coll), level + 1);
}

static void
print_collections_list (xmmsv_t *val, cli_infos_t *infos,
                        const gchar *mark, gboolean all)
{
	xmmsv_list_iter_t *it;
	const gchar *s;

	xmmsv_get_list_iter (val, &it);
	while (xmmsv_list_iter_entry_string (it, &s)) {
		/* Skip hidden playlists if all is FALSE*/
		if ((*s != '_') || all) {
			/* Highlight active playlist */
			if (mark && strcmp (s, mark) == 0) {
				g_printf ("* %s\n", s);
			} else {
				g_printf ("  %s\n", s);
			}
		}
		xmmsv_list_iter_next (it);
	}
}

void
list_print_collections (xmmsv_t *list, cli_infos_t *infos)
{
	print_collections_list (list, infos, NULL, TRUE);
}

void
list_print_playlists (xmmsv_t *list, cli_infos_t *infos, gboolean all)
{
	print_collections_list (list, infos,
	                        infos->cache->active_playlist_name, all);
}

/* Abstract jump, use inc to choose the direction. */
static void
list_jump_rel (xmmsv_t *val, cli_infos_t *infos, gint inc)
{
	xmmsv_list_iter_t *it;
	gint i, id, currpos, plsize;
	GArray *playlist;

	currpos = infos->cache->currpos;
	plsize = infos->cache->active_playlist->len;
	playlist = infos->cache->active_playlist;

	/* If no currpos, start jump from beginning */
	if (currpos < 0) {
		currpos = 0;
	}

	inc += plsize; /* magic trick so we can loop in either direction */

	xmmsv_get_list_iter (val, &it);

	/* Loop on the playlist */
	for (i = (currpos + inc) % plsize; i != currpos; i = (i + inc) % plsize) {
		/* Loop on the matched media */
		while (xmmsv_list_iter_entry_int (it, &id)) {
			/* If both match, jump! */
			if (g_array_index (playlist, guint, i) == id) {
				XMMS_CALL_CHAIN (XMMS_CALL_P (xmmsc_playlist_set_next, infos->sync, i),
				                 XMMS_CALL_P (xmmsc_playback_tickle, infos->sync));
				return;
			}
			xmmsv_list_iter_next (it);
		}
	}

	/* No matching media found, don't jump */
	g_printf (_("No media matching the pattern in the playlist!\n"));
}

void
list_jump_back (xmmsv_t *res, cli_infos_t *infos)
{
	list_jump_rel (res, infos, -1);
}

void
list_jump (xmmsv_t *res, cli_infos_t *infos)
{
	list_jump_rel (res, infos, 1);
}

/**
 * Add a list of ids to a playlist, starting at a given
 * position.
 */
void
add_list (xmmsv_t *idlist, cli_infos_t *infos,
          const gchar *playlist, gint pos, gint32 *count)

{
	xmmsv_list_iter_t *it;
	gint32 id;

	xmmsv_get_list_iter (idlist, &it);

	while (xmmsv_list_iter_entry_int (it, &id)) {
		XMMS_CALL (xmmsc_playlist_insert_id, infos->sync, playlist, pos++, id);
		xmmsv_list_iter_next (it);
	}

	*count = xmmsv_list_get_size (idlist);
}

void
move_entries (xmmsv_t *matching, xmmsv_t *lisval, cli_infos_t *infos,
              const gchar *playlist, gint pos)
{
	xmmsv_list_iter_t *it;
	gint curr, id, inc;
	gboolean up;
	GTree *list;

	/* store matching mediaids in a tree (faster lookup) */
	list = matching_ids_tree (matching);

	/* move matched playlist items */
	curr = 0;
	inc = 0;
	up = TRUE;

	xmmsv_get_list_iter (lisval, &it);
	while (xmmsv_list_iter_entry_int (it, &id)) {
		if (curr == pos) {
			up = FALSE;
		}
		if (g_tree_lookup (list, &id) != NULL) {
			if (up) {
				/* moving forward */
				XMMS_CALL (xmmsc_playlist_move_entry,
				           infos->sync, playlist, curr - inc, pos - 1);
			} else {
				/* moving backward */
				XMMS_CALL (xmmsc_playlist_move_entry,
				           infos->sync, playlist, curr, pos + inc);
			}
			inc++;
		}
		curr++;

		xmmsv_list_iter_next (it);
	}

	g_tree_destroy (list);
}

static void
pos_move_cb (gint curr, void *userdata)
{
	pl_pos_udata_t *pack = (pl_pos_udata_t *) userdata;

	/* Entries are moved in descending order, pack->inc is used as
	 * offset both for forward and backward moves, and reset
	 * inbetween. */

	if (curr < pack->pos) {
		/* moving forward */
		if (pack->inc >= 0) {
			pack->inc = -1; /* start inc at -1, decrement */
		}
		XMMS_CALL (xmmsc_playlist_move_entry, pack->infos->sync,
		           pack->playlist, curr, pack->pos + pack->inc);
		pack->inc--;
	} else {
		/* moving backward */
		XMMS_CALL (xmmsc_playlist_move_entry, pack->infos->sync,
		           pack->playlist, curr + pack->inc, pack->pos);
		pack->inc++;
	}
}

void
positions_move (cli_infos_t *infos, const gchar *playlist,
                playlist_positions_t *positions, gint pos)
{
	pl_pos_udata_t udata = { infos, NULL, playlist, NULL, 0, pos };
	playlist_positions_foreach (positions, pos_move_cb, FALSE, &udata);
}

void
remove_cached_list (xmmsv_t *matching, cli_infos_t *infos)
{
	/* FIXME: w00t at code copy-paste, please modularize */
	xmmsv_list_iter_t *it;
	guint plid;
	gint32 id;
	gint plsize;
	GArray *playlist;
	gint i;

	plsize = infos->cache->active_playlist->len;
	playlist = infos->cache->active_playlist;

	xmmsv_get_list_iter (matching, &it);

	/* Loop on the playlist (backward, easier to remove) */
	for (i = plsize - 1; i >= 0; i--) {
		plid = g_array_index (playlist, guint, i);

		/* Loop on the matched media */
		while (xmmsv_list_iter_entry_int (it, &id)) {
			/* If both match, remove! */
			if (plid == id) {
				XMMS_CALL (xmmsc_playlist_remove_entry, infos->sync, NULL, i);
				break;
			}

			xmmsv_list_iter_next (it);
		}
	}
}

void
remove_list (xmmsv_t *matchval, xmmsv_t *plistval,
             cli_infos_t *infos, const gchar *playlist)
{
	xmmsv_list_iter_t *matchit, *plistit;
	/* FIXME: w00t at code copy-paste, please modularize */
	gint32 plid, id;
	guint i;
	gint offset;

	/* FIXME: Can we use a GList to remove more safely in the rev order? */
	offset = 0;
	i = 0;

	xmmsv_get_list_iter (matchval, &matchit);
	xmmsv_get_list_iter (plistval, &plistit);

	/* Loop on the playlist */
	while (xmmsv_list_iter_entry_int (plistit, &plid)) {
		/* Loop on the matched media */
		while (xmmsv_list_iter_entry_int (matchit, &id)) {
			/* If both match, jump! */
			if (plid == id) {
				XMMS_CALL (xmmsc_playlist_remove_entry, infos->sync,
				           playlist, i - offset);
				offset++;
				break;
			}
			xmmsv_list_iter_next (matchit);
		}

		i++;
		xmmsv_list_iter_next (plistit);
	}
}

void configure_collection (xmmsv_t *val, cli_infos_t *infos,
                           const gchar *ns, const gchar *name,
                           const gchar *attrname, const gchar *attrvalue)
{
	xmmsv_coll_attribute_set_string (val, attrname, attrvalue);
	coll_save (infos, val, ns, name, TRUE);
}

void
configure_playlist (xmmsv_t *val, cli_infos_t *infos, const gchar *playlist,
                    gint history, gint upcoming, const gchar *typestr,
                    const gchar *input, const gchar *jumplist)
{
	xmmsv_t *newcoll = NULL;

	/* If no type string passed, and collection didn't have any, there's no point
	 * in configuring the other attributes.
	 */
	if (typestr == NULL && !xmmsv_coll_attribute_get_string (val, "type", &typestr))
		return;

	if (typestr) {
		xmmsv_coll_attribute_set_string (val, "type", typestr);
	}
	if (history >= 0) {
		coll_int_attribute_set (val, "history", history);
	}
	if (upcoming >= 0) {
		coll_int_attribute_set (val, "upcoming", upcoming);
	}
	if (input) {
		/* Replace previous operand. */
		newcoll = coll_make_reference (input, XMMS_COLLECTION_NS_COLLECTIONS);
	} else if (typestr && strcmp (typestr, "pshuffle") == 0 &&
	           xmmsv_list_get_size (xmmsv_coll_operands_get (val)) == 0) {
		newcoll = xmmsv_new_coll (XMMS_COLLECTION_TYPE_UNIVERSE);
	}

	if (newcoll) {
		xmmsv_list_clear (xmmsv_coll_operands_get (val));
		xmmsv_coll_add_operand (val, newcoll);
		xmmsv_unref (newcoll);
	}
	if (jumplist) {
		/* FIXME: Check for the existence of the target ? */
		xmmsv_coll_attribute_set_string (val, "jumplist", jumplist);
	}

	XMMS_CALL (xmmsc_coll_save, infos->sync, val, playlist, XMMS_COLLECTION_NS_PLAYLISTS);
}

void
collection_print_config (xmmsv_t *coll, const gchar *attrname)
{
	const gchar *attrvalue;

	if (attrname == NULL) {
		xmmsv_dict_foreach (xmmsv_coll_attributes_get (coll),
		                    coll_print_attributes, NULL);
	} else {
		if (xmmsv_coll_attribute_get_string (coll, attrname, &attrvalue)) {
			g_printf ("[%s] %s\n", attrname, attrvalue);
		} else {
			g_printf (_("Invalid attribute!\n"));
		}
	}
}

gboolean
playlist_exists (cli_infos_t *infos, const gchar *playlist)
{
	gboolean retval = FALSE;
	xmmsc_result_t *res;
	xmmsv_t *val;

	res = xmmsc_coll_get (infos->sync, playlist, XMMS_COLLECTION_NS_PLAYLISTS);
	xmmsc_result_wait (res);

	val = xmmsc_result_get_value (res);

	if (!xmmsv_is_error (val)) {
		retval = TRUE;
	}

	xmmsc_result_unref (res);

	return retval;
}

static void
coll_int_attribute_set (xmmsv_t *coll, const char *key, gint value)
{
	gchar buf[MAX_INT_VALUE_BUFFER_SIZE + 1];

	g_snprintf (buf, MAX_INT_VALUE_BUFFER_SIZE, "%d", value);
	xmmsv_coll_attribute_set_string (coll, key, buf);
}

static xmmsv_t *
coll_make_reference (const char *name, xmmsc_coll_namespace_t ns)
{
	xmmsv_t *ref;

	ref = xmmsv_new_coll (XMMS_COLLECTION_TYPE_REFERENCE);
	xmmsv_coll_attribute_set_string (ref, "reference", name);
	xmmsv_coll_attribute_set_string (ref, "namespace", ns);

	return ref;
}

static void
coll_print_attributes (const char *key, xmmsv_t *val, void *udata)
{
	const char *value;

	if (xmmsv_get_string (val, &value)) {
		g_printf ("[%s] %s\n", key, value);
	}
}

void
playlist_print_config (xmmsv_t *coll, const gchar *name)
{
	const gchar *type, *upcoming, *history, *jumplist;
	xmmsv_t *operands, *operand;

	g_printf (_("name: %s\n"), name);

	if (xmmsv_coll_attribute_get_string (coll, "type", &type))
		g_printf (_("type: %s\n"), type);

	if (xmmsv_coll_attribute_get_string (coll, "history", &history))
		g_printf (_("history: %s\n"), history);

	if (xmmsv_coll_attribute_get_string (coll, "upcoming", &upcoming))
		g_printf (_("upcoming: %s\n"), upcoming);

	operands = xmmsv_coll_operands_get (coll);
	if (xmmsv_list_get (operands, 0, &operand)) {
		if (xmmsv_coll_is_type (operand, XMMS_COLLECTION_TYPE_REFERENCE)) {
			const gchar *input_ns = NULL;
			const gchar *input = NULL;

			xmmsv_coll_attribute_get_string (operand, "namespace", &input_ns);
			xmmsv_coll_attribute_get_string (operand, "reference", &input);

			g_printf (_("input: %s/%s\n"), input_ns, input);
		}
	}

	if (xmmsv_coll_attribute_get_string (coll, "jumplist", &jumplist))
		g_printf (_("jumplist: %s\n"), jumplist);
}

void
print_padding (gint length, const gchar padchar)
{
	while (length-- > 0) {
		g_printf ("%c", padchar);
	}
}

void
print_indented (const gchar *string, guint level)
{
	gboolean indent = TRUE;
	const gchar *c;

	for (c = string; *c; c++) {
		if (indent) {
			print_padding (level, ' ');
			indent = FALSE;
		}
		g_printf ("%c", *c);
		if (*c == '\n') {
			indent = TRUE;
		}
	}
}

/* Returned string must be freed by the caller */
gchar *
format_time (guint64 duration, gboolean use_hours)
{
	guint64 hour, min, sec;
	gchar *time;

	/* +500 for rounding */
	sec = (duration+500) / 1000;
	min = sec / 60;
	sec = sec % 60;

	if (use_hours) {
		hour = min / 60;
		min = min % 60;
		time = g_strdup_printf ("%" G_GUINT64_FORMAT \
		                        ":%02" G_GUINT64_FORMAT \
		                        ":%02" G_GUINT64_FORMAT, hour, min, sec);
	} else {
		time = g_strdup_printf ("%02" G_GUINT64_FORMAT \
		                        ":%02" G_GUINT64_FORMAT, min, sec);
	}

	return time;
}

gchar *
decode_url (const gchar *string)
{
	gint i = 0, j = 0;
	gchar *url;

	url = g_strdup (string);
	if (!url)
		return NULL;

	while (url[i]) {
		guchar chr = url[i++];

		if (chr == '+') {
			chr = ' ';
		} else if (chr == '%') {
			gchar ts[3];
			gchar *t;

			ts[0] = url[i++];
			if (!ts[0])
				goto err;
			ts[1] = url[i++];
			if (!ts[1])
				goto err;
			ts[2] = '\0';

			chr = strtoul (ts, &t, 16);

			if (t != &ts[2])
				goto err;
		}

		url[j++] = chr;
	}

	url[j] = '\0';

	return url;

 err:
	g_free (url);
	return NULL;
}

/* Transform a path (possibly absolute or relative) into a valid XMMS2
 * path with protocol prefix, and applies a file test to it.
 * The resulting string must be freed manually.
 *
 * @return the path in URL format if the test passes, or NULL.
 */
gchar *
format_url (const gchar *path, GFileTest test)
{
	gchar rpath[XMMS_PATH_MAX];
	const gchar *p;
	gchar *url;

	/* Check if path matches "^[a-z]+://" */
	for (p = path; *p >= 'a' && *p <= 'z'; ++p);

	/* If this test passes, path is a valid url and
	 * p points past its "file://" prefix.
	 */
	if (!(*p == ':' && *(++p) == '/' && *(++p) == '/')) {

		/* This is not a valid URL, try to work with
		 * the absolute path.
		 */
		if (!x_realpath (path, rpath)) {
			return NULL;
		}

		if (!g_file_test (rpath, test)) {
			return NULL;
		}

		url = g_strconcat ("file://", rpath, NULL);
	} else {
		url = g_strdup (path);
	}

	return x_path2url (url);
}
