/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003	Peter Alm, Tobias Rundstr�m, Anders Gustafsson
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

#include "xmms/object.h"
#include "xmms/signal_xmms.h"
#include "xmms/util.h"
#include "xmms/playlist.h"
#include "xmms/medialib.h"

#include <stdarg.h>
#include <string.h>

/** @defgroup Object Object
  * @ingroup XMMSServer
  * @brief Object representation in XMMS server. A object can
  * be used to emit signals.
  * @{
  */

/**
 * A signal handler and it's data.
 */
typedef struct {
	xmms_object_handler_t handler;
	gpointer userdata;
} xmms_object_handler_entry_t;


/**
 * Cleanup all the resources for the object
 */
void
xmms_object_cleanup (xmms_object_t *object)
{
	gint i;

	g_return_if_fail (object);
	g_return_if_fail (XMMS_IS_OBJECT (object));

	for (i = 0; i < XMMS_IPC_SIGNAL_END; i++) {
		if (object->signals[i]) {
			GList *list = object->signals[i];
			GList *node;

			for (node = list; node; node = g_list_next (node)) {
				if (node->data)
					g_free (node->data);
			}
			if (list)
				g_list_free (list);
		}
	}

	g_mutex_free (object->mutex);
}


/**
  * Connect to a signal that is emitted by this object.
  * You can connect many handlers to the same signal as long as
  * the handler address is unique.
  * 
  * @todo fix the need for a unique handler adress?
  *
  * @param object the object that will emit the signal
  * @param signalid the signalid to connect to @sa signal_xmms.h
  * @param handler the Callback function to be called when signal is emited.
  * @param userdata data to the callback function
  */

void
xmms_object_connect (xmms_object_t *object, guint32 signalid,
		     xmms_object_handler_t handler, gpointer userdata)
{
	GList *list = NULL;
	GList *node;
	xmms_object_handler_entry_t *entry;

	g_return_if_fail (object);
	g_return_if_fail (XMMS_IS_OBJECT (object));
	g_return_if_fail (handler);

	entry = g_new0 (xmms_object_handler_entry_t, 1);
	entry->handler = handler;
	entry->userdata = userdata;

	if (object->signals[signalid]) {
		node = object->signals[signalid];
		list = g_list_prepend (node, entry);
		object->signals[signalid] = list;
	} else {
		list = g_list_prepend (list, entry);
		object->signals[signalid] = list;
	}
	
}

/**
  * Disconnect from a signal
  */

void
xmms_object_disconnect (xmms_object_t *object, guint32 signalid,
			xmms_object_handler_t handler)
{
	GList *list = NULL, *node;
	xmms_object_handler_entry_t *entry;

	g_return_if_fail (object);
	g_return_if_fail (XMMS_IS_OBJECT (object));
	g_return_if_fail (handler);

	g_mutex_lock (object->mutex);
	
	if (!object->signals[signalid])
		goto unlock;

	list = object->signals[signalid];

	for (node = list; node; node = g_list_next (node)) {
		entry = node->data;

		if (entry->handler == handler)
			break;
	}

	if (!node)
		goto unlock;

	g_free (node->data);
	object->signals[signalid] = g_list_delete_link (list, node);
unlock:
	g_mutex_unlock (object->mutex);
}

/**
  * Emit a signal and thus call all the handlers that are connected.
  *
  * @param object the object to signal on.
  * @param signalid the signalid to emit
  * @param data the data that should be sent to the handler.
  */

void
xmms_object_emit (xmms_object_t *object, guint32 signalid, gconstpointer data)
{
	GList *list, *node, *list2 = NULL;
	xmms_object_handler_entry_t *entry;
	
	g_return_if_fail (object);
	g_return_if_fail (XMMS_IS_OBJECT (object));

	g_mutex_lock (object->mutex);
	
	list = object->signals[signalid];
	for (node = list; node; node = g_list_next (node)) {
		entry = node->data;

		list2 = g_list_prepend (list2, entry);
	}

	g_mutex_unlock (object->mutex);

	for (node = list2; node; node = g_list_next (node)) {
		entry = node->data;
		
		if (entry && entry->handler)
			entry->handler (object, data, entry->userdata);
	}
	g_list_free (list2);

}

/**
 * Initialize a command argument.
 */

void
xmms_object_cmd_arg_init (xmms_object_cmd_arg_t *arg)
{
	g_return_if_fail (arg);

	memset (arg, 0, sizeof (xmms_object_cmd_arg_t));
	xmms_error_reset (&arg->error);
}

/**
 * Emits a signal on the current object. This is like xmms_object_emit
 * but you don't have to create the #xmms_object_cmd_arg_t yourself.
 * Use this when you creating non-complex signal arguments.
 *
 * @param object Object to signal on.
 * @param signalid Signal to emit.
 * @param type the argument type to emit followed by the argument data.
 *
 */

void
xmms_object_emit_f (xmms_object_t *object, guint32 signalid,
		    xmms_object_cmd_arg_type_t type, ...)
{
	va_list ap;
	xmms_object_cmd_arg_t arg;

	va_start(ap, type);

	switch (type) {
		case XMMS_OBJECT_CMD_ARG_UINT32:
			arg.retval.uint32 = va_arg (ap, guint32);
			break;
		case XMMS_OBJECT_CMD_ARG_INT32:
			arg.retval.int32 = va_arg (ap, gint32);
			break;
		case XMMS_OBJECT_CMD_ARG_STRING:
			arg.retval.string = va_arg (ap, gchar *);
			break;
		case XMMS_OBJECT_CMD_ARG_HASHTABLE:
			arg.retval.hashtable = (GHashTable *) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_UINTLIST:
			arg.retval.uintlist = (GList*) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_HASHLIST:
			arg.retval.hashlist = (GList*) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_INTLIST:
			arg.retval.intlist = (GList*) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_STRINGLIST:
			arg.retval.stringlist = (GList*) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_PLCH:
			arg.retval.plch = (xmms_playlist_changed_msg_t *) va_arg (ap, gpointer);
			break;
		case XMMS_OBJECT_CMD_ARG_NONE:
			break;
	}
	arg.rettype = type;
	va_end(ap);

	xmms_object_emit (object, signalid, &arg);

}


/**
  * Add a command that could be called from the client API to a object.
  *
  * @param object The object that should have the method.
  * @param cmdid A command id.
  * @param desc A command description.
  */
void
xmms_object_cmd_add (xmms_object_t *object, guint cmdid, 
		     xmms_object_cmd_desc_t *desc)
{
	g_return_if_fail (object);
	g_return_if_fail (desc);

	object->cmds[cmdid] = desc;
}

/**
  * Call a command with argument.
  */

void
xmms_object_cmd_call (xmms_object_t *object, guint cmdid, xmms_object_cmd_arg_t *arg)
{
	xmms_object_cmd_desc_t *desc;
	
	g_return_if_fail (object);

	desc = object->cmds[cmdid];

	if (desc->func)
		desc->func (object, arg);
}

/** @} */

void
__int_xmms_object_unref (xmms_object_t *object)
{
	object->ref--;
	if (object->ref == 0) {
		XMMS_DBG ("Free %p", object);
		xmms_object_emit (object, XMMS_IPC_SIGNAL_OBJECT_DESTROYED, NULL);
		if (object->destroy_func)
			object->destroy_func (object);
		xmms_object_cleanup (object);
		g_free (object);
	}
}

xmms_object_t *
__int_xmms_object_new (gint size, xmms_object_destroy_func_t destfunc)
{
	xmms_object_t *ret;

	ret = g_malloc0 (size);
	ret->destroy_func = destfunc;
	ret->id = XMMS_OBJECT_MID;

	ret->mutex = g_mutex_new ();
	xmms_object_ref (ret);

	return ret;
}

