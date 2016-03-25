/*
 * livepatch.c - x86-specific Kernel Live Patching Core
 *
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2014 SUSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include <asm/page_types.h>
#include <asm/elf.h>
#include <asm/livepatch.h>

/**
 * klp_write_module_reloc() - write a relocation in a module
 * @mod:	module in which the section to be modified is found
 * @type:	ELF relocation type (see asm/elf.h)
 * @loc:	address that the relocation should be written to
 * @value:	relocation value (sym address + addend)
 *
 * This function writes a relocation to the specified location for
 * a particular module.
 */
int klp_write_module_reloc(struct module *mod, unsigned long type,
			   unsigned long loc, unsigned long value)
{
	int ret, numpages, size = 4;
	bool readonly;
	unsigned long val;
	unsigned long core_rx = (unsigned long)mod->module_core_rx;
	unsigned long core_rw = (unsigned long)mod->module_core_rw;
	unsigned long core_size_rx = mod->core_size_rx;
	unsigned long core_size_rw = mod->core_size_rw;

	switch (type) {
	case R_X86_64_NONE:
		return 0;
	case R_X86_64_64:
		val = value;
		size = 8;
		break;
	case R_X86_64_32:
		val = (u32)value;
		break;
	case R_X86_64_32S:
		val = (s32)value;
		break;
	case R_X86_64_PC32:
		val = (u32)(value - loc);
		break;
	default:
		/* unsupported relocation type */
		return -EINVAL;
	}

	if ((loc < core_rx || loc >= core_rx + core_size_rx) &&
	    (loc < core_rw || loc >= core_rw + core_size_rw))
		/* loc does not point to any symbol inside the module */
		return -EINVAL;

	readonly = false;

#ifdef CONFIG_DEBUG_SET_MODULE_RONX
	if (loc < core_rx + core_size_rx)
		readonly = true;
#endif

	/* determine if the relocation spans a page boundary */
	numpages = ((loc & PAGE_MASK) == ((loc + size) & PAGE_MASK)) ? 1 : 2;

	if (readonly)
		set_memory_rw(loc & PAGE_MASK, numpages);

	ret = probe_kernel_write((void *)loc, &val, size);

	if (readonly)
		set_memory_ro(loc & PAGE_MASK, numpages);

	return ret;
}