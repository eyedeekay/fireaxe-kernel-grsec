/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2010
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * xattr.c
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/xattr.h>
#include <linux/slab.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "squashfs_fs_i.h"
#include "squashfs.h"

static const struct xattr_handler *squashfs_xattr_handler(int);

ssize_t squashfs_listxattr(struct dentry *d, char *buffer,
	size_t buffer_size)
{
	struct inode *inode = d_inode(d);
	struct super_block *sb = inode->i_sb;
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	u64 start = SQUASHFS_XATTR_BLK(squashfs_i(inode)->xattr)
						 + msblk->xattr_table;
	int offset = SQUASHFS_XATTR_OFFSET(squashfs_i(inode)->xattr);
	int count = squashfs_i(inode)->xattr_count;
	size_t used = 0;
	ssize_t err;

	/* check that the file system has xattrs */
	if (msblk->xattr_id_table == NULL)
		return -EOPNOTSUPP;

	/* loop reading each xattr name */
	while (count--) {
		struct squashfs_xattr_entry entry;
		struct squashfs_xattr_val val;
		const struct xattr_handler *handler;
		int name_size, prefix_size = 0;

		err = squashfs_read_metadata(sb, &entry, &start, &offset,
							sizeof(entry));
		if (err < 0)
			goto failed;

		name_size = le16_to_cpu(entry.size);
		handler = squashfs_xattr_handler(le16_to_cpu(entry.type));
		if (handler)
			prefix_size = handler->list(handler, d, buffer, buffer ? buffer_size - used : 0,
						    NULL, name_size);
		if (prefix_size) {
			if (buffer) {
				if (prefix_size + name_size + 1 > buffer_size - used) {
					err = -ERANGE;
					goto failed;
				}
				buffer += prefix_size;
			}
			err = squashfs_read_metadata(sb, buffer, &start,
				&offset, name_size);
			if (err < 0)
				goto failed;
			if (buffer) {
				buffer[name_size] = '\0';
				buffer += name_size + 1;
			}
			used += prefix_size + name_size + 1;
		} else  {
			/* no handler or insuffficient privileges, so skip */
			err = squashfs_read_metadata(sb, NULL, &start,
				&offset, name_size);
			if (err < 0)
				goto failed;
		}


		/* skip remaining xattr entry */
		err = squashfs_read_metadata(sb, &val, &start, &offset,
						sizeof(val));
		if (err < 0)
			goto failed;

		err = squashfs_read_metadata(sb, NULL, &start, &offset,
						le32_to_cpu(val.vsize));
		if (err < 0)
			goto failed;
	}
	err = used;

failed:
	return err;
}


static int squashfs_xattr_get(struct inode *inode, int name_index,
	const char *name, void *buffer, size_t buffer_size)
{
	struct super_block *sb = inode->i_sb;
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	u64 start = SQUASHFS_XATTR_BLK(squashfs_i(inode)->xattr)
						 + msblk->xattr_table;
	int offset = SQUASHFS_XATTR_OFFSET(squashfs_i(inode)->xattr);
	int count = squashfs_i(inode)->xattr_count;
	int name_len = strlen(name);
	int err, vsize;
	char *target = kmalloc(name_len, GFP_KERNEL);

	if (target == NULL)
		return  -ENOMEM;

	/* loop reading each xattr name */
	for (; count; count--) {
		struct squashfs_xattr_entry entry;
		struct squashfs_xattr_val val;
		int type, prefix, name_size;

		err = squashfs_read_metadata(sb, &entry, &start, &offset,
							sizeof(entry));
		if (err < 0)
			goto failed;

		name_size = le16_to_cpu(entry.size);
		type = le16_to_cpu(entry.type);
		prefix = type & SQUASHFS_XATTR_PREFIX_MASK;

		if (prefix == name_index && name_size == name_len)
			err = squashfs_read_metadata(sb, target, &start,
						&offset, name_size);
		else
			err = squashfs_read_metadata(sb, NULL, &start,
						&offset, name_size);
		if (err < 0)
			goto failed;

		if (prefix == name_index && name_size == name_len &&
					strncmp(target, name, name_size) == 0) {
			/* found xattr */
			if (type & SQUASHFS_XATTR_VALUE_OOL) {
				__le64 xattr_val;
				u64 xattr;
				/* val is a reference to the real location */
				err = squashfs_read_metadata(sb, &val, &start,
						&offset, sizeof(val));
				if (err < 0)
					goto failed;
				err = squashfs_read_metadata(sb, &xattr_val,
					&start, &offset, sizeof(xattr_val));
				if (err < 0)
					goto failed;
				xattr = le64_to_cpu(xattr_val);
				start = SQUASHFS_XATTR_BLK(xattr) +
							msblk->xattr_table;
				offset = SQUASHFS_XATTR_OFFSET(xattr);
			}
			/* read xattr value */
			err = squashfs_read_metadata(sb, &val, &start, &offset,
							sizeof(val));
			if (err < 0)
				goto failed;

			vsize = le32_to_cpu(val.vsize);
			if (buffer) {
				if (vsize > buffer_size) {
					err = -ERANGE;
					goto failed;
				}
				err = squashfs_read_metadata(sb, buffer, &start,
					 &offset, vsize);
				if (err < 0)
					goto failed;
			}
			break;
		}

		/* no match, skip remaining xattr entry */
		err = squashfs_read_metadata(sb, &val, &start, &offset,
							sizeof(val));
		if (err < 0)
			goto failed;
		err = squashfs_read_metadata(sb, NULL, &start, &offset,
						le32_to_cpu(val.vsize));
		if (err < 0)
			goto failed;
	}
	err = count ? vsize : -ENODATA;

failed:
	kfree(target);
	return err;
}


static size_t squashfs_xattr_handler_list(const struct xattr_handler *handler,
					  struct dentry *d, char *list,
					  size_t list_size, const char *name,
					  size_t name_len)
{
	int len = strlen(handler->prefix);

	if (list && len <= list_size)
		memcpy(list, handler->prefix, len);
	return len;
}

static int squashfs_xattr_handler_get(const struct xattr_handler *handler,
				      struct dentry *d, const char *name,
				      void *buffer, size_t size)
{
	if (name[0] == '\0')
		return  -EINVAL;

	return squashfs_xattr_get(d_inode(d), handler->flags, name,
		buffer, size);
}

/*
 * User namespace support
 */
static const struct xattr_handler squashfs_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.flags	= SQUASHFS_XATTR_USER,
	.list	= squashfs_xattr_handler_list,
	.get	= squashfs_xattr_handler_get
};

/*
 * Trusted namespace support
 */
static size_t squashfs_trusted_xattr_handler_list(const struct xattr_handler *handler,
						  struct dentry *d, char *list,
						  size_t list_size, const char *name,
						  size_t name_len)
{
	if (!capable(CAP_SYS_ADMIN))
		return 0;
	return squashfs_xattr_handler_list(handler, d, list, list_size, name,
					   name_len);
}

static const struct xattr_handler squashfs_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.flags	= SQUASHFS_XATTR_TRUSTED,
	.list	= squashfs_trusted_xattr_handler_list,
	.get	= squashfs_xattr_handler_get
};

/*
 * Security namespace support
 */
static const struct xattr_handler squashfs_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.flags	= SQUASHFS_XATTR_SECURITY,
	.list	= squashfs_xattr_handler_list,
	.get	= squashfs_xattr_handler_get
};

static const struct xattr_handler *squashfs_xattr_handler(int type)
{
	if (type & ~(SQUASHFS_XATTR_PREFIX_MASK | SQUASHFS_XATTR_VALUE_OOL))
		/* ignore unrecognised type */
		return NULL;

	switch (type & SQUASHFS_XATTR_PREFIX_MASK) {
	case SQUASHFS_XATTR_USER:
		return &squashfs_xattr_user_handler;
	case SQUASHFS_XATTR_TRUSTED:
		return &squashfs_xattr_trusted_handler;
	case SQUASHFS_XATTR_SECURITY:
		return &squashfs_xattr_security_handler;
	default:
		/* ignore unrecognised type */
		return NULL;
	}
}

const struct xattr_handler *squashfs_xattr_handlers[] = {
	&squashfs_xattr_user_handler,
	&squashfs_xattr_trusted_handler,
	&squashfs_xattr_security_handler,
	NULL
};
