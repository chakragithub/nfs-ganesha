/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * -------------
 */

/* main.c
 * Module core functions
 */

#include "config.h"

#include "fsal.h"
#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include "nlm_list.h"
#include "fsal_internal.h"
#include "FSAL/fsal_init.h"

/* ZFS FSAL module private storage
 */

struct zfs_fsal_module {
	struct fsal_module fsal;
	struct fsal_staticfsinfo_t fs_info;
	fsal_init_info_t fsal_info;
	/* vfsfs_specific_initinfo_t specific_info;  placeholder */
};

const char myname[] = "ZFS";

/* filesystem info for your filesystem */
static struct fsal_staticfsinfo_t default_zfs_info = {
	.maxfilesize = 0xFFFFFFFFFFFFFFFFLL,	/* max file size (64bits) */
	.maxlink = 1024,	/* max links for an object of your filesystem */
	.maxnamelen = MAXNAMLEN,		/* max filename */
	.maxpathlen = MAXPATHLEN,		/* min filename */
	.no_trunc = true,			/* no_trunc */
	.chown_restricted = true,		/* chown restricted */
	.case_insensitive = false,		/* case insensitivity */
	.case_preserving = true,		/* case preserving */
	.lock_support = false,	/* lock support */
	.lock_support_owner = false,		/* lock owners */
	.lock_support_async_block = false,	/* async blocking locks */
	.named_attr = true,			/* named attributes */
	.unique_handles = true,		/* handles are unique and persistent */
	.lease_time = {10, 0},	/* Duration of lease at FS in seconds */
	.acl_support = FSAL_ACLSUPPORT_ALLOW,	/* ACL support */
	.homogenous = true,			/* homogenous */
	.supported_attrs = ZFS_SUPPORTED_ATTRIBUTES, /* supported attributes */
};

static struct config_item zfs_params[] = {
	CONF_ITEM_BOOL("link_support", true,
		       fsal_staticfsinfo_t, link_support),
	CONF_ITEM_BOOL("symlink_support", true,
		       fsal_staticfsinfo_t, symlink_support),
	CONF_ITEM_BOOL("cansettime", true,
		       fsal_staticfsinfo_t, cansettime),
	CONF_ITEM_UI32("maxread", 512, 1024*1024, 1000000,
		       fsal_staticfsinfo_t, maxread),
	CONF_ITEM_UI32("maxwrite", 512, 1024*1024, 1000000,
		       fsal_staticfsinfo_t, maxwrite),
	CONF_ITEM_MODE("umask", 0, 0777, 0,
		       fsal_staticfsinfo_t, umask),
	CONF_ITEM_BOOL("auth_xdev_export", false,
		       fsal_staticfsinfo_t, auth_exportpath_xdev),
	CONF_ITEM_MODE("xattr_access_rights", 0, 0777, 0400,
		       fsal_staticfsinfo_t, xattr_access_rights),
	CONFIG_EOL
};

struct config_block zfs_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.zfs",
	.blk_desc.name = "ZFS",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = zfs_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/* private helper for export object
 */

struct fsal_staticfsinfo_t *zfs_staticinfo(struct fsal_module *hdl)
{
	struct zfs_fsal_module *myself;

	myself = container_of(hdl, struct zfs_fsal_module, fsal);
	return &myself->fs_info;
}

/* Module methods
 */

/* init_config
 * must be called with a reference taken (via lookup_fsal)
 */

static fsal_status_t init_config(struct fsal_module *fsal_hdl,
				 config_file_t config_struct)
{
	struct zfs_fsal_module *zfs_me =
	    container_of(fsal_hdl, struct zfs_fsal_module, fsal);
	int rc;

	zfs_me->fs_info = default_zfs_info;	/* copy the consts */
	rc = load_config_from_parse(config_struct,
				    &zfs_param,
				    &zfs_me->fs_info,
				    true);
	if (rc < 0)
		return fsalstat(ERR_FSAL_INVAL, 0);
	display_fsinfo(&zfs_me->fs_info);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes constant = 0x%" PRIx64,
		     (uint64_t) ZFS_SUPPORTED_ATTRIBUTES);
	LogFullDebug(COMPONENT_FSAL,
		     "Supported attributes default = 0x%" PRIx64,
		     default_zfs_info.supported_attrs);
	LogDebug(COMPONENT_FSAL,
		 "FSAL INIT: Supported attributes mask = 0x%" PRIx64,
		 zfs_me->fs_info.supported_attrs);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* Internal ZFS method linkage to export object
 */

fsal_status_t zfs_create_export(struct fsal_module *fsal_hdl,
				const char *export_path,
				void *parse_node,
				struct exportlist *exp_entry,
				struct fsal_module *next_fsal,
				const struct fsal_up_vector *up_ops,
				struct fsal_export **export);

/* Module initialization.
 * Called by dlopen() to register the module
 * keep a private pointer to me in myself
 */

/* my module private storage
 */

static struct zfs_fsal_module ZFS;

MODULE_INIT void zfs_load(void)
{
	int retval;
	struct fsal_module *myself = &ZFS.fsal;

	retval = register_fsal(myself, myname,
			       FSAL_MAJOR_VERSION,
			       FSAL_MINOR_VERSION);
	if (retval != 0) {
		fprintf(stderr, "ZFS module failed to register");
		return;
	}

	myself->ops->create_export = zfs_create_export;
	myself->ops->init_config = init_config;
	init_fsal_parameters(&ZFS.fsal_info);
}

MODULE_FINI void zfs_unload(void)
{
	int retval;

	retval = unregister_fsal(&ZFS.fsal);
	if (retval != 0) {
		fprintf(stderr, "ZFS module failed to unregister");
		return;
	}
}