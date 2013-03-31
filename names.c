/*
 * kdbus - interprocess message routing
 *
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include "kdbus.h"

#include "kdbus_internal.h"

#define KDBUS_MSG_DATA_SIZE(SIZE) \
	ALIGN((SIZE) + offsetof(struct kdbus_msg_data, data), sizeof(u64))

static void __kdbus_name_registry_free(struct kref *kref)
{
	struct kdbus_name_registry *reg =
		container_of(kref, struct kdbus_name_registry, kref);
	kfree(reg);
}

void kdbus_name_registry_unref(struct kdbus_name_registry *reg)
{
	kref_put(&reg->kref, __kdbus_name_registry_free);
}

struct kdbus_name_registry *kdbus_name_registry_new(void)
{
	struct kdbus_name_registry *reg;

	reg = kzalloc(sizeof(*reg), GFP_KERNEL);
	if (!reg)
		return NULL;

	kref_init(&reg->kref);
	INIT_LIST_HEAD(&reg->entries_list);
	mutex_init(&reg->entries_lock);

	return reg;
}

static u64 kdbus_name_make_hash(const char *name)
{
	unsigned int len = strlen(name);
	u64 hash = init_name_hash();

	while (len--)
		hash = partial_name_hash(*name++, hash);

	return end_name_hash(hash);
}

struct kdbus_name_entry *__kdbus_name_lookup(struct kdbus_name_registry *reg,
					     u64 hash, const char *name,
					     u64 type)
{
	struct kdbus_name_entry *e;

	list_for_each_entry(e, &reg->entries_list, registry_entry) {
		if (e->hash == hash && strcmp(e->name, name) == 0)
			return e;
	}

	return NULL;
}

static void kdbus_name_add_to_conn(struct kdbus_name_entry *e,
				   struct kdbus_conn *conn)
{
	e->conn = conn;
	list_add_tail(&conn->names_list, &e->conn_entry);
}

static void kdbus_name_entry_free(struct kdbus_name_entry *e)
{
	list_del(&e->conn_entry);
	list_del(&e->registry_entry);
	kfree(e->name);
	kfree(e);
}

void kdbus_name_remove_by_conn(struct kdbus_name_registry *reg,
			       struct kdbus_conn *conn)
{
	struct kdbus_name_entry *tmp, *e;

	mutex_lock(&reg->entries_lock);

	list_for_each_entry_safe(e, tmp, &conn->names_list, conn_entry)
		kdbus_name_entry_free(e);

	mutex_unlock(&reg->entries_lock);
}

struct kdbus_name_entry *kdbus_name_lookup(struct kdbus_name_registry *reg,
					   const char *name, u64 flags)
{
	struct kdbus_name_entry *e = NULL;
	u64 hash = kdbus_name_make_hash(name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name, flags);
	mutex_unlock(&reg->entries_lock);

	return e;
}

static int kdbus_name_send_name_changed_msg(struct kdbus_conn *old,
					    struct kdbus_conn *new,
					    struct kdbus_name_entry *e)
{
	struct kdbus_kmsg *kmsg;
	struct kdbus_msg_data *data;
	struct kdbus_manager_msg_name_change *name_change;
	u64 extra_size = sizeof(*name_change) + strlen(e->name) + 1;
	int ret;

	ret = kdbus_kmsg_new(new, extra_size, &kmsg);
	if (ret < 0)
		return ret;

	data = kmsg->msg.data;
	data->type = KDBUS_MSG_NAME_CHANGE;
	name_change = &data->name_change;

	name_change->size = sizeof(*name_change) + strlen(e->name) + 1;
	name_change->old_id = old->id;
	name_change->new_id = new->id;
	name_change->flags = 0; /* FIXME */
	strcpy(name_change->name, e->name);

	/* FIXME: broadcast? */
	ret = kdbus_kmsg_send(new, kmsg);
	if (ret < 0)
		return ret;

	kdbus_kmsg_unref(kmsg);

	return 0;
}

static int kdbus_name_handle_conflict(struct kdbus_name_registry *reg,
				      struct kdbus_conn *conn,
				      struct kdbus_name_entry *e, u64 *flags)
{
	if ((*flags   & KDBUS_CMD_NAME_REPLACE_EXISTING) &&
	    (e->flags & KDBUS_CMD_NAME_ALLOW_REPLACEMENT)) {
		/* ... */
		return kdbus_name_send_name_changed_msg(e->conn, conn, e);
	}

	if (*flags & KDBUS_CMD_NAME_QUEUE) {
		*flags |= KDBUS_CMD_NAME_IN_QUEUE;
		return 0;
	}

	return -EEXIST;
}

/* IOCTL interface */

int kdbus_name_acquire(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       void __user *buf)
{
	u64 __user *msgsize = buf + offsetof(struct kdbus_cmd_name, size);
	struct kdbus_name_entry *e = NULL;
	struct kdbus_cmd_name *name;
	u64 size, hash, flags = 0; /* FIXME */
	int ret = 0;

	if (get_user(size, msgsize))
		return -EFAULT;

	if (size < sizeof(*name) || size >= 0xffff)
		return -EMSGSIZE;

	name = kzalloc(size, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	ret = copy_from_user(name, buf, size);
	if (ret < 0)
		return -EFAULT;

	hash = kdbus_name_make_hash(name->name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name->name, flags);
	if (e) {
		ret = kdbus_name_handle_conflict(reg, conn, e, &flags);
		if (ret < 0)
			goto err_unlock;

		goto exit_copy;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e) {
		ret = -ENOMEM;
		goto err_unlock_free;
	}

	e->name = kstrdup(name->name, GFP_KERNEL);
	if (!e->name) {
		ret = -ENOMEM;
		goto err_unlock_free;
	}

	e->hash = hash;
	e->flags = flags;
	INIT_LIST_HEAD(&e->registry_entry);
	INIT_LIST_HEAD(&e->conn_entry);

	list_add_tail(&reg->entries_list, &e->registry_entry);
	kdbus_name_add_to_conn(e, conn);

exit_copy:
	ret = copy_to_user(buf, name, size);
	if (ret < 0) {
		ret = -EFAULT;
		goto err_unlock_free;
	}

	kfree(name);
	mutex_unlock(&reg->entries_lock);
	return 0;

err_unlock_free:
	kfree(name);
	kdbus_name_entry_free(e);

err_unlock:
	mutex_unlock(&reg->entries_lock);

	return ret;
}

int kdbus_name_release(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       void __user *buf)
{
	u64 __user *msgsize = buf + offsetof(struct kdbus_cmd_name, size);
	struct kdbus_name_entry *e;
	struct kdbus_cmd_name *name;
	u64 size, hash;
	u64 type = 0; /* FIXME */
	int ret = 0;

	if (get_user(size, msgsize))
		return -EFAULT;

	if (size < sizeof(*name) || size >= 0xffff)
		return -EMSGSIZE;

	name = kzalloc(size, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	ret = copy_from_user(name, buf, size);
	if (ret < 0) {
		ret = -EFAULT;
		goto exit_free;
	}

	hash = kdbus_name_make_hash(name->name);

	mutex_lock(&reg->entries_lock);
	e = __kdbus_name_lookup(reg, hash, name->name, type);
	if (e && e->conn == conn)
		kdbus_name_entry_free(e);
	mutex_unlock(&reg->entries_lock);

exit_free:
	kfree(name);

	return ret;
}

int kdbus_name_list(struct kdbus_name_registry *reg,
		    struct kdbus_conn *conn,
		    void __user *buf)
{
	u64 __user *msgsize = buf + offsetof(struct kdbus_cmd_names, size);
	struct kdbus_cmd_names *names;
	struct kdbus_cmd_name *name;
	struct kdbus_name_entry *e;
	u64 user_size, size = 0;
	int ret = 0;

	if (get_user(user_size, msgsize))
		return -EFAULT;

	mutex_lock(&reg->entries_lock);

	size = sizeof(struct kdbus_cmd_names);

	list_for_each_entry(e, &reg->entries_list, registry_entry)
		size += sizeof(struct kdbus_cmd_name) + strlen(e->name) + 1;

	if (size > user_size) {
		ret = -ENOSPC;
		goto exit_unlock;
	}

	names = kzalloc(size, GFP_KERNEL);
	if (!names) {
		ret = -ENOMEM;
		goto exit_unlock;
	}

	names->size = size;
	name = names->names;

	list_for_each_entry(e, &reg->entries_list, registry_entry) {
		name->size = sizeof(struct kdbus_cmd_name) + strlen(e->name) + 1;
		name->flags = 0; /* FIXME */
		name->id = e->conn->id;
		strcpy(name->name, e->name);
		name = (struct kdbus_cmd_name *) ((u8 *) name + name->size);
	}

	if (copy_to_user(buf, names, size) < 0) {
		ret = -EFAULT;
		goto exit_unlock;
	}

exit_unlock:
	mutex_unlock(&reg->entries_lock);

	return ret;
}

int kdbus_name_query(struct kdbus_name_registry *reg,
		     struct kdbus_conn *conn,
		     void __user *buf)
{
	//struct kdbus_name_entry *e;
	//struct kdbus_cmd_name_info name_info;

	return -ENOSYS;
}
