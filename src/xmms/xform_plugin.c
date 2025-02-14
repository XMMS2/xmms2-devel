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

#include <xmmspriv/xmms_xform.h>
#include <xmmspriv/xmms_xform_plugin.h>
#include <xmmspriv/xmms_metadata_mapper.h>
#include <xmms/xmms_log.h>

struct xmms_xform_plugin_St {
	xmms_plugin_t plugin;
	xmms_xform_methods_t methods;
	GHashTable *metadata_mapper;
	GList *in_types;
	xmms_stream_type_t *default_out_type;
};

static void
destroy (xmms_object_t *obj)
{
	xmms_xform_plugin_t *plugin = (xmms_xform_plugin_t *) obj;

	g_list_free_full (plugin->in_types, xmms_object_unref);
	if (plugin->default_out_type) {
		xmms_object_unref (plugin->default_out_type);
	}

	if (plugin->metadata_mapper != NULL) {
		g_hash_table_unref (plugin->metadata_mapper);
	}

	xmms_plugin_destroy ((xmms_plugin_t *) obj);
}

xmms_plugin_t *
xmms_xform_plugin_new (void)
{
	xmms_xform_plugin_t *res;

	res = xmms_object_new (xmms_xform_plugin_t, destroy);

	return (xmms_plugin_t *)res;
}

void
xmms_xform_plugin_methods_set (xmms_xform_plugin_t *plugin,
                               xmms_xform_methods_t *methods)
{
	g_return_if_fail (plugin);
	g_return_if_fail (plugin->plugin.type == XMMS_PLUGIN_TYPE_XFORM);

	XMMS_DBG ("Registering xform '%s'",
	          xmms_plugin_shortname_get ((xmms_plugin_t *) plugin));

	memcpy (&plugin->methods, methods, sizeof (xmms_xform_methods_t));
}

gboolean
xmms_xform_plugin_verify (xmms_plugin_t *_plugin)
{
	xmms_xform_plugin_t *plugin = (xmms_xform_plugin_t *) _plugin;

	g_return_val_if_fail (plugin, FALSE);
	g_return_val_if_fail (plugin->plugin.type == XMMS_PLUGIN_TYPE_XFORM, FALSE);

	/* more checks */

	return TRUE;
}

void
xmms_xform_plugin_indata_add (xmms_xform_plugin_t *plugin, ...)
{
	xmms_stream_type_t *t;
	va_list ap;
	gchar *config_key, config_value[32];
	gint priority;

	va_start (ap, plugin);
	t = xmms_stream_type_parse (ap);
	va_end (ap);

	config_key = g_strconcat ("priority.",
	                          xmms_stream_type_get_str (t, XMMS_STREAM_TYPE_NAME),
	                          NULL);
	priority = xmms_stream_type_get_int (t, XMMS_STREAM_TYPE_PRIORITY);
	g_snprintf (config_value, sizeof (config_value), "%d", priority);
	xmms_xform_plugin_config_property_register (plugin, config_key,
	                                            config_value, NULL, NULL);
	g_free (config_key);

	plugin->in_types = g_list_prepend (plugin->in_types, t);
}

void
xmms_xform_plugin_set_out_stream_type (xmms_xform_plugin_t *plugin, ...)
{
	va_list ap;

	va_start (ap, plugin);
	plugin->default_out_type = xmms_stream_type_parse (ap);
	va_end (ap);
}


xmms_stream_type_t *
xmms_xform_plugin_get_out_stream_type (xmms_xform_plugin_t *plugin)
{
	return plugin->default_out_type;
}

gboolean
xmms_xform_plugin_supports (const xmms_xform_plugin_t *plugin, const xmms_stream_type_t *st,
                            gint *priority)
{
	GList *t;

	g_return_val_if_fail (st, FALSE);
	g_return_val_if_fail (plugin, FALSE);
	g_return_val_if_fail (priority, FALSE);

	for (t = plugin->in_types; t; t = g_list_next (t)) {
		xmms_config_property_t *config_priority;
		const gchar *type_name;
		gchar *config_key;

		if (!xmms_stream_type_match (t->data, st)) {
			continue;
		}

		type_name = xmms_stream_type_get_str (t->data, XMMS_STREAM_TYPE_NAME);

		config_key = g_strconcat ("priority.", type_name, NULL);
		config_priority = xmms_plugin_config_lookup ((xmms_plugin_t *) plugin,
		                                             config_key);
		g_free (config_key);

		if (config_priority) {
			*priority = xmms_config_property_get_int (config_priority);
		} else {
			*priority = XMMS_STREAM_TYPE_PRIORITY_DEFAULT;
		}

		return TRUE;
	}

	return FALSE;
}

/* Returns TRUE if the given plugin is an effect plugin, and is thus not
   required when building a chain.  These plugins are omitted entirely from
   chain building, and are instead added to the end of complete chains in the
   order given by the user prefs. */
gboolean
xmms_xform_plugin_is_effect (const xmms_xform_plugin_t *plugin)
{
	GList *t;

	g_return_val_if_fail (plugin, FALSE);

	for (t = plugin->in_types; t; t = g_list_next (t)) {
		const gchar *mime;

		mime = xmms_stream_type_get_str (t->data, XMMS_STREAM_TYPE_MIMETYPE);

		/* Any plugin taking in audio/pcm data is considered an effect */
		if (strcmp (mime, "audio/pcm") == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

/* Returns TRUE if the given plugin is required to build certain chains, and
   without the plugin the chain could not be built.
   Currently only the sample converter plugin is here, as this is the only
   effect plugin required to build some chains (particularly those formats like
   FLAC that produce samples often not directly supported by an output plugin. */
gboolean
xmms_xform_plugin_is_chain_support_effect (const xmms_xform_plugin_t *plugin)
{
	const char *plugin_name;

	g_return_val_if_fail (plugin, FALSE);

	plugin_name = xmms_plugin_shortname_get ((const xmms_plugin_t *)plugin);

	return
		(strcmp (plugin_name, "converter") == 0)
	;
}

void
xmms_xform_plugin_metadata_basic_mapper_init (xmms_xform_plugin_t *xform_plugin,
                                               const xmms_xform_metadata_basic_mapping_t *mappings,
                                               gint count)
{
	g_return_if_fail (xform_plugin != NULL);
	g_return_if_fail (mappings != NULL && count > 0);
	xform_plugin->metadata_mapper = xmms_metadata_mapper_init (mappings, count, NULL, 0);
}

void
xmms_xform_plugin_metadata_mapper_init (xmms_xform_plugin_t *xform_plugin,
                                        const xmms_xform_metadata_basic_mapping_t *basic_mappings,
                                        gint basic_count,
                                        const xmms_xform_metadata_mapping_t *mappings,
                                        gint count)
{
	g_return_if_fail (xform_plugin != NULL);
	g_return_if_fail (basic_mappings != NULL && basic_count > 0);
	g_return_if_fail (mappings != NULL && count > 0);
	xform_plugin->metadata_mapper = xmms_metadata_mapper_init (basic_mappings, basic_count,
	                                                           mappings, count);
}

gboolean
xmms_xform_plugin_metadata_mapper_match (const xmms_xform_plugin_t *xform_plugin, xmms_xform_t *xform,
                                         const gchar *key, const gchar *value, gsize length)
{
	g_return_val_if_fail (xform_plugin->metadata_mapper != NULL, FALSE);
	g_return_val_if_fail (key != NULL && value != NULL, FALSE);
	return xmms_metadata_mapper_match (xform_plugin->metadata_mapper, xform, key, value, length);
}

xmms_config_property_t *
xmms_xform_plugin_config_property_register (xmms_xform_plugin_t *xform_plugin,
                                            const gchar *name,
                                            const gchar *default_value,
                                            xmms_object_handler_t cb,
                                            gpointer userdata)
{
	xmms_plugin_t *plugin = (xmms_plugin_t *) xform_plugin;

	return xmms_plugin_config_property_register (plugin, name,
	                                             default_value,
	                                             cb, userdata);
}

xmms_config_property_t *
xmms_xform_plugin_config_lookup (xmms_xform_plugin_t *xform_plugin,
                                 const gchar *path)
{
	xmms_plugin_t *plugin = (xmms_plugin_t *) xform_plugin;

	return xmms_plugin_config_lookup (plugin, path);
}

gboolean
xmms_xform_plugin_can_init (const xmms_xform_plugin_t *plugin)
{
	return !!plugin->methods.init;
}

gboolean
xmms_xform_plugin_can_read (const xmms_xform_plugin_t *plugin)
{
	return !!plugin->methods.read;
}

gboolean
xmms_xform_plugin_can_seek (const xmms_xform_plugin_t *plugin)
{
	return !!plugin->methods.seek;
}

gboolean
xmms_xform_plugin_can_browse (const xmms_xform_plugin_t *plugin)
{
	return !!plugin->methods.browse;
}

gboolean
xmms_xform_plugin_can_destroy (const xmms_xform_plugin_t *plugin)
{
	return !!plugin->methods.destroy;
}

gboolean
xmms_xform_plugin_init (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform)
{
	if (plugin->methods.init)
		return plugin->methods.init (xform);
	return TRUE;
}

gint
xmms_xform_plugin_read (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform,
                        xmms_sample_t *buf, gint length, xmms_error_t *error)
{
	return plugin->methods.read (xform, buf, length, error);
}

gint64
xmms_xform_plugin_seek (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform,
                        gint64 offset, xmms_xform_seek_mode_t whence,
                        xmms_error_t *err)
{
	return plugin->methods.seek (xform, offset, whence, err);
}


gboolean
xmms_xform_plugin_browse (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform,
                          const gchar *url, xmms_error_t *error)
{
	return plugin->methods.browse (xform, url, error);
}

void
xmms_xform_plugin_destroy (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform)
{
	plugin->methods.destroy (xform);
}

