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

#include <xmms_configuration.h>
#include <xmmspriv/xmms_medialib.h>
#include <xmmspriv/xmms_xform.h>
#include <xmmspriv/xmms_utils.h>
#include <xmms/xmms_error.h>
#include <xmms/xmms_config.h>
#include <xmms/xmms_object.h>
#include <xmms/xmms_ipc.h>
#include <xmms/xmms_log.h>


#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <time.h>

#include <xmmspriv/xmms_fetch_info.h>
#include <xmmspriv/xmms_fetch_spec.h>
#include "s4.h"


/**
 * @file
 * Medialib is a metainfo cache that is searchable.
 */


static void xmms_medialib_client_remove_entry (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, xmms_error_t *error);
gchar *xmms_medialib_url_encode (const gchar *path);

static void xmms_medialib_client_add_entry (xmms_medialib_t *, const gchar *, xmms_error_t *);
static void xmms_medialib_client_move_entry (xmms_medialib_t *, gint32 entry, const gchar *, xmms_error_t *);
static void xmms_medialib_client_import_path (xmms_medialib_t *medialib, const gchar *path, xmms_error_t *error);
static void xmms_medialib_client_rehash (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, xmms_error_t *error);
static void xmms_medialib_client_set_property_string (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, const gchar *source, const gchar *key, const gchar *value, xmms_error_t *error);
static void xmms_medialib_client_set_property_int (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, const gchar *source, const gchar *key, gint32 value, xmms_error_t *error);
static void xmms_medialib_client_remove_property (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, const gchar *source, const gchar *key, xmms_error_t *error);
static xmmsv_t *xmms_medialib_client_get_info (xmms_medialib_t *medialib, xmms_medialib_entry_t entry, xmms_error_t *err);
static gint32 xmms_medialib_client_get_id (xmms_medialib_t *medialib, const gchar *url, xmms_error_t *error);

static s4_t *xmms_medialib_database_open (const gchar *config_path, const gchar *indices[]);
static xmms_medialib_entry_t xmms_medialib_entry_new_insert (xmms_medialib_session_t *session, guint32 id, const gchar *url, xmms_error_t *error);

#include "medialib_ipc.c"

/**
 *
 * @defgroup Medialib Medialib
 * @ingroup XMMSServer
 * @brief Medialib caches metadata
 *
 * Controls metadata storage.
 *
 * @{
 */

/**
 * Medialib structure
 */
struct xmms_medialib_St {
	xmms_object_t object;
	s4_t *s4;
	s4_sourcepref_t *default_sp;
};

static void
xmms_medialib_destroy (xmms_object_t *object)
{
	xmms_medialib_t *mlib = (xmms_medialib_t *) object;

	XMMS_DBG ("Deactivating medialib object.");

	s4_sourcepref_unref (mlib->default_sp);
	s4_close (mlib->s4);

	xmms_medialib_unregister_ipc_commands ();
}

#define XMMS_MEDIALIB_SOURCE_SERVER "server"

/**
 * Initialize the medialib and open the database file.
 *
 * @returns TRUE if successful and FALSE if there was a problem
 */

xmms_medialib_t *
xmms_medialib_init (void)
{
	xmms_config_property_t *cfg;
	xmms_medialib_t *medialib;
	const gchar *medialib_path;
	gchar *path;

	const gchar *indices[] = {
		XMMS_MEDIALIB_ENTRY_PROPERTY_URL,
		XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS,
		NULL
	};

	medialib = xmms_object_new (xmms_medialib_t, xmms_medialib_destroy);

	xmms_medialib_register_ipc_commands (XMMS_OBJECT (medialib));

	path = XMMS_BUILD_PATH ("medialib.s4");
	cfg = xmms_config_property_register ("medialib.path", path, NULL, NULL);
	g_free (path);

	path = XMMS_BUILD_PATH ("collections", "${uuid}");
	xmms_config_property_register ("collection.directory", path, NULL, NULL);
	g_free (path);

	xmms_config_property_register ("sqlite2s4.path", "sqlite2s4", NULL, NULL);

	medialib_path = xmms_config_property_get_string (cfg);
	medialib->s4 = xmms_medialib_database_open (medialib_path, indices);
	medialib->default_sp = s4_sourcepref_create (xmmsv_default_source_pref);

	return medialib;
}

s4_sourcepref_t *
xmms_medialib_get_source_preferences (xmms_medialib_t *medialib)
{
	return s4_sourcepref_ref (medialib->default_sp);
}

s4_t *
xmms_medialib_get_database_backend (xmms_medialib_t *medialib)
{
	return medialib->s4;
}

/**
 * Extracts the file name of the old media library
 * and replaces its suffix with .s4
 */
static gchar *
xmms_medialib_database_converted_name (const gchar *conf_path)
{
	gchar *filename, *dirname, *dot, *converted_name, *fullpath;

	dirname = g_path_get_dirname (conf_path);
	filename = g_path_get_basename (conf_path);

	/* nuke the suffix */
	dot = g_strrstr (filename, ".");
	if (dot != NULL) {
		*dot = '\0';
	}

	converted_name = g_strconcat (filename, ".s4", NULL);

	fullpath = g_build_path (G_DIR_SEPARATOR_S, dirname,
	                         converted_name, NULL);

	g_free (converted_name);
	g_free (filename);
	g_free (dirname);

	return fullpath;
}

static s4_t *
xmms_medialib_database_convert (const gchar *database_name,
                                const gchar *indices[])
{
	const gchar *coll_conf, *conv_conf;
	gchar *cmdline, *new_name, *obsolete_name;
	xmms_config_property_t *cfg;
	gint exit_status;
	s4_t *s4;

	cfg = xmms_config_lookup ("collection.directory");
	coll_conf = xmms_config_property_get_string (cfg);

	cfg = xmms_config_lookup ("sqlite2s4.path");
	conv_conf = xmms_config_property_get_string (cfg);

	new_name = xmms_medialib_database_converted_name (database_name);

	cmdline = g_strjoin (" ", conv_conf, database_name,
	                     new_name, coll_conf, NULL);

	xmms_log_info ("Attempting to migrate database to new format.");

	if (!g_spawn_command_line_sync (cmdline, NULL, NULL, &exit_status, NULL) || exit_status) {
		xmms_log_fatal ("Could not run \"%s\", try to run it manually", cmdline);
	}

	g_free (cmdline);

	s4 = s4_open (new_name, indices, 0);
	/* Now we give up */
	if (s4 == NULL) {
		xmms_log_fatal ("Could not open the S4 database");
	}

	xmms_log_info ("Migration successful.");

	/* Move the sqlite database */
	obsolete_name = g_strconcat (database_name, ".obsolete", NULL);
	g_rename (database_name, obsolete_name);
	g_free (obsolete_name);

	/* Update the config path */
	cfg = xmms_config_lookup ("medialib.path");
	xmms_config_property_set_data (cfg, new_name);

	g_free (new_name);

	return s4;
}

static s4_t *
xmms_medialib_database_open (const gchar *database_name,
                             const gchar *indices[])
{
	gint flags = 0;
	s4_t *s4;

	g_return_val_if_fail (database_name, NULL);

	if (strcmp (database_name, "memory://") == 0) {
		flags = S4_MEMORY;
	}

	s4 = s4_open (database_name, indices, flags);
	if (s4 != NULL) {
		return s4;
	}

	if (s4_errno () != S4E_MAGIC) {
		/* The database was a S4 database, but still couldn't be opened. */
		xmms_log_fatal ("Could not open the S4 database");
	}

	/* Seems like we've found a SQLite database, lets convert it. */
	return xmms_medialib_database_convert (database_name, indices);
}

char *
xmms_medialib_uuid (xmms_medialib_t *medialib)
{
	return s4_get_uuid_string (medialib->s4);
}

static s4_resultset_t *
xmms_medialib_filter (xmms_medialib_session_t *session,
                      const gchar *filter_key, const s4_val_t *filter_val,
                      gint filter_flags, s4_sourcepref_t *sourcepref,
                      const gchar *fetch_key, gint fetch_flags)
{
	s4_condition_t *cond;
	s4_fetchspec_t *spec;
	s4_resultset_t *ret;
	s4_cmp_mode_t cmp;

	/* Case-sensitive keys */
	if (strcmp (filter_key, XMMS_MEDIALIB_ENTRY_PROPERTY_URL) == 0) {
		cmp = S4_CMP_BINARY;
	} else {
		/* Everything else is case-insensitive */
		cmp = S4_CMP_CASELESS;
	}

	cond = s4_cond_new_filter (S4_FILTER_EQUAL, filter_key, filter_val,
	                           sourcepref, cmp, filter_flags);

	spec = s4_fetchspec_create ();
	s4_fetchspec_add (spec, fetch_key, sourcepref, fetch_flags);
	ret = xmms_medialib_session_query (session, spec, cond);

	s4_cond_free (cond);
	s4_fetchspec_free (spec);

	return ret;
}

static s4_val_t *
xmms_medialib_entry_property_get (xmms_medialib_session_t *session,
                                  xmms_medialib_entry_t entry,
                                  const gchar *property)
{
	s4_sourcepref_t *sourcepref;
	const s4_result_t *res;
	s4_resultset_t *set;
	s4_val_t *ret = NULL;
	s4_val_t *song_id;

	g_return_val_if_fail (property, NULL);

	song_id = s4_val_new_int (entry);

	if (strcmp (property, XMMS_MEDIALIB_ENTRY_PROPERTY_ID) == 0) {
		/* only resolving attributes other than 'id' */
		return song_id;
	}

	sourcepref = xmms_medialib_session_get_source_preferences (session);

	set = xmms_medialib_filter (session, "song_id", song_id, S4_COND_PARENT,
	                            sourcepref, property, S4_FETCH_DATA);

	s4_sourcepref_unref (sourcepref);

	res = s4_resultset_get_result (set, 0, 0);
	if (res != NULL) {
		ret = s4_val_copy (s4_result_get_val (res));
	}

	s4_resultset_free (set);
	s4_val_free (song_id);

	return ret;
}


/**
 * Retrieve a property from an entry
 *
 * @see xmms_medialib_entry_property_get_str
 */

xmmsv_t *
xmms_medialib_entry_property_get_value (xmms_medialib_session_t *session,
                                        xmms_medialib_entry_t id_num,
                                        const gchar *property)
{
	xmmsv_t *ret = NULL;
	s4_val_t *prop;
	const gchar *s;
	gint32 i;

	prop = xmms_medialib_entry_property_get (session, id_num, property);
	if (prop == NULL)
		return NULL;

	if (s4_val_get_str (prop, &s)) {
		ret = xmmsv_new_string (s);
	} else if (s4_val_get_int (prop, &i)) {
		ret = xmmsv_new_int (i);
	}

	s4_val_free (prop);

	return ret;
}

/**
 * Retrieve a property from an entry.
 *
 * @param entry Entry to query.
 * @param property The property to extract. Strings passed should
 * be defined in medialib.h
 *
 * @returns Newly allocated gchar that needs to be freed with g_free
 */

gchar *
xmms_medialib_entry_property_get_str (xmms_medialib_session_t *session,
                                      xmms_medialib_entry_t entry,
                                      const gchar *property)
{
	const gchar *string;
	gchar *result = NULL;
	s4_val_t *value;

	value = xmms_medialib_entry_property_get (session, entry, property);
	if (value != NULL && s4_val_get_str (value, &string)) {
		result = g_strdup (string);
	}
	s4_val_free (value);

	return result;
}

/**
 * Retrieve a property as a int from a entry.
 *
 * @param id_num Entry to query.
 * @param property The property to extract. Strings passed should
 * be defined in medialib.h
 *
 * @returns Property as integer, or -1 if it doesn't exist.
 */
gint
xmms_medialib_entry_property_get_int (xmms_medialib_session_t *session,
                                      xmms_medialib_entry_t id_num,
                                      const gchar *property)
{
	gint32 ret;
	s4_val_t *prop;

	prop = xmms_medialib_entry_property_get (session, id_num, property);
	if (prop == NULL) {
		return -1;
	}

	if (!s4_val_get_int (prop, &ret)) {
		ret = -1;
	}

	s4_val_free (prop);

	return ret;
}

/**
 * Set a entry property to a new value, overwriting the old value.
 *
 * @param entry Entry to alter.
 * @param property The property to extract. Strings passed should
 * be defined in medialib.h
 * @param value gint with the new value, will be copied in to the medialib
 *
 * @returns TRUE on success and FALSE on failure.
 */
gboolean
xmms_medialib_entry_property_set_int (xmms_medialib_session_t *session,
                                      xmms_medialib_entry_t entry,
                                      const gchar *property, gint value)
{
	return xmms_medialib_entry_property_set_int_source (session,
	                                                    entry,
	                                                    property, value,
	                                                    "server");
}


gboolean
xmms_medialib_entry_property_set_int_source (xmms_medialib_session_t *session,
                                             xmms_medialib_entry_t id_num,
                                             const gchar *property, gint value,
                                             const gchar *source)
{
	gboolean ret;
	s4_val_t *prop;

	g_return_val_if_fail (property, FALSE);

	prop = s4_val_new_int (value);
	ret = xmms_medialib_session_property_set (session, id_num, property, prop, source);
	s4_val_free (prop);

	return ret;
}

/**
 * Set a entry property to a new value, overwriting the old value.
 *
 * @param entry Entry to alter.
 * @param property The property to extract. Strings passed should
 * be defined in medialib.h
 * @param value gchar with the new value, will be copied in to the medialib
 *
 * @returns TRUE on success and FALSE on failure.
 */
gboolean
xmms_medialib_entry_property_set_str (xmms_medialib_session_t *session,
                                      xmms_medialib_entry_t entry,
                                      const gchar *property, const gchar *value)
{
	return xmms_medialib_entry_property_set_str_source (session,
	                                                    entry,
	                                                    property, value,
	                                                    "server");
}


gboolean
xmms_medialib_entry_property_set_str_source (xmms_medialib_session_t *session,
                                             xmms_medialib_entry_t id_num,
                                             const gchar *property, const gchar *value,
                                             const gchar *source)
{
	gboolean ret;
	s4_val_t *prop;

	g_return_val_if_fail (property, FALSE);

	if (value && !g_utf8_validate (value, -1, NULL)) {
		XMMS_DBG ("OOOOOPS! Trying to set property %s to a NON UTF-8 string (%s) I will deny that!", property, value);
		return FALSE;
	}

	prop = s4_val_new_string (value);
	ret = xmms_medialib_session_property_set (session, id_num, property, prop, source);
	s4_val_free (prop);

	return ret;

}


/* A filter to find the highest id. It always returns -1, thus the
 * search algorithm will ultimately end up passing it the highest value.
 */
static gint
highest_id_filter (const s4_val_t *value, s4_condition_t *cond)
{
	gint32 *i = s4_cond_get_funcdata (cond);
	gint32 ival;

	if (s4_val_get_int (value, &ival)) {
		*i = ival;
	}

	return -1;
}

/**
 * Return a fresh unused medialib id.
 *
 * The first id starts at 1 as 0 is considered reserved for other use.
 */
static int32_t
xmms_medialib_get_new_id (xmms_medialib_session_t *session)
{
	gint32 highest = 0;
	s4_fetchspec_t *fs;
	s4_condition_t *cond;
	s4_resultset_t *set;
	s4_sourcepref_t *sourcepref;

	sourcepref = xmms_medialib_session_get_source_preferences (session);
	cond = s4_cond_new_custom_filter (highest_id_filter, &highest, NULL,
	                                  "song_id", sourcepref, 0, 1, S4_COND_PARENT);
	s4_sourcepref_unref (sourcepref);

	fs = s4_fetchspec_create ();
	set = xmms_medialib_session_query (session, fs, cond);

	s4_resultset_free (set);
	s4_cond_free (cond);
	s4_fetchspec_free (fs);

	return highest + 1;
}


static void
xmms_medialib_client_remove_entry (xmms_medialib_t *medialib,
                                   xmms_medialib_entry_t entry,
                                   xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_entry_remove (session, entry);
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));
}

/**
 * Remove a medialib entry from the database
 *
 * @param id_num Entry to remove
 */
void
xmms_medialib_entry_remove (xmms_medialib_session_t *session,
                            xmms_medialib_entry_t entry)
{
	s4_resultset_t *set;
	s4_val_t *song_id;
	gint i;

	song_id = s4_val_new_int (entry);

	set = xmms_medialib_filter (session, "song_id", song_id,
	                            S4_COND_PARENT, NULL, NULL, S4_FETCH_DATA);

	for (i = 0; i < s4_resultset_get_rowcount (set); i++) {
		const s4_result_t *res;

		res = s4_resultset_get_result (set, i, 0);
		while (res != NULL) {
			xmms_medialib_session_property_unset (session, entry,
			                                      s4_result_get_key (res),
			                                      s4_result_get_val (res),
			                                      s4_result_get_src (res));
			res = s4_result_next (res);
		}
	}

	s4_resultset_free (set);
	s4_val_free (song_id);
}

/**
 * Check if a (source, key) tuple can be derived from a re-scan of the media.
 *
 * This is used to protect data that comes when importing attributes
 * from a playlist, or attributes set by a client.
 */
static gboolean
entry_attribute_is_derived (const gchar *source, const gchar *key)
{
	if (strcmp (XMMS_MEDIALIB_SOURCE_SERVER, source) == 0) {
		if (strcmp (key, XMMS_MEDIALIB_ENTRY_PROPERTY_URL) == 0)
			return FALSE;
		if (strcmp (key, XMMS_MEDIALIB_ENTRY_PROPERTY_ADDED) == 0)
			return FALSE;
		if (strcmp (key, XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS) == 0)
			return FALSE;
		if (strcmp (key, XMMS_MEDIALIB_ENTRY_PROPERTY_LMOD) == 0)
			return FALSE;
		if (strcmp (key, XMMS_MEDIALIB_ENTRY_PROPERTY_LASTSTARTED) == 0)
			return FALSE;
		return TRUE;
	}

	if (strncmp (source, "plugin/", 7) == 0) {
		if (strcmp (source, "plugin/playlist") == 0) {
			return FALSE;
		}
		return TRUE;
	}

	return FALSE;
}


void
xmms_medialib_entry_cleanup (xmms_medialib_session_t *session,
                             xmms_medialib_entry_t entry)
{
	s4_resultset_t *set;
	s4_val_t *song_id;
	gint i;

	song_id = s4_val_new_int (entry);

	set = xmms_medialib_filter (session, "song_id", song_id,
	                            S4_COND_PARENT, NULL, NULL, S4_FETCH_DATA);


	for (i = 0; i < s4_resultset_get_rowcount (set); i++) {
		const s4_result_t *res;

		res = s4_resultset_get_result (set, i, 0);
		while (res != NULL) {
			const gchar *src = s4_result_get_src (res);
			const gchar *key = s4_result_get_key (res);

			if (entry_attribute_is_derived (src, key)) {
				const s4_val_t *value = s4_result_get_val (res);
				xmms_medialib_session_property_unset (session, entry, key,
				                                      value, src);
			}

			res = s4_result_next (res);
		}
	}

	s4_resultset_free (set);
	s4_val_free (song_id);
}

static void
xmms_medialib_client_rehash (xmms_medialib_t *medialib,
                             xmms_medialib_entry_t entry,
                             xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_entry_status_set (session, entry, XMMS_MEDIALIB_ENTRY_STATUS_REHASH);
		} else if (entry == 0) {
			s4_sourcepref_t *sourcepref;
			s4_resultset_t *set;
			s4_val_t *status;
			gint i;

			sourcepref = xmms_medialib_session_get_source_preferences (session);

			status = s4_val_new_int (XMMS_MEDIALIB_ENTRY_STATUS_OK);
			set = xmms_medialib_filter (session, XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS,
			                            status, 0, sourcepref, "song_id", S4_FETCH_PARENT);
			s4_val_free (status);
			s4_sourcepref_unref (sourcepref);

			for (i = 0; i < s4_resultset_get_rowcount (set); i++) {
				const s4_result_t *res;

				res = s4_resultset_get_result (set, i, 0);
				for (; res != NULL; res = s4_result_next (res)) {
					xmms_medialib_entry_t item;
					s4_val_get_int (s4_result_get_val (res), &item);
					xmms_medialib_entry_status_set (session, item, XMMS_MEDIALIB_ENTRY_STATUS_REHASH);
				}
			}

			s4_resultset_free (set);
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));
}

/**
 * Recursively scan a directory for media files.
 *
 * @return a reverse sorted list of encoded urls
 */
static gboolean
process_dir (xmms_medialib_t *medialib, xmmsv_t *entries,
             const gchar *directory, xmms_error_t *error)
{
	xmmsv_list_iter_t *it;
	xmmsv_t *list, *val;

	list = xmms_xform_browse (directory, error);
	if (!list) {
		return FALSE;
	}

	xmmsv_get_list_iter (list, &it);

	while (xmmsv_list_iter_entry (it, &val)) {
		const gchar *str;
		gint isdir;

		xmmsv_dict_entry_get_string (val, "path", &str);
		xmmsv_dict_entry_get_int (val, "isdir", &isdir);

		if (isdir == 1) {
			process_dir (medialib, entries, str, error);
		} else {
			xmms_medialib_session_t *session;
			xmms_medialib_entry_t entry;

			do {
				session = xmms_medialib_session_begin (medialib);
				entry = xmms_medialib_entry_new_encoded (session, str, error);
			} while (!xmms_medialib_session_commit (session));

			if (entry) {
				xmmsv_coll_idlist_append (entries, entry);
			}
		}

		xmmsv_list_iter_remove (it);
	}

	xmmsv_unref (list);

	return TRUE;
}

/**
 * Recursively add files under a path to the media library.
 *
 * @param medialib the medialib object
 * @param path the directory to scan for files
 * @param error If an error occurs, it will be stored in there.
 *
 * @return an IDLIST collection with the added entries
 */
xmmsv_t *
xmms_medialib_add_recursive (xmms_medialib_t *medialib, const gchar *path,
                             xmms_error_t *error)
{
	xmmsv_t *entries;

	entries = xmmsv_new_coll (XMMS_COLLECTION_TYPE_IDLIST);

	g_return_val_if_fail (medialib, entries);
	g_return_val_if_fail (path, entries);

	process_dir (medialib, entries, path, error);

	return entries;
}

static void
xmms_medialib_client_import_path (xmms_medialib_t *medialib, const gchar *path,
                                  xmms_error_t *error)
{
	xmmsv_unref (xmms_medialib_add_recursive (medialib, path, error));
}

static gboolean
xmms_medialib_entry_new_insert (xmms_medialib_session_t *session,
                                guint32 id,
                                const char *url,
                                xmms_error_t *error)
{
	if (!xmms_medialib_entry_property_set_str (session, id, XMMS_MEDIALIB_ENTRY_PROPERTY_URL, url)) {
		return FALSE;
	}

	xmms_medialib_entry_status_set (session, id, XMMS_MEDIALIB_ENTRY_STATUS_NEW);

	return TRUE;
}

/**
 * @internal
 */
static xmms_medialib_entry_t
xmms_medialib_get_id (xmms_medialib_session_t *session, const char *url, xmms_error_t *error)
{
	s4_sourcepref_t *sourcepref;
	s4_resultset_t *set;
	s4_val_t *value;
	const s4_result_t *res;
	gint32 id = 0;

	sourcepref = xmms_medialib_session_get_source_preferences (session);

	value = s4_val_new_string (url);
	set = xmms_medialib_filter (session, XMMS_MEDIALIB_ENTRY_PROPERTY_URL,
	                            value, 0, sourcepref, "song_id", S4_FETCH_PARENT);
	s4_val_free (value);
	s4_sourcepref_unref (sourcepref);

	res = s4_resultset_get_result (set, 0, 0);
	if (res != NULL) {
		s4_val_get_int (s4_result_get_val (res), &id);
	}

	s4_resultset_free (set);

	return id;
}

xmms_medialib_entry_t
xmms_medialib_entry_new_encoded (xmms_medialib_session_t *session,
                                 const gchar *url, xmms_error_t *error)
{
	guint ret = 0;

	g_return_val_if_fail (url, 0);

	ret = xmms_medialib_get_id (session, url, error);

	if (ret == 0) {
		ret = xmms_medialib_get_new_id (session);

		if (!xmms_medialib_entry_new_insert (session, ret, url, error)) {
			return 0;
		}
	}

	return ret;

}

/**
 * Welcome to a function that should be called something else.
 * Returns a entry for a URL, if the URL is already in the medialib
 * the current entry will be returned otherwise a new one will be
 * created and returned.
 *
 * @todo rename to something better?
 *
 * @param url URL to add/retrieve from the medialib
 * @param error If an error occurs, it will be stored in there.
 *
 * @returns Entry mapped to the URL
 */
xmms_medialib_entry_t
xmms_medialib_entry_new (xmms_medialib_session_t *session,
                         const char *url, xmms_error_t *error)
{
	gchar *enc_url;
	xmms_medialib_entry_t res;

	enc_url = xmms_medialib_url_encode (url);
	if (!enc_url) {
		return 0;
	}

	res = xmms_medialib_entry_new_encoded (session, enc_url, error);

	g_free (enc_url);

	return res;
}

gint32
xmms_medialib_client_get_id (xmms_medialib_t *medialib, const gchar *url,
                             xmms_error_t *error)
{
	xmms_medialib_session_t *session;
	gint32 ret;

	do {
		session = xmms_medialib_session_begin_ro (medialib);
		ret = xmms_medialib_get_id (session, url, error);
	} while (!xmms_medialib_session_commit (session));

	return ret;
}

static void
xmms_medialib_tree_add_tuple (xmmsv_t *dict, const char *key,
                              const char *source, xmmsv_t *value)
{
	xmmsv_t *entry;

	if (key == NULL || source == NULL || value == NULL) {
		return;
	}

	/* Find (or insert) subtree matching the prop key */
	if (!xmmsv_dict_get (dict, key, &entry)) {
		entry = xmmsv_new_dict ();
		xmmsv_dict_set (dict, key, entry);
		xmmsv_unref (entry);
	}

	/* Replace (or insert) value matching the prop source */
	xmmsv_dict_set (entry, source, value);
}

/**
 * Convert a entry and all properties to a key-source-value tree that
 * could be feed to the client or somewhere else in the daemon.
 *
 * @param session The medialib session to be used for the transaction.
 * @param entry Entry to convert.
 *
 * @returns Newly allocated tree with newly allocated strings
 * make sure to free them all.
 */

static xmmsv_t *
xmms_medialib_entry_to_tree (xmms_medialib_session_t *session,
                             xmms_medialib_entry_t entry)
{
	s4_resultset_t *set;
	s4_val_t *song_id;
	xmmsv_t *ret, *id;
	gint i;

	song_id = s4_val_new_int (entry);
	set = xmms_medialib_filter (session, "song_id", song_id, S4_COND_PARENT,
	                            NULL, NULL, S4_FETCH_PARENT | S4_FETCH_DATA);
	s4_val_free (song_id);

	ret = xmmsv_new_dict ();

	for (i = 0; i < s4_resultset_get_rowcount (set); i++) {
		const s4_result_t *res;

		res = s4_resultset_get_result (set, 0, 0);
		while (res != NULL) {
			xmmsv_t *v_entry = NULL;
			const s4_val_t *val;
			const char *s;
			gint32 i;

			val = s4_result_get_val (res);
			if (s4_val_get_str (val, &s)) {
				v_entry = xmmsv_new_string (s);
			} else if (s4_val_get_int (val, &i)) {
				v_entry = xmmsv_new_int (i);
			}

			xmms_medialib_tree_add_tuple (ret, s4_result_get_key (res),
			                              s4_result_get_src (res), v_entry);
			xmmsv_unref (v_entry);

			res = s4_result_next (res);
		}
	}

	s4_resultset_free (set);

	id = xmmsv_new_int (entry);
	xmms_medialib_tree_add_tuple (ret, "id", "server", id);
	xmmsv_unref (id);

	return ret;
}

static xmmsv_t *
xmms_medialib_client_get_info (xmms_medialib_t *medialib,
                               xmms_medialib_entry_t entry,
                               xmms_error_t *err)
{
	xmms_medialib_session_t *session;
	xmmsv_t *ret = NULL;

	do {
		session = xmms_medialib_session_begin_ro (medialib);
		if (xmms_medialib_check_id (session, entry)) {
			ret = xmms_medialib_entry_to_tree (session, entry);
		} else {
			xmms_error_set (err, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));

	return ret;
}

/**
 * Add a entry to the medialib. Calls #xmms_medialib_entry_new and then
 * wakes up the mediainfo_reader in order to resolve the metadata.
 *
 * @param medialib Medialib pointer
 * @param url URL to add
 * @param error In case of error this will be filled.
 */

static void
xmms_medialib_client_add_entry (xmms_medialib_t *medialib, const gchar *url,
                                xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	g_return_if_fail (medialib);
	g_return_if_fail (url);

	do {
		session = xmms_medialib_session_begin (medialib);
		xmms_medialib_entry_new_encoded (session, url, error);
	} while (!xmms_medialib_session_commit (session));
}

/**
 * Changes the URL of an entry in the medialib.
 *
 * @param medialib Medialib pointer
 * @param entry entry to modify
 * @param url URL to change to
 * @param error In case of error this will be filled.
 */
static void
xmms_medialib_client_move_entry (xmms_medialib_t *medialib,
                                 xmms_medialib_entry_t entry,
                                 const gchar *url, xmms_error_t *error)
{
	xmms_medialib_session_t *session;
	gchar *encoded;

	encoded = xmms_medialib_url_encode (url);

	do {
		session = xmms_medialib_session_begin (medialib);
		if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_entry_property_set_str_source (session, entry,
			                                             XMMS_MEDIALIB_ENTRY_PROPERTY_URL,
			                                             encoded, "server");
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));

	g_free (encoded);
}

static void
xmms_medialib_client_set_property_string (xmms_medialib_t *medialib,
                                          xmms_medialib_entry_t entry,
                                          const gchar *source, const gchar *key,
                                          const gchar *value, xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (g_ascii_strcasecmp (source, "server") == 0) {
			xmms_error_set (error, XMMS_ERROR_GENERIC, "Can't write to source server!");
		} else if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_entry_property_set_str_source (session, entry, key,
			                                             value, source);
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));
}

static void
xmms_medialib_client_set_property_int (xmms_medialib_t *medialib,
                                       xmms_medialib_entry_t entry,
                                       const gchar *source, const gchar *key,
                                       gint32 value, xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (g_ascii_strcasecmp (source, "server") == 0) {
			xmms_error_set (error, XMMS_ERROR_GENERIC, "Can't write to source server!");
		} else if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_entry_property_set_int_source (session, entry, key,
			                                             value, source);
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));
}

static void
xmms_medialib_property_remove (xmms_medialib_session_t *session,
                               xmms_medialib_entry_t entry,
                               const gchar *source, const gchar *key,
                               xmms_error_t *error)
{
	const char *sources[2] = { source, NULL };
	s4_sourcepref_t *sp;
	s4_resultset_t *set;
	const s4_result_t *res;
	s4_val_t *song_id;

	sp = s4_sourcepref_create (sources);

	song_id = s4_val_new_int (entry);
	set = xmms_medialib_filter (session, "song_id", song_id,
	                            S4_COND_PARENT, sp, key, S4_FETCH_DATA);

	res = s4_resultset_get_result (set, 0, 0);
	if (res != NULL) {
		xmms_medialib_session_property_unset (session, entry, key,
		                                      s4_result_get_val (res),
		                                      source);
	}

	s4_resultset_free (set);
	s4_sourcepref_unref (sp);
	s4_val_free (song_id);
}

static void
xmms_medialib_client_remove_property (xmms_medialib_t *medialib,
                                      xmms_medialib_entry_t entry,
                                      const gchar *source, const gchar *key,
                                      xmms_error_t *error)
{
	xmms_medialib_session_t *session;

	do {
		session = xmms_medialib_session_begin (medialib);
		if (g_ascii_strcasecmp (source, "server") == 0) {
			xmms_error_set (error, XMMS_ERROR_GENERIC, "Can't remove properties set by the server!");
		} else if (xmms_medialib_check_id (session, entry)) {
			xmms_medialib_property_remove (session, entry, source, key, error);
		} else {
			xmms_error_set (error, XMMS_ERROR_NOENT, "No such entry");
		}
	} while (!xmms_medialib_session_commit (session));
}

/** @} */

/**
 * @internal
 */

gboolean
xmms_medialib_check_id (xmms_medialib_session_t *session,
                        xmms_medialib_entry_t id)
{
	xmmsv_t *val;

	val = xmms_medialib_entry_property_get_value (session, id, XMMS_MEDIALIB_ENTRY_PROPERTY_URL);
	if (val == NULL) {
		return FALSE;
	}

	xmmsv_unref (val);

	return TRUE;
}


/**
 * Returns a random entry from a collection
 *
 * @param coll The collection to find a random entry in
 * @return A random entry from the collection, 0 if the collection is empty
 */
xmms_medialib_entry_t
xmms_medialib_query_random_id (xmms_medialib_session_t *session,
                               xmmsv_t *coll)
{
	xmms_medialib_entry_t ret;
	xmmsv_t *spec, *res;
	xmms_error_t err;

	spec = xmmsv_build_list (XMMSV_LIST_ENTRY_STR ("id"),
	                         XMMSV_LIST_END);

	spec = xmmsv_build_dict (XMMSV_DICT_ENTRY_STR ("type", "metadata"),
	                         XMMSV_DICT_ENTRY_STR ("aggregate", "random"),
	                         XMMSV_DICT_ENTRY ("get", spec),
	                         XMMSV_DICT_END);

	res = xmms_medialib_query (session, coll, spec, &err);
	xmmsv_get_int (res, &ret);

	xmmsv_unref (spec);
	xmmsv_unref (res);

	return ret;
}

/**
 * @internal
 * Get the next unresolved entry. Used by the mediainfo reader..
 */

static s4_resultset_t *
not_resolved_set (xmms_medialib_session_t *session)
{
	s4_condition_t *cond1, *cond2, *cond;
	s4_sourcepref_t *sourcepref;
	s4_fetchspec_t *spec;
	s4_resultset_t *ret;
	s4_val_t *v1, *v2;

	sourcepref = xmms_medialib_session_get_source_preferences (session);

	v1 = s4_val_new_int (XMMS_MEDIALIB_ENTRY_STATUS_NEW);
	v2 = s4_val_new_int (XMMS_MEDIALIB_ENTRY_STATUS_REHASH);

	cond1 = s4_cond_new_filter (S4_FILTER_EQUAL,
	                            XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS,
	                            v1, sourcepref, S4_CMP_CASELESS, 0);
	cond2 = s4_cond_new_filter (S4_FILTER_EQUAL,
	                            XMMS_MEDIALIB_ENTRY_PROPERTY_STATUS,
	                            v2, sourcepref, S4_CMP_CASELESS, 0);

	cond = s4_cond_new_combiner (S4_COMBINE_OR);
	s4_cond_add_operand (cond, cond1);
	s4_cond_add_operand (cond, cond2);

	spec = s4_fetchspec_create ();
	s4_fetchspec_add (spec, "song_id", sourcepref, S4_FETCH_PARENT);


	ret = xmms_medialib_session_query (session, spec, cond);

	s4_sourcepref_unref (sourcepref);
	s4_fetchspec_free (spec);
	s4_cond_free (cond);
	s4_cond_free (cond1);
	s4_cond_free (cond2);
	s4_val_free (v1);
	s4_val_free (v2);

	return ret;
}

xmms_medialib_entry_t
xmms_medialib_entry_not_resolved_get (xmms_medialib_session_t *session)
{
	const s4_result_t *res;
	s4_resultset_t *set;
	gint32 ret = 0;

	set = not_resolved_set (session);
	res = s4_resultset_get_result (set, 0, 0);

	if (res != NULL) {
		s4_val_get_int (s4_result_get_val (res), &ret);
	}

	s4_resultset_free (set);

	return ret;
}

guint
xmms_medialib_num_not_resolved (xmms_medialib_session_t *session)
{
	s4_resultset_t *set;
	gint ret = 0;

	set = not_resolved_set (session);
	ret = s4_resultset_get_rowcount (set);
	s4_resultset_free (set);

	return ret;
}


gboolean
xmms_medialib_decode_url (gchar *url)
{
	gint i = 0, j = 0;

	g_return_val_if_fail (url, TRUE);

	while (url[i]) {
		guchar chr = url[i++];

		if (chr == '+') {
			url[j++] = ' ';
		} else if (chr == '%') {
			char ts[3];
			char *t;

			ts[0] = url[i++];
			if (!ts[0])
				return FALSE;
			ts[1] = url[i++];
			if (!ts[1])
				return FALSE;
			ts[2] = '\0';

			url[j++] = strtoul (ts, &t, 16);
			if (t != &ts[2])
				return FALSE;
		} else {
			url[j++] = chr;
		}
	}

	url[j] = '\0';

	return TRUE;
}


#define GOODCHAR(a) ((((a) >= 'a') && ((a) <= 'z')) || \
                     (((a) >= 'A') && ((a) <= 'Z')) || \
                     (((a) >= '0') && ((a) <= '9')) || \
                     ((a) == ':') || \
                     ((a) == '/') || \
                     ((a) == '-') || \
                     ((a) == '.') || \
                     ((a) == '_'))

/* we don't share code here with medialib because we want to use g_malloc :( */
gchar *
xmms_medialib_url_encode (const gchar *path)
{
	static gchar hex[16] = "0123456789abcdef";
	gchar *res;
	gint i = 0, j = 0;

	res = g_malloc (strlen (path) * 3 + 1);
	if (!res) {
		return NULL;
	}

	while (path[i]) {
		guchar chr = path[i++];
		if (GOODCHAR (chr)) {
			res[j++] = chr;
		} else if (chr == ' ') {
			res[j++] = '+';
		} else {
			res[j++] = '%';
			res[j++] = hex[((chr & 0xf0) >> 4)];
			res[j++] = hex[(chr & 0x0f)];
		}
	}

	res[j] = '\0';

	return res;
}


/**
 * Queries the medialib and returns an xmmsv_t with the info requested
 *
 * @param coll The collection to find
 * @param fetch Specifies what to fetch
 * @return An xmmsv_t with the structure requested in fetch
 */
xmmsv_t *
xmms_medialib_query (xmms_medialib_session_t *session, xmmsv_t *coll,
                     xmmsv_t *fetch, xmms_error_t *err)
{
	s4_sourcepref_t *sourcepref;
	s4_resultset_t *set;
	xmmsv_t *ret;
	xmms_fetch_info_t *info;
	xmms_fetch_spec_t *spec;

	xmms_error_reset (err);

	sourcepref = xmms_medialib_session_get_source_preferences (session);

	info = xmms_fetch_info_new (sourcepref);
	spec = xmms_fetch_spec_new (fetch, info, sourcepref, err);

	s4_sourcepref_unref (sourcepref);

	if (spec == NULL) {
		xmms_fetch_spec_free (spec);
		xmms_fetch_info_free (info);
		return NULL;
	}

	set = xmms_medialib_query_recurs (session, coll, info);
	ret = xmms_medialib_query_to_xmmsv (set, spec);
	s4_resultset_free (set);

	xmms_fetch_spec_free (spec);
	xmms_fetch_info_free (info);

	if (ret == NULL) {
		if (err) {
			xmms_error_set (err, XMMS_ERROR_NOENT, "Failed to retrieve query "
			                "result. This is probably a bug in xmms2d.");
		}
		return NULL;
	}

	xmms_medialib_session_track_garbage (session, ret);

	return ret;
}
