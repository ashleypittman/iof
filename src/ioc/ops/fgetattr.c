/* Copyright (C) 2016-2017 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "iof_common.h"
#include "ioc.h"
#include "log.h"

#define REQ_NAME request
#define POOL_NAME fgh_pool
#define TYPE_NAME getattr_req
#define RESTOCK_ON_SEND
#include "ioc_ops.h"

static const struct ioc_request_api api = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= ioc_getattr_cb,
	.on_evict	= ioc_simple_resend
};

#define STAT_KEY getfattr

int ioc_getattr_gah(struct iof_file_handle *fh, struct stat *stbuf)
{
	struct iof_projection_info *fs_handle = fh->fs_handle;
	struct TYPE_NAME *desc = NULL;
	struct iof_gah_in *in;
	int rc;

	IOF_TRACE_INFO(fh, GAH_PRINT_STR,
		       GAH_PRINT_VAL(fh->common.gah));
	if (!fh->common.gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}
	IOC_REQ_INIT(desc, fs_handle, api, in, rc);
	if (rc)
		D_GOTO(out, rc);

	desc->request.ptr = stbuf;
	in->gah = fh->common.gah;
	IOC_REQ_SEND(desc, fs_handle, rc);
	/* Cache the inode number */
	if (rc == 0)
		fh->inode_no = stbuf->st_ino;
out:
	IOC_REQ_RELEASE(desc);
	IOF_TRACE_DEBUG(fh, GAH_PRINT_STR " rc %d",
			GAH_PRINT_VAL(fh->common.gah), rc);
	return rc;
}

int ioc_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	if (fi)
		return ioc_getattr_gah((struct iof_file_handle *)fi->fh, stbuf);

	if (!path)
		return -EIO;
	return ioc_getattr_name(path, stbuf);
}

static void getattr_ll_cb(struct ioc_request *request)
{
	struct iof_getattr_out *out	= IOC_GET_RESULT(request);
	fuse_req_t f_req		= request->req;
	int rc;

	request->req = NULL;
	ioc_getattr_cb(request);
	rc = IOC_STATUS_TO_RC_LL(request);
	if (rc == 0)
		IOF_FUSE_REPLY_ATTR(f_req, out->stat.iov_buf);
	else
		IOF_FUSE_REPLY_ERR(f_req, -rc);
	IOC_REQ_RELEASE(CONTAINER(request));
}

static const struct ioc_request_api api_ll = {
	.get_fsh	= get_fs_handle,
	.on_send	= post_send,
	.on_result	= getattr_ll_cb,
	.on_evict	= ioc_simple_resend
};

void
ioc_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct iof_projection_info *fs_handle	= fuse_req_userdata(req);
	struct TYPE_NAME *desc			= NULL;
	struct iof_gah_in *in;
	int rc;

	IOF_TRACE_INFO(req, "ino %lu", ino);
	IOC_REQ_INIT_LL(desc, fs_handle, api_ll, in, req, rc);
	if (rc)
		D_GOTO(err, rc);
	IOF_TRACE_LINK(req, desc, "request");

	rc = find_gah(fs_handle, ino, &in->gah);
	if (rc != 0)
		D_GOTO(err, rc = ENOENT);
	IOC_REQ_SEND_LL(desc, fs_handle, rc);
	if (rc != 0)
		D_GOTO(err, rc);
	return;
err:
	IOC_REQ_RELEASE(desc);
	IOF_FUSE_REPLY_ERR(req, rc);
}
