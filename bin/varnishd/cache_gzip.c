/*-
 * Copyright (c) 2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Interaction with the linvgz (zlib) library.
 *
 * The libz library pollutes namespace a LOT when you include the "zlib.h"
 * file so we contain the damage by vectoring all access to libz through
 * this source file.
 *
 * The API defined by this file, will also insulate the rest of the code,
 * should we find a better gzip library at a later date.
 *
 * The absolutely worst case gzip processing path, once we have pipe-lining,
 * will be the following, so we need to be a bit careful with the scratch
 * space we use:
 *
 * 	Backend		Tmp	Input	Output
 *         |		----------------------
 *	   v
 *	 gunzip		wrk	stack	?
 *         |
 *	   v
 *	  esi
 *         |
 *	   v
 *	  gzip		wrk	?	storage
 *         |
 *	   v
 *	  cache
 *         |
 *	   v
 *	 gunzip		wrk	storage	stack
 *         |
 *	   v
 *	 client
 *
 * XXXX: The two '?' are obviously the same memory, but I have yet to decide
 * where it goes.   As usual we try to avoid the session->ws if we can but
 * I may have to use that.
 *
 */

#include "config.h"
#include <stdio.h>

#include "svnid.h"
SVNID("$Id$")

#include "cache.h"
#include "stevedore.h"

#include "zlib.h"

struct vgz {
	unsigned		magic;
#define VGZ_MAGIC		0x162df0cb
	struct ws		*tmp;
	char			*tmp_snapshot;

	void			*before;

	z_stream		vz;
};

/*--------------------------------------------------------------------*/

static voidpf
vgz_alloc(voidpf opaque, uInt items, uInt size)
{
	struct vgz *vg;

	CAST_OBJ_NOTNULL(vg, opaque, VGZ_MAGIC);

	return (WS_Alloc(vg->tmp, items * size));
}

static void
vgz_free(voidpf opaque, voidpf address)
{
	struct vgz *vg;

	CAST_OBJ_NOTNULL(vg, opaque, VGZ_MAGIC);
	(void)address;
}

/*--------------------------------------------------------------------
 * Set up a gunzip instance
 */

static struct vgz *
vgz_alloc_vgz(struct ws *ws)
{
	char *s;
	struct vgz *vg;

	WS_Assert(ws);
	s = WS_Snapshot(ws);
	vg = (void*)WS_Alloc(ws, sizeof *vg);
	AN(vg);
	memset(vg, 0, sizeof *vg);
	vg->magic = VGZ_MAGIC;
	vg->tmp = ws;
	vg->tmp_snapshot = s;

	vg->vz.zalloc = vgz_alloc;
	vg->vz.zfree = vgz_free;
	vg->vz.opaque = vg;

	return (vg);
}

struct vgz *
VGZ_NewUngzip(const struct sess *sp, struct ws *tmp)
{
	struct vgz *vg;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	vg = vgz_alloc_vgz(tmp);

	/*
	 * Max memory usage according to zonf.h:
	 * 	mem_needed = "a few kb" + (1 << (windowBits))
	 * Since we don't control windowBits, we have to assume
	 * it is 15, so 34-35KB or so.
	 */
	assert(Z_OK == inflateInit2(&vg->vz, 31));
	return (vg);
}

static struct vgz *
VGZ_NewGzip(const struct sess *sp, struct ws *tmp)
{
	struct vgz *vg;
	int i;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	vg = vgz_alloc_vgz(tmp);

	/*
	 * From zconf.h:
	 *
	 * 	mem_needed = "a few kb" 
	 *		+ (1 << (windowBits+2))
	 *		+  (1 << (memLevel+9))
	 *
	 * windowBits [8..15] (-> 1K..128K)
	 * memLevel [1..9] (-> 1K->256K)
	 *
	 * XXX: They probably needs to be params...
	 *
	 * XXX: It may be more efficent to malloc them, rather than have
	 * XXX: too many worker threads grow the stacks.
	 */
	i = deflateInit2(&vg->vz,
	    0,				/* Level */
	    Z_DEFLATED,			/* Method */
	    16 + 8,			/* Window bits (16=gzip + 15) */
	    1,				/* memLevel */
	    Z_DEFAULT_STRATEGY);
	if (i != Z_OK)
		printf("deflateInit2() = %d\n", i);
	assert(Z_OK == i);
	return (vg);
}

/*--------------------------------------------------------------------*/

void
VGZ_Ibuf(struct vgz *vg, const void *ptr, size_t len)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	AZ(vg->vz.avail_in);
	vg->vz.next_in = TRUST_ME(ptr);
	vg->vz.avail_in = len;
}

/*--------------------------------------------------------------------*/

void
VGZ_Obuf(struct vgz *vg, const void *ptr, size_t len)
{

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	vg->vz.next_out = TRUST_ME(ptr);
	vg->vz.avail_out = len;
}

/*--------------------------------------------------------------------*/

int
VGZ_Gunzip(struct vgz *vg, const void **pptr, size_t *plen)
{
	int i;

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	*pptr = NULL;
	*plen = 0;
	AN(vg->vz.next_out);
	AN(vg->vz.avail_out);
	vg->before = vg->vz.next_out;
	i = inflate(&vg->vz, 0);
	if (i == Z_OK || i == Z_STREAM_END) {
		*pptr = vg->before;
		*plen = (const uint8_t *)vg->vz.next_out - (const uint8_t*)vg->before;
	}
	if (i == Z_OK)
		return (0);
	if (i == Z_STREAM_END)
		return (1);
	if (i == Z_BUF_ERROR)
		return (2);
	return (-1);
}

/*--------------------------------------------------------------------*/

int
VGZ_Gzip(struct vgz *vg, const void **pptr, size_t *plen, int flags)
{
	int i;

	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);

	*pptr = NULL;
	*plen = 0;
	AN(vg->vz.next_out);
	AN(vg->vz.avail_out);
	vg->before = vg->vz.next_out;
	i = deflate(&vg->vz, flags);
	if (i == Z_OK || i == Z_STREAM_END) {
		*pptr = vg->before;
		*plen = (const uint8_t *)vg->vz.next_out - (const uint8_t*)vg->before;
	}
	if (i == Z_OK)
		return (0);
	if (i == Z_STREAM_END)
		return (1);
	if (i == Z_BUF_ERROR)
		return (2);
	return (-1);
}

/*--------------------------------------------------------------------*/

void
VGZ_Destroy(struct vgz **vg)
{

	CHECK_OBJ_NOTNULL(*vg, VGZ_MAGIC);
	WS_Reset((*vg)->tmp, (*vg)->tmp_snapshot);
	*vg = NULL;
}

/*--------------------------------------------------------------------
 * VFP_GUNZIP
 *
 * A VFP for gunzip'ing an object as we receive it from the backend
 */

static void __match_proto__()
vfp_gunzip_begin(struct sess *sp, size_t estimate)
{
	(void)estimate;
	sp->wrk->vgz_rx = VGZ_NewUngzip(sp, sp->ws);
}

static int __match_proto__()
vfp_gunzip_bytes(struct sess *sp, struct http_conn *htc, size_t bytes)
{
	struct vgz *vg;
	struct storage *st;
	ssize_t l, w;
	int i = -100;
	uint8_t	ibuf[64*1024];		/* XXX size ? */
	size_t dl;
	const void *dp;

	vg = sp->wrk->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->vz.avail_in);
	while (bytes > 0 || vg->vz.avail_in > 0) {
		if (sp->wrk->storage == NULL)
			sp->wrk->storage = STV_alloc(sp,
			    params->fetch_chunksize * 1024LL);
		if (sp->wrk->storage == NULL) {
			errno = ENOMEM;
			return (-1);
		}
		st = sp->wrk->storage;

		VGZ_Obuf(vg, st->ptr + st->len, st->space - st->len);

		if (vg->vz.avail_in == 0 && bytes > 0) {
			l = sizeof ibuf;
			if (l > bytes)
				l = bytes;
			w = HTC_Read(htc, ibuf, l);
			if (w <= 0)
				return (w);
			VGZ_Ibuf(vg, ibuf, w);
			bytes -= w;
		}

		i = VGZ_Gunzip(vg, &dp, &dl);
		assert(i == Z_OK || i == Z_STREAM_END);
		st->len += dl;
		if (st->len == st->space) {
			VTAILQ_INSERT_TAIL(&sp->obj->store,
			    sp->wrk->storage, list);
			sp->obj->len += st->len;
			sp->wrk->storage = NULL;
		}
	}
	if (i == Z_STREAM_END)
		return (1);
	return (-1);
}

static int __match_proto__()
vfp_gunzip_end(struct sess *sp)
{
	struct vgz *vg;
	struct storage *st;

	vg = sp->wrk->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	VGZ_Destroy(&vg);

	st = sp->wrk->storage;
	sp->wrk->storage = NULL;
	if (st == NULL)
		return (0);

	if (st->len == 0) {
		STV_free(st);
		return (0);
	}
	if (st->len < st->space)
		STV_trim(st, st->len);
	sp->obj->len += st->len;
	VTAILQ_INSERT_TAIL(&sp->obj->store, st, list);
	return (0);
}

struct vfp vfp_gunzip = {
        .begin  =       vfp_gunzip_begin,
        .bytes  =       vfp_gunzip_bytes,
        .end    =       vfp_gunzip_end,
};


/*--------------------------------------------------------------------
 * VFP_GZIP
 *
 * A VFP for gzip'ing an object as we receive it from the backend
 */

static void __match_proto__()
vfp_gzip_begin(struct sess *sp, size_t estimate)
{
	(void)estimate;

	sp->wrk->vgz_rx = VGZ_NewGzip(sp, sp->ws);
}

static int __match_proto__()
vfp_gzip_bytes(struct sess *sp, struct http_conn *htc, size_t bytes)
{
	struct vgz *vg;
	struct storage *st;
	ssize_t l, w;
	int i = -100;
	uint8_t ibuf[64*1024];		/* XXX size ? */
	size_t dl;
	const void *dp;

	vg = sp->wrk->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	AZ(vg->vz.avail_in);
	while (bytes > 0 || vg->vz.avail_in > 0) {
		if (sp->wrk->storage == NULL)
			sp->wrk->storage = STV_alloc(sp,
			    params->fetch_chunksize * 1024LL);
		if (sp->wrk->storage == NULL) {
			errno = ENOMEM;
			return (-1);
		}
		st = sp->wrk->storage;

		VGZ_Obuf(vg, st->ptr + st->len, st->space - st->len);

		if (vg->vz.avail_in == 0 && bytes > 0) {
			l = sizeof ibuf;
			if (l > bytes)
				l = bytes;
			w = HTC_Read(htc, ibuf, l);
			if (w <= 0)
				return (w);
			VGZ_Ibuf(vg, ibuf, w);
			bytes -= w;
		}

		i = VGZ_Gzip(vg, &dp, &dl, bytes == 0 ? Z_FINISH : 0);
		assert(i == Z_OK || i == Z_STREAM_END);
		st->len = st->space - dl;
		if (st->len == st->space) {
			VTAILQ_INSERT_TAIL(&sp->obj->store,
			    sp->wrk->storage, list);
			sp->obj->len += st->len;
			sp->wrk->storage = NULL;
		}
	}
	if (i == Z_STREAM_END)
		return (1);
	return (-1);
}

static int __match_proto__()
vfp_gzip_end(struct sess *sp)
{
	struct vgz *vg;
	struct storage *st;

	vg = sp->wrk->vgz_rx;
	CHECK_OBJ_NOTNULL(vg, VGZ_MAGIC);
	VGZ_Destroy(&vg);

	st = sp->wrk->storage;
	sp->wrk->storage = NULL;
	if (st == NULL)
		return (0);

	if (st->len == 0) {
		STV_free(st);
		return (0);
	}
	if (st->len < st->space)
		STV_trim(st, st->len);
	sp->obj->len += st->len;
	VTAILQ_INSERT_TAIL(&sp->obj->store, st, list);
	return (0);
}

struct vfp vfp_gzip = {
        .begin  =       vfp_gzip_begin,
        .bytes  =       vfp_gzip_bytes,
        .end    =       vfp_gzip_end,
};