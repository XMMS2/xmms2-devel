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
#ifndef __XMMSPRIV_XFORMPLUGIN_H__
#define __XMMSPRIV_XFORMPLUGIN_H__

gboolean xmms_xform_plugin_can_init (const xmms_xform_plugin_t *plugin);
gboolean xmms_xform_plugin_can_read (const xmms_xform_plugin_t *plugin);
gboolean xmms_xform_plugin_can_seek (const xmms_xform_plugin_t *plugin);
gboolean xmms_xform_plugin_can_browse (const xmms_xform_plugin_t *plugin);
gboolean xmms_xform_plugin_can_destroy (const xmms_xform_plugin_t *plugin);

gboolean xmms_xform_plugin_init (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform);
gboolean xmms_xform_plugin_metadata_mapper_match (const xmms_xform_plugin_t *xform_plugin, xmms_xform_t *xform, const gchar *key, const gchar *value, gsize length);
gint xmms_xform_plugin_read (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform, xmms_sample_t *buf, gint length, xmms_error_t *error);
gint64 xmms_xform_plugin_seek (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform, gint64 offset, xmms_xform_seek_mode_t whence, xmms_error_t *err);
gboolean xmms_xform_plugin_browse (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform, const gchar *url, xmms_error_t *error);
void xmms_xform_plugin_destroy (const xmms_xform_plugin_t *plugin, xmms_xform_t *xform);

gboolean xmms_xform_plugin_supports (const xmms_xform_plugin_t *plugin, const xmms_stream_type_t *st, gint *priority);
gboolean xmms_xform_plugin_is_effect (const xmms_xform_plugin_t *plugin);
gboolean xmms_xform_plugin_is_chain_support_effect (const xmms_xform_plugin_t *plugin);

xmms_stream_type_t *xmms_xform_plugin_get_out_stream_type (xmms_xform_plugin_t *plugin);

#endif
