/*
 * DBus generic interfaces for wicked
 *
 * Copyright (C) 2011 Olaf Kirch <okir@suse.de>
 */

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include "netinfo_priv.h"
#include "dbus-common.h"
#include "model.h"
#include "config.h"
#include "debug.h"
#include "dbus-connection.h"
#include "process.h"

extern ni_dbus_object_t *	ni_objectmodel_new_interface(ni_dbus_server_t *server,
					const ni_dbus_service_t *service,
					const ni_dbus_variant_t *dict, DBusError *error);

#define NI_DBUS_SERVICES_MAX	128
typedef struct ni_dbus_service_array {
	unsigned int		count;
	const ni_dbus_service_t *services[NI_DBUS_SERVICES_MAX];
} ni_dbus_service_array_t;

#define NI_DBUS_CLASSES_MAX	1024
typedef struct ni_dbus_class_array {
	unsigned int		count;
	const ni_dbus_class_t *	class[NI_DBUS_CLASSES_MAX];
} ni_dbus_class_array_t;

static ni_dbus_class_array_t	ni_objectmodel_class_registry;
static ni_dbus_service_array_t	ni_objectmodel_service_registry;

static const ni_dbus_class_t	ni_objectmodel_netif_list_class = {
	.name		= NI_OBJECTMODEL_NETIF_LIST_CLASS,
};

static ni_dbus_service_t	ni_objectmodel_netif_list_service;
static ni_dbus_service_t	ni_objectmodel_netif_root_interface;

ni_dbus_server_t *		__ni_objectmodel_server;

/*
 * Create the dbus service
 */
ni_dbus_server_t *
ni_objectmodel_create_service(void)
{
	ni_dbus_server_t *server;

	server = ni_server_listen_dbus(WICKED_DBUS_BUS_NAME);
	if (server == NULL)
		ni_fatal("unable to initialize dbus service");

	/* register the netif-list class (to allow extensions to attach to it) */
	ni_objectmodel_register_class(&ni_objectmodel_netif_list_class);

	/* register all netif classes and service */
	ni_objectmodel_register_netif_classes();

	/* Initialize our addrconf clients */
	ni_objectmodel_dhcp4_init(server);
	ni_objectmodel_autoip_init(server);

	ni_objectmodel_register_service(&ni_objectmodel_netif_list_service);
#if 0
	ni_objectmodel_register_link_service(NI_IFTYPE_ETHERNET, &wicked_dbus_ethernet_service);
	ni_objectmodel_register_link_service(NI_IFTYPE_VLAN, &wicked_dbus_vlan_service);
	ni_objectmodel_register_link_service(NI_IFTYPE_BRIDGE, &wicked_dbus_bridge_service);
	//ni_objectmodel_register_link_service(NI_IFTYPE_BOND, &wicked_dbus_bond_service);
#endif

	__ni_objectmodel_server = server;
	return server;
}

/*
 * Create the initial object hierarchy
 */
dbus_bool_t
ni_objectmodel_create_initial_objects(ni_dbus_server_t *server)
{
	ni_dbus_object_t *object;

	/* Register root interface with the root of the object hierarchy */
	object = ni_dbus_server_get_root_object(server);
	ni_dbus_object_register_service(object, &ni_objectmodel_netif_root_interface);

	/* Register com.suse.Wicked.Interface, which is the list of all interfaces */
	object = ni_dbus_server_register_object(server, "Interface",
					&ni_objectmodel_netif_list_class,
					NULL);
	if (object == NULL)
		ni_fatal("Unable to create dbus object for interfaces");

	ni_objectmodel_bind_compatible_interfaces(object);
	return TRUE;
}

dbus_bool_t
ni_objectmodel_bind_compatible_interfaces(ni_dbus_object_t *object)
{
	unsigned int i;

	if (object->class == NULL) {
		ni_error("%s: object \"%s\" without class", __func__, object->path);
		return FALSE;
	}

	NI_TRACE_ENTER_ARGS("object=%s, class=%s", object->path, object->class->name);
	for (i = 0; i < ni_objectmodel_service_registry.count; ++i) {
		const ni_dbus_service_t *service = ni_objectmodel_service_registry.services[i];
		const ni_dbus_class_t *class;

		/* If the service is compatible with the object's dbus class,
		 * or any of its superclasses, register this interface to this
		 * object */
		for (class = object->class; class; class = class->superclass) {
			if (service->compatible == class) {
				ni_dbus_object_register_service(object, service);
				break;
			}
		}
	}

	return TRUE;
}

/*
 * objectmodel service registry
 */
void
ni_objectmodel_register_service(const ni_dbus_service_t *service)
{
	unsigned int index = ni_objectmodel_service_registry.count;

	ni_assert(index < NI_DBUS_SERVICES_MAX);

	ni_objectmodel_service_registry.services[index++] = service;
	ni_objectmodel_service_registry.count = index;
}

const ni_dbus_service_t *
ni_objectmodel_service_by_name(const char *name)
{
	unsigned int i;

	for (i = 0; i < ni_objectmodel_service_registry.count; ++i) {
		const ni_dbus_service_t *service = ni_objectmodel_service_registry.services[i];

		if (!strcmp(service->name, name))
			return service;
	}

	return NULL;
}

const ni_dbus_service_t *
ni_objectmodel_service_by_class(const ni_dbus_class_t *class)
{
	unsigned int i;

	for (i = 0; i < ni_objectmodel_service_registry.count; ++i) {
		const ni_dbus_service_t *service = ni_objectmodel_service_registry.services[i];

		if (service->compatible == class)
			return service;
	}

	return NULL;
}

/*
 * objectmodel service registry
 * This is mostly needed for doing proper type checking when binding
 * extensions
 */
void
ni_objectmodel_register_class(const ni_dbus_class_t *class)
{
	unsigned int index = ni_objectmodel_class_registry.count;

	ni_assert(class->name);
	ni_assert(index < NI_DBUS_CLASSES_MAX);

	ni_objectmodel_class_registry.class[index++] = class;
	ni_objectmodel_class_registry.count = index;
}

const ni_dbus_class_t *
ni_objectmodel_get_class(const char *name)
{
	unsigned int i;

	for (i = 0; i < ni_objectmodel_class_registry.count; ++i) {
		const ni_dbus_class_t *class = ni_objectmodel_class_registry.class[i];

		if (!strcmp(class->name, name))
			return class;
	}
	return NULL;
}

/*
 * This method allows clients to create new (virtual) network interfaces.
 * The first argument is the DBus service name of the interface type to
 * create, eg. com.suse.Wicked.Interface.VLAN for a vlan interface.
 * The second argument is a dict containing all the properties making up the
 * configuration of the new interface. These properties must be supported by
 * the chosen service, i.e. when creating a VLAN device, you can only specify
 * VLAN properties, but no, say, network configuration items.
 *
 * The only exception from this rule is the special property "name", which
 * can be used to requests a specific name for the newly created interface.
 */
static dbus_bool_t
__ni_dbus_netif_create(ni_dbus_object_t *object, const ni_dbus_method_t *method,
			unsigned int argc, const ni_dbus_variant_t *argv,
			ni_dbus_message_t *reply, DBusError *error)
{
	const char *interface_name, *object_path;
	const ni_dbus_service_t *service;
	ni_dbus_object_t *result;
	DBusMessageIter iter;

	NI_TRACE_ENTER();
	if (!ni_dbus_variant_get_string(&argv[0], &interface_name))
		goto bad_args;

	service = ni_objectmodel_service_by_name(interface_name);
	if (service == NULL) {
		dbus_set_error(error, DBUS_ERROR_FAILED,
			"Unknown dbus interface %s", interface_name);
		return FALSE;
	}

	/* FIXME: we should make sure this is compatible with class netif */

	result = ni_objectmodel_new_interface(ni_dbus_object_get_server(object), service, &argv[1], error);
	if (!result)
		return FALSE;

	dbus_message_iter_init_append(reply, &iter);

	object_path = ni_dbus_object_get_path(result);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &object_path);

	return TRUE;

bad_args:
	dbus_set_error(error, DBUS_ERROR_FAILED,
			"Bad argument in call to Interface.create()");
	return FALSE;
}

static ni_dbus_method_t		wicked_dbus_netif_methods[] = {
	{ "create",		"sa{sv}",	__ni_dbus_netif_create },
	{ NULL }
};


static ni_dbus_service_t	ni_objectmodel_netif_list_service = {
	.name		= WICKED_DBUS_INTERFACE ".Obsolete",
	.compatible	= &ni_objectmodel_netif_list_class,
	.methods	= wicked_dbus_netif_methods,
//	.properties	= wicked_dbus_netif_properties,
};

/*
 * The interface for the dbus root node. Nothing much for now.
 */
static ni_dbus_service_t	ni_objectmodel_netif_root_interface = {
	.name		= WICKED_DBUS_INTERFACE,
};

/*
 * Write dbus message to a temporary file
 */
static char *
__ni_objectmodel_write_message(ni_dbus_message_t *msg)
{
	char *tempname = NULL;
	char *msg_data;
	int msg_len;
	FILE *fp;

	if (!dbus_message_marshal(msg, &msg_data, &msg_len)) {
		ni_error("%s: unable to marshal script arguments", __func__);
		return NULL;
	}

	if ((fp = ni_mkstemp(&tempname)) == NULL) {
		ni_error("%s: unable to create tempfile for script arguments", __func__);
	} else {
		if (ni_file_write(fp, msg_data, msg_len) < 0) {
			ni_error("%s: unable to store message (len=%d)", __func__, msg_len);
			unlink(tempname);
			ni_string_free(&tempname);
			/* tempname is NULL after this */
		}

		fclose(fp);
	}

	free(msg_data);
	return tempname;
}

static char *
__ni_objectmodel_empty_tempfile(void)
{
	char *tempname = NULL;
	FILE *fp;

	if ((fp = ni_mkstemp(&tempname)) == NULL) {
		ni_error("%s: unable to create tempfile for script arguments", __func__);
		return NULL;
	}

	fclose(fp);
	return tempname;
}

dbus_bool_t
ni_objectmodel_extension_call(ni_dbus_connection_t *connection,
				ni_dbus_object_t *object, const ni_dbus_method_t *method,
				ni_dbus_message_t *call)
{
	DBusError error = DBUS_ERROR_INIT;
	const char *interface = dbus_message_get_interface(call);
	ni_extension_t *extension;
	ni_process_t *command;
	ni_process_instance_t *process;
	char *tempname = NULL;

	extension = ni_config_find_extension(ni_global.config, interface);
	if (extension == NULL) {
		dbus_set_error(&error, DBUS_ERROR_SERVICE_UNKNOWN, "%s: no/unknown interface %s",
				__func__, interface);
		ni_dbus_connection_send_error(connection, call, &error);
		return FALSE;
	}

	if ((command = ni_extension_script_find(extension, method->name)) == NULL) {
		dbus_set_error(&error, DBUS_ERROR_FAILED, "%s: no/unknown extension method %s",
				__func__, method->name);
		ni_dbus_connection_send_error(connection, call, &error);
		return FALSE;
	}

	/* Create an instance of this command */
	process = ni_process_instance_new(command);

	/* Build the argument blob and store it in a file */
	tempname = __ni_objectmodel_write_message(call);
	if (tempname != NULL) {
		ni_process_instance_setenv(process, "WICKED_ARGFILE", tempname);
		ni_string_free(&tempname);
	} else {
		goto general_failure;
	}

	/* Create empty reply for script return data */
	tempname = __ni_objectmodel_empty_tempfile();
	if (tempname != NULL) {
		ni_process_instance_setenv(process, "WICKED_RETFILE", tempname);
		ni_string_free(&tempname);
	} else {
		goto general_failure;
	}

	/* Run the process */
	if (ni_dbus_async_server_call_run_command(connection, object, method, call, process) < 0) {
		ni_error("%s: error executing method %s", __func__, method->name);
		dbus_set_error(&error, DBUS_ERROR_FAILED, "%s: error executing method %s",
				__func__, method->name);
		ni_dbus_connection_send_error(connection, call, &error);
		ni_process_instance_free(process);
		return FALSE;
	}

	return TRUE;

general_failure:
	dbus_set_error(&error, DBUS_ERROR_FAILED, "%s - general failure when executing method",
			method->name);
	ni_dbus_connection_send_error(connection, call, &error);

	if (process)
		ni_process_instance_free(process);

	if (tempname) {
		unlink(tempname);
		free(tempname);
	}
	return FALSE;
}

static dbus_bool_t
ni_objectmodel_extension_completion(ni_dbus_connection_t *connection,
				ni_dbus_object_t *object, const ni_dbus_method_t *method,
				ni_dbus_message_t *call, const ni_process_instance_t *process)
{
	ni_dbus_message_t *reply;

	if (ni_process_exit_status_okay(process)) {
		reply = dbus_message_new_method_return(call);
		/* FIXME: if the method returns anything, we need to read it
		 * from the response file */
	} else {
		reply = dbus_message_new_error(call, DBUS_ERROR_FAILED,
				"dbus extension script returns error");
	}

	if (ni_dbus_connection_send_message(connection, reply) < 0)
		ni_error("unable to send reply (out of memory)");

	dbus_message_unref(reply);
	return TRUE;
}

/*
 * Bind extension scripts to the interface functions they are specified for.
 */
int
ni_objectmodel_bind_extensions(void)
{
	unsigned int i;

	NI_TRACE_ENTER();
	for (i = 0; i < ni_objectmodel_service_registry.count; ++i) {
		const ni_dbus_service_t *service = ni_objectmodel_service_registry.services[i];
		const ni_dbus_method_t *method;
		ni_extension_t *extension;

		extension = ni_config_find_extension(ni_global.config, service->name);
		if (extension == NULL)
			continue;

		for (method = service->methods; method->name != NULL; ++method) {
			if (method->handler != NULL)
				continue;
			if (ni_extension_script_find(extension, method->name) != NULL) {
				ni_dbus_method_t *mod_method = (ni_dbus_method_t *) method;

				ni_debug_dbus("registering extension hook for method %s.%s",
						service->name, method->name);
				mod_method->async_handler = ni_objectmodel_extension_call;
				mod_method->async_completion = ni_objectmodel_extension_completion;
			}
		}
	}

	return 0;
}

