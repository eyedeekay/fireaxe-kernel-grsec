/*
 * Copyright (c) 2014 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef __WIL_PLATFORM_H__
#define __WIL_PLATFORM_H__

struct device;

/**
 * struct wil_platform_ops - wil platform module callbacks
 */
struct wil_platform_ops {
	int (*bus_request)(void *handle, uint32_t kbps /* KBytes/Sec */);
	int (*suspend)(void *handle);
	int (*resume)(void *handle);
	void (*uninit)(void *handle);
} __no_const;

void *wil_platform_init(struct device *dev, struct wil_platform_ops *ops);

int __init wil_platform_modinit(void);
void wil_platform_modexit(void);

#endif /* __WIL_PLATFORM_H__ */