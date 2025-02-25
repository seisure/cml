/*
 * This file is part of GyroidOS
 * Copyright(c) 2013 - 2022 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <gyroidos@aisec.fraunhofer.de>
 */

#define _GNU_SOURCE

//#define LOGF_LOG_MIN_PRIO LOGF_PRIO_TRACE

#include "hotplug.h"

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cmld.h"
#include "container.h"
#include "common/event.h"
#include "common/fd.h"
#include "common/file.h"
#include "common/dir.h"
#include "common/macro.h"
#include "common/mem.h"
#include "common/network.h"
#include "common/str.h"
#include "common/uevent.h"

typedef struct hotplug_container_dev_mapping {
	container_t *container;
	hotplug_usbdev_t *usbdev;
	bool assign;
} hotplug_container_dev_mapping_t;

typedef struct hotplug_net_dev_mapping {
	container_t *container;
	container_pnet_cfg_t *pnet_cfg;
	uint8_t mac[6];
} hotplug_container_netdev_mapping_t;

struct hotplug_usbdev {
	char *i_serial;
	uint16_t id_vendor;
	uint16_t id_product;
	int major;
	int minor;
	bool assign;
	hotplug_usbdev_type_t type;
};

static uevent_uev_t *uevent_uev = NULL;

// track usb devices mapped to containers
static list_t *hotplug_container_dev_mapping_list = NULL;

// track net devices mapped to containers
static list_t *hotplug_container_netdev_mapping_list = NULL;

hotplug_usbdev_t *
hotplug_usbdev_new(hotplug_usbdev_type_t type, uint16_t id_vendor, uint16_t id_product,
		   char *i_serial, bool assign)
{
	hotplug_usbdev_t *usbdev = mem_new0(hotplug_usbdev_t, 1);
	usbdev->type = type;
	usbdev->id_vendor = id_vendor;
	usbdev->id_product = id_product;
	usbdev->i_serial = mem_strdup(i_serial);
	usbdev->assign = assign;
	usbdev->major = -1;
	usbdev->minor = -1;
	return usbdev;
}

uint16_t
hotplug_usbdev_get_id_vendor(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->id_vendor;
}

uint16_t
hotplug_usbdev_get_id_product(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->id_product;
}

hotplug_usbdev_type_t
hotplug_usbdev_get_type(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->type;
}

char *
hotplug_usbdev_get_i_serial(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->i_serial;
}

bool
hotplug_usbdev_is_assigned(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->assign;
}

void
hotplug_usbdev_set_major(hotplug_usbdev_t *usbdev, int major)
{
	ASSERT(usbdev);
	usbdev->major = major;
}

void
hotplug_usbdev_set_minor(hotplug_usbdev_t *usbdev, int minor)
{
	ASSERT(usbdev);
	usbdev->minor = minor;
}

int
hotplug_usbedv_get_major(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->major;
}

int
hotplug_usbdev_get_minor(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	return usbdev->minor;
}

static hotplug_container_dev_mapping_t *
hotplug_container_dev_mapping_new(container_t *container, hotplug_usbdev_t *usbdev)
{
	hotplug_container_dev_mapping_t *mapping = mem_new0(hotplug_container_dev_mapping_t, 1);
	mapping->container = container;
	mapping->usbdev = mem_new0(hotplug_usbdev_t, 1);
	mapping->usbdev->i_serial = mem_strdup(usbdev->i_serial);
	mapping->usbdev->id_vendor = usbdev->id_vendor;
	mapping->usbdev->id_product = usbdev->id_product;
	mapping->usbdev->major = usbdev->major;
	mapping->usbdev->minor = usbdev->minor;
	mapping->usbdev->assign = usbdev->assign;
	mapping->usbdev->type = usbdev->type;

	return mapping;
}

static void
hotplug_container_dev_mapping_free(hotplug_container_dev_mapping_t *mapping)
{
	if (mapping->usbdev) {
		if (mapping->usbdev->i_serial)
			mem_free0(mapping->usbdev->i_serial);
		mem_free0(mapping->usbdev);
	}
	mem_free0(mapping);
}

static void
hotplug_container_netdev_mapping_free(hotplug_container_netdev_mapping_t *mapping)
{
	mem_free0(mapping);
}

static hotplug_container_netdev_mapping_t *
hotplug_container_netdev_mapping_new(container_t *container, container_pnet_cfg_t *pnet_cfg)
{
	hotplug_container_netdev_mapping_t *mapping =
		mem_new0(hotplug_container_netdev_mapping_t, 1);
	mapping->container = container;
	mapping->pnet_cfg = pnet_cfg;

	// We only accept mac strings in pnet config for mappings
	if (-1 == network_str_to_mac_addr(pnet_cfg->pnet_name, mapping->mac)) {
		hotplug_container_netdev_mapping_free(mapping);
		return NULL;
	}

	return mapping;
}

static char *
hotplug_replace_devpath_new(const char *str, const char *oldstr, const char *newstr)
{
	char *ptr_old = NULL;
	int len_diff = strlen(newstr) - strlen(oldstr);
	if (!(ptr_old = strstr(str, oldstr))) {
		DEBUG("Could not find %s in %s", oldstr, str);
		return NULL;
	}

	unsigned int off_old;
	char *str_replaced = mem_alloc0((strlen(str) + 1) + len_diff);
	unsigned int pos_new = 0;

	off_old = ptr_old - str;

	strncpy(str_replaced, str, off_old);
	pos_new += off_old;

	strcpy(str_replaced + pos_new, newstr);
	pos_new += strlen(newstr);

	strcpy(str_replaced + pos_new, ptr_old + strlen(oldstr));

	return str_replaced;
}

static char *
hotplug_rename_ifi_new(const char *oldname, const char *infix)
{
	static unsigned int cmld_wlan_idx = 0;
	static unsigned int cmld_eth_idx = 0;

	//generate interface name that is unique
	//in the root network namespace
	unsigned int *ifi_idx;
	char *newname = NULL;

	ifi_idx = !strcmp(infix, "wlan") ? &cmld_wlan_idx : &cmld_eth_idx;

	if (-1 == asprintf(&newname, "%s%s%d", "cml", infix, *ifi_idx)) {
		ERROR("Failed to generate new interface name");
		return NULL;
	}

	*ifi_idx += 1;

	INFO("Renaming %s to %s", oldname, newname);

	if (network_rename_ifi(oldname, newname)) {
		ERROR("Failed to rename interface %s", oldname);
		mem_free0(newname);
		return NULL;
	}

	return newname;
}

static uevent_event_t *
hotplug_rename_interface(const uevent_event_t *event)
{
	char *event_ifname = uevent_event_get_interface(event);
	char *event_devpath = uevent_event_get_devpath(event);
	const char *prefix = uevent_event_get_devtype(event);

	char *new_ifname = NULL;
	char *new_devpath = NULL;
	uevent_event_t *uev_chname = NULL;
	uevent_event_t *uev_chdevpath = NULL;

	// if no devtype is set in uevent prefix with eth by default
	if (!*prefix)
		prefix = "eth";

	new_ifname = hotplug_rename_ifi_new(event_ifname, prefix);

	if (!new_ifname) {
		DEBUG("Failed to prepare renamed uevent member (ifname)");
		goto err;
	}

	// replace ifname in cmld's available netifs
	if (cmld_netif_phys_remove_by_name(event_ifname))
		cmld_netif_phys_add_by_name(new_ifname);

	new_devpath = hotplug_replace_devpath_new(event_devpath, event_ifname, new_ifname);

	if (!new_devpath) {
		DEBUG("Failed to prepare renamed uevent member (devpath)");
		goto err;
	}

	uev_chname = uevent_replace_member(event, event_ifname, new_ifname);

	if (!uev_chname) {
		ERROR("Failed to rename interface name %s in uevent", event_ifname);
		goto err;
	}

	event_devpath = uevent_event_get_devpath(uev_chname);
	uev_chdevpath = uevent_replace_member(uev_chname, event_devpath, new_devpath);

	if (!uev_chdevpath) {
		ERROR("Failed to rename devpath %s in uevent", event_devpath);
		goto err;
	}
	DEBUG("Injected renamed interface name %s, devpath %s into uevent", new_ifname,
	      new_devpath);

	mem_free0(new_ifname);
	mem_free0(new_devpath);
	mem_free0(uev_chname);

	return uev_chdevpath;

err:
	if (new_ifname)
		mem_free0(new_ifname);
	if (new_devpath)
		mem_free0(new_devpath);
	if (uev_chname)
		mem_free0(uev_chname);

	return NULL;
}

static int
hotplug_netdev_move(uevent_event_t *event)
{
	uint8_t iface_mac[6];
	char *macstr = NULL;
	uevent_event_t *newevent = NULL;
	container_pnet_cfg_t *pnet_cfg_c0 = NULL;
	char *event_ifname = uevent_event_get_interface(event);

	if (network_get_mac_by_ifname(event_ifname, iface_mac)) {
		ERROR("Iface '%s' with no mac, skipping!", event_ifname);
		goto error;
	}

	container_t *container = NULL;
	container_pnet_cfg_t *pnet_cfg = NULL;
	for (list_t *l = hotplug_container_netdev_mapping_list; l; l = l->next) {
		hotplug_container_netdev_mapping_t *mapping = l->data;
		if (0 == memcmp(iface_mac, mapping->mac, 6)) {
			container = mapping->container;
			pnet_cfg = mapping->pnet_cfg;
			break;
		}
	}

	// no mapping found move to c0
	if (!container) {
		container = cmld_containers_get_c0();
		pnet_cfg_c0 = container_pnet_cfg_new(event_ifname, false, NULL);
		pnet_cfg = pnet_cfg_c0;
	}

	if (!container) {
		WARN("Target container not found, skip moving %s", event_ifname);
		goto error;
	}

	if ((container_get_state(container) != COMPARTMENT_STATE_BOOTING) &&
	    (container_get_state(container) != COMPARTMENT_STATE_RUNNING) &&
	    (container_get_state(container) != COMPARTMENT_STATE_STARTING)) {
		WARN("Target container '%s' is not running, skip moving %s",
		     container_get_description(container), event_ifname);
		goto error;
	}

	// rename network interface to avoid name clashes when moving to container
	DEBUG("Renaming new interface we were notified about");
	newevent = hotplug_rename_interface(event);

	// uevent pointer is not freed inside this function, therefore we can safely drop it
	if (newevent) {
		DEBUG("using renamed uevent");
		event = newevent;
		event_ifname = uevent_event_get_interface(event);
		container_pnet_cfg_set_pnet_name(pnet_cfg, event_ifname);
	} else {
		WARN("failed to rename interface %s. injecting uevent as it is", event_ifname);
	}

	macstr = network_mac_addr_to_str_new(iface_mac);
	if (cmld_container_add_net_iface(container, pnet_cfg, false)) {
		ERROR("cannot move '%s' to %s!", macstr, container_get_name(container));
		goto error;
	} else {
		INFO("moved phys network interface '%s' (mac: %s) to %s", event_ifname, macstr,
		     container_get_name(container));
	}

	// if mac_filter is applied we have a bridge interface and do not
	// need to send the uevent about the physical if
	if (pnet_cfg->mac_filter) {
		goto out;
	}

	// if moving was successful also inject uevent
	if (uevent_event_inject_into_netns(event, container_get_pid(container),
					   container_has_userns(container)) < 0) {
		WARN("could not inject uevent into netns of container %s!",
		     container_get_name(container));
	} else {
		TRACE("successfully injected uevent into netns of container %s!",
		      container_get_name(container));
	}
out:
	if (newevent)
		mem_free0(newevent);
	mem_free0(macstr);
	return 0;
error:
	if (newevent)
		mem_free0(newevent);
	if (pnet_cfg_c0)
		mem_free0(pnet_cfg_c0);
	mem_free0(macstr);
	return -1;
}

static void
hotplug_sysfs_netif_timer_cb(event_timer_t *timer, void *data)
{
	ASSERT(data);
	uevent_event_t *event = data;

	// if sysfs is not ready in case of wifi just return and retry.
	IF_TRUE_RETURN(!strcmp(uevent_event_get_devtype(event), "wlan") &&
		       !network_interface_is_wifi(uevent_event_get_interface(event)));

	if (hotplug_netdev_move(event) == -1)
		WARN("Did not move net interface!");
	else
		INFO("Moved net interface to target.");

	mem_free0(event);
	event_remove_timer(timer);
	event_timer_free(timer);
}

static int
hotplug_usbdev_sysfs_foreach_cb(const char *path, const char *name, void *data)
{
	uint16_t id_product, id_vendor;
	char buf[256];
	int len;
	bool found;
	int dev[2];

	hotplug_usbdev_t *usbdev = data;
	IF_NULL_RETVAL(usbdev, -1);

	found = false;

	char *id_product_file = mem_printf("%s/%s/idProduct", path, name);
	char *id_vendor_file = mem_printf("%s/%s/idVendor", path, name);
	char *i_serial_file = mem_printf("%s/%s/serial", path, name);
	char *dev_file = mem_printf("%s/%s/dev", path, name);

	TRACE("id_product_file: %s", id_product_file);
	TRACE("id_vendor_file: %s", id_vendor_file);
	TRACE("i_serial_file: %s", i_serial_file);

	IF_FALSE_GOTO_TRACE(file_exists(id_product_file), out);
	IF_FALSE_GOTO_TRACE(file_exists(id_vendor_file), out);
	IF_FALSE_GOTO_TRACE(file_exists(dev_file), out);

	len = file_read(id_product_file, buf, sizeof(buf));
	IF_TRUE_GOTO((len < 4), out);
	IF_TRUE_GOTO((sscanf(buf, "%hx", &id_product) < 0), out);
	found = (id_product == hotplug_usbdev_get_id_product(usbdev));
	TRACE("found: %d", found);

	len = file_read(id_vendor_file, buf, sizeof(buf));
	IF_TRUE_GOTO((len < 4), out);
	IF_TRUE_GOTO((sscanf(buf, "%hx", &id_vendor) < 0), out);
	found &= (id_vendor == hotplug_usbdev_get_id_vendor(usbdev));
	TRACE("found: %d", found);

	if (file_exists(i_serial_file)) {
		len = file_read(i_serial_file, buf, sizeof(buf));
		TRACE("%s len=%d", buf, len);
		TRACE("%s len=%zu", hotplug_usbdev_get_i_serial(usbdev),
		      strlen(hotplug_usbdev_get_i_serial(usbdev)));
		found &= (0 == strncmp(buf, hotplug_usbdev_get_i_serial(usbdev),
				       strlen(hotplug_usbdev_get_i_serial(usbdev))));
		TRACE("found: %d", found);
	} else {
		buf[0] = '\0';
	}
	IF_FALSE_GOTO_TRACE(found, out);

	// major = minor = -1;
	dev[0] = dev[1] = -1;
	found = false; // we use this in case of error during file parsing

	len = file_read(dev_file, buf, sizeof(buf));
	IF_TRUE_GOTO(len < 0, out);
	IF_TRUE_GOTO((sscanf(buf, "%d:%d", &dev[0], &dev[1]) < 0), out);
	IF_FALSE_GOTO((dev[0] > -1 && dev[1] > -1), out);

	found = true; // parsing dev_file succeded.

	hotplug_usbdev_set_major(usbdev, dev[0]);
	hotplug_usbdev_set_minor(usbdev, dev[1]);

out:
	mem_free0(id_product_file);
	mem_free0(id_vendor_file);
	mem_free0(i_serial_file);
	mem_free0(dev_file);
	return found ? 1 : 0;
}

int
hotplug_usbdev_set_sysfs_props(hotplug_usbdev_t *usbdev)
{
	ASSERT(usbdev);
	const char *sysfs_path = "/sys/bus/usb/devices";

	// for the first time iterate through sysfs to find device
	if (0 >= dir_foreach(sysfs_path, &hotplug_usbdev_sysfs_foreach_cb, usbdev)) {
		WARN("Could not find usb device (%d:%d, %s) in %s!", usbdev->id_vendor,
		     usbdev->id_product, usbdev->i_serial, sysfs_path);
		return -1;
	}

	return 0;
}

struct hotplug_token_data {
	container_t *container;
	char *devname;
};

static void
hotplug_token_timer_cb(event_timer_t *timer, void *data)
{
	ASSERT(data);
	struct hotplug_token_data *token_data = data;

	static int retries = 10;

	DEBUG("devname: %s", token_data->devname);

	IF_TRUE_GOTO(0 > retries--, out);

	// wait for device node to become available
	IF_TRUE_RETURN(!file_exists(token_data->devname));

	container_token_attach(token_data->container);
	INFO("Processed token attachment of token %s for container %s", token_data->devname,
	     container_get_name(token_data->container));

out:
	mem_free0(token_data->devname);
	mem_free0(token_data);
	event_remove_timer(timer);
	event_timer_free(timer);
}

/*
 * return true if uevent is handled completely, false if uevent should process further
 * in calling funtion
 */
static bool
hotplug_handle_usb_device(unsigned actions, uevent_event_t *event)
{
	IF_TRUE_RETVAL_TRACE(strncmp(uevent_event_get_subsystem(event), "usb", 3) ||
				     strncmp(uevent_event_get_devtype(event), "usb_device", 10),
			     false);

	if (actions & UEVENT_ACTION_REMOVE) {
		TRACE("usb remove");
		for (list_t *l = hotplug_container_dev_mapping_list; l; l = l->next) {
			hotplug_container_dev_mapping_t *mapping = l->data;
			if ((uevent_event_get_major(event) == mapping->usbdev->major) &&
			    (uevent_event_get_minor(event) == mapping->usbdev->minor)) {
				if (HOTPLUG_USBDEV_TYPE_TOKEN == mapping->usbdev->type) {
					INFO("HOTPLUG USB TOKEN removed");
					container_token_detach(mapping->container);
				} else {
					container_device_deny(mapping->container, 'c',
							      mapping->usbdev->major,
							      mapping->usbdev->minor);
				}
				INFO("Denied access to unbound device node %d:%d mapped in container %s",
				     mapping->usbdev->major, mapping->usbdev->minor,
				     container_get_name(mapping->container));
			}
		}
	}

	if (actions & UEVENT_ACTION_ADD) {
		TRACE("usb add");

		char *serial_path = mem_printf("/sys/%s/serial", uevent_event_get_devpath(event));
		char *serial = NULL;

		if (file_exists(serial_path))
			serial = file_read_new(serial_path, 255);

		mem_free0(serial_path);

		if (!serial || strlen(serial) < 1) {
			TRACE("Failed to read serial of usb device");
			return false;
		}

		if ('\n' == serial[strlen(serial) - 1]) {
			serial[strlen(serial) - 1] = 0;
		}

		for (list_t *l = hotplug_container_dev_mapping_list; l; l = l->next) {
			hotplug_container_dev_mapping_t *mapping = l->data;
			uint16_t vendor_id = uevent_event_get_usb_vendor(event);
			uint16_t product_id = uevent_event_get_usb_product(event);

			INFO("check mapping: %04x:%04x '%s' for %s bound device node %d:%d -> container %s",
			     vendor_id, product_id, serial, (mapping->assign) ? "assign" : "allow",
			     uevent_event_get_major(event), uevent_event_get_minor(event),
			     container_get_name(mapping->container));

			if ((mapping->usbdev->id_vendor == vendor_id) &&
			    (mapping->usbdev->id_product == product_id) &&
			    (0 == strcmp(mapping->usbdev->i_serial, serial))) {
				mapping->usbdev->major = uevent_event_get_major(event);
				mapping->usbdev->minor = uevent_event_get_minor(event);
				INFO("%s bound device node %d:%d -> container %s",
				     (mapping->assign) ? "assign" : "allow", mapping->usbdev->major,
				     mapping->usbdev->minor,
				     container_get_name(mapping->container));
				if (HOTPLUG_USBDEV_TYPE_TOKEN == mapping->usbdev->type) {
					INFO("HOTPLUG USB TOKEN added");
					struct hotplug_token_data *token_data =
						mem_new0(struct hotplug_token_data, 1);
					token_data->container = mapping->container;
					token_data->devname = mem_printf(
						"%s%s",
						strncmp("/dev/", uevent_event_get_devname(event),
							4) ?
							"/dev/" :
							"/",
						uevent_event_get_devname(event));

					// give devfs some time to create device node for token
					event_timer_t *e =
						event_timer_new(100, EVENT_TIMER_REPEAT_FOREVER,
								hotplug_token_timer_cb, token_data);
					event_add_timer(e);
				}
				container_device_allow(mapping->container, 'c',
						       mapping->usbdev->major,
						       mapping->usbdev->minor, mapping->assign);
			}
		}
		mem_free0(serial);
	}
	return false;
}

static void
hotplug_handle_uevent_cb(unsigned actions, uevent_event_t *event, UNUSED void *data)
{
	/*
	 * if handler returns true the event is completely handled
	 * otherwise event should be checked for possible forwarding
	 */
	IF_TRUE_RETURN_TRACE(hotplug_handle_usb_device(actions, event));

	TRACE("Got new add/remove/change uevent");

	/* move network ifaces to containers */
	if (actions & UEVENT_ACTION_ADD && !strcmp(uevent_event_get_subsystem(event), "net") &&
	    !strstr(uevent_event_get_devpath(event), "virtual")) {
		// got new physical interface, initially add to cmld tracking list
		cmld_netif_phys_add_by_name(uevent_event_get_interface(event));

		// give sysfs some time to settle if iface is wifi
		event_timer_t *e =
			event_timer_new(100, EVENT_TIMER_REPEAT_FOREVER,
					hotplug_sysfs_netif_timer_cb, uevent_event_copy_new(event));
		event_add_timer(e);
	}
}

int
hotplug_init()
{
	if (!cmld_is_hostedmode_active()) {
		// Initially rename all physical interfaces before starting uevent handling.
		for (list_t *l = cmld_get_netif_phys_list(); l; l = l->next) {
			const char *ifname = l->data;
			const char *prefix = (network_interface_is_wifi(ifname)) ? "wlan" : "eth";
			char *if_name_new = hotplug_rename_ifi_new(ifname, prefix);
			if (if_name_new) {
				mem_free0(l->data);
				l->data = if_name_new;
			}
		}
	}

	// Register uevent handler for kernel events
	uevent_uev = uevent_uev_new(UEVENT_UEV_TYPE_KERNEL,
				    UEVENT_ACTION_ADD | UEVENT_ACTION_CHANGE | UEVENT_ACTION_REMOVE,
				    hotplug_handle_uevent_cb, NULL);

	return uevent_add_uev(uevent_uev);
}

void
hotplug_cleanup()
{
	IF_NULL_RETURN(uevent_uev);

	uevent_remove_uev(uevent_uev);
	uevent_uev_free(uevent_uev);
}

int
hotplug_register_usbdevice(container_t *container, hotplug_usbdev_t *usbdev)
{
	hotplug_container_dev_mapping_t *mapping =
		hotplug_container_dev_mapping_new(container, usbdev);
	hotplug_container_dev_mapping_list =
		list_append(hotplug_container_dev_mapping_list, mapping);

	INFO("Registered usbdevice %04x:%04x '%s' [c %d:%d] for container %s",
	     mapping->usbdev->id_vendor, mapping->usbdev->id_product, mapping->usbdev->i_serial,
	     mapping->usbdev->major, mapping->usbdev->minor,
	     container_get_name(mapping->container));

	return 0;
}

int
hotplug_unregister_usbdevice(container_t *container, hotplug_usbdev_t *usbdev)
{
	hotplug_container_dev_mapping_t *mapping_to_remove = NULL;

	for (list_t *l = hotplug_container_dev_mapping_list; l; l = l->next) {
		hotplug_container_dev_mapping_t *mapping = l->data;
		if ((mapping->container == container) &&
		    (mapping->usbdev->id_vendor == usbdev->id_vendor) &&
		    (mapping->usbdev->id_product == usbdev->id_product) &&
		    (0 == strcmp(mapping->usbdev->i_serial, usbdev->i_serial))) {
			mapping_to_remove = mapping;
		}
	}

	IF_NULL_RETVAL(mapping_to_remove, -1);

	hotplug_container_dev_mapping_list =
		list_remove(hotplug_container_dev_mapping_list, mapping_to_remove);

	INFO("Unregistered usbdevice %04x:%04x '%s' for container %s",
	     mapping_to_remove->usbdev->id_vendor, mapping_to_remove->usbdev->id_product,
	     mapping_to_remove->usbdev->i_serial, container_get_name(mapping_to_remove->container));

	hotplug_container_dev_mapping_free(mapping_to_remove);

	return 0;
}

int
hotplug_register_netdev(container_t *container, container_pnet_cfg_t *pnet_cfg)
{
	hotplug_container_netdev_mapping_t *mapping =
		hotplug_container_netdev_mapping_new(container, pnet_cfg);

	IF_NULL_RETVAL(mapping, -1);

	hotplug_container_netdev_mapping_list =
		list_append(hotplug_container_netdev_mapping_list, mapping);
	char *macstr = network_mac_addr_to_str_new(mapping->mac);

	INFO("Registered netdev '%s' for container %s", macstr,
	     container_get_name(mapping->container));

	mem_free0(macstr);
	return 0;
}

int
hotplug_unregister_netdev(container_t *container, uint8_t mac[6])
{
	hotplug_container_netdev_mapping_t *mapping_to_remove = NULL;

	for (list_t *l = hotplug_container_netdev_mapping_list; l; l = l->next) {
		hotplug_container_netdev_mapping_t *mapping = l->data;
		if ((mapping->container == container) && (0 == memcmp(mapping->mac, mac, 6))) {
			mapping_to_remove = mapping;
		}
	}

	IF_NULL_RETVAL(mapping_to_remove, -1);

	hotplug_container_netdev_mapping_list =
		list_remove(hotplug_container_netdev_mapping_list, mapping_to_remove);

	char *macstr = network_mac_addr_to_str_new(mapping_to_remove->mac);

	INFO("Unregistered netdev '%s' for container %s", macstr,
	     container_get_name(mapping_to_remove->container));

	hotplug_container_netdev_mapping_free(mapping_to_remove);
	mem_free0(macstr);

	return 0;
}
