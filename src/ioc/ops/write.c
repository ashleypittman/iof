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
#include "ios_gah.h"

struct write_cb_r {
	struct iof_file_handle *handle;
	size_t len;
	struct iof_tracker tracker;
	int err;
	int rc;
};

static void
write_cb(const struct crt_cb_info *cb_info)
{
	struct write_cb_r *reply = cb_info->cci_arg;
	struct iof_write_out *out = crt_reply_get(cb_info->cci_rpc);

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_TRACE_INFO(reply->handle, "Bad RPC reply %d",
			       cb_info->cci_rc);
		if (cb_info->cci_rc == -DER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		iof_tracker_signal(&reply->tracker);
		return;
	}

	if (out->err) {
		/* Convert the error types, out->err is a IOF error code
		 * so translate it to a errno we can pass back to FUSE.
		 */
		IOF_TRACE_ERROR(reply->handle, "Error from target %d",
				out->err);

		reply->err = EIO;
		if (out->err == IOF_GAH_INVALID)
			reply->handle->common.gah_valid = 0;

		if (out->err == IOF_ERR_NOMEM)
			reply->err = EAGAIN;

		iof_tracker_signal(&reply->tracker);
		return;
	}

	reply->len = out->len;
	reply->rc = out->rc;
	iof_tracker_signal(&reply->tracker);
}

int ioc_write_direct(const char *buff, size_t len, off_t position,
		     struct iof_file_handle *handle)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_write_in *in;
	struct write_cb_r reply = {0};

	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_TRACE_INFO(handle, "path %s", handle->name);

	rc = crt_req_create(fs_handle->proj.crt_ctx, &handle->common.ep,
			    FS_TO_OP(fs_handle, write_direct), &rpc);
	if (rc || !rpc) {
		IOF_TRACE_ERROR(handle, "Could not create request, rc = %u",
				rc);
		return -EIO;
	}
	IOF_TRACE_LINK(rpc, handle, "write_direct_rpc");

	iof_tracker_init(&reply.tracker, 1);
	in = crt_req_get(rpc);
	in->gah = handle->common.gah;
	d_iov_set(&in->data, (void *)buff, len);
	in->base = position;

	reply.handle = handle;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send open rpc, rc = %u", rc);
		return -EIO;
	}

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	if (reply.err != 0)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	return reply.len;
}

int ioc_write_bulk(const char *buff, size_t len, off_t position,
		   struct iof_file_handle *handle)
{
	struct iof_projection_info *fs_handle = handle->fs_handle;
	struct iof_write_bulk *in;
	crt_bulk_t bulk;
	struct write_cb_r reply = {0};

	d_sg_list_t sgl = {0};
	d_iov_t iov = {0};
	crt_rpc_t *rpc = NULL;
	int rc;

	rc = crt_req_create(fs_handle->proj.crt_ctx, &handle->common.ep,
			    FS_TO_OP(fs_handle, write_bulk), &rpc);
	if (rc || !rpc) {
		IOF_TRACE_ERROR(handle, "Could not create request, rc = %u",
				rc);
		return -EIO;
	}
	IOF_TRACE_LINK(rpc, handle, "write_bulk_rpc");

	in = crt_req_get(rpc);

	in->gah = handle->common.gah;

	iov.iov_len = len;
	iov.iov_buf_len = len;
	iov.iov_buf = (void *)buff;
	sgl.sg_iovs = &iov;
	sgl.sg_nr.num = 1;

	rc = crt_bulk_create(fs_handle->proj.crt_ctx, &sgl, CRT_BULK_RO,
			     &in->bulk);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Failed to make local bulk handle %d",
				rc);
		return -EIO;
	}

	iof_tracker_init(&reply.tracker, 1);
	in->base = position;

	bulk = in->bulk;

	reply.handle = handle;

	rc = crt_req_send(rpc, write_cb, &reply);
	if (rc) {
		IOF_TRACE_ERROR(handle, "Could not send open rpc, rc = %u", rc);
		return -EIO;
	}

	iof_fs_wait(&fs_handle->proj, &reply.tracker);

	rc = crt_bulk_free(bulk);
	if (rc)
		return -EIO;

	if (reply.err != 0)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	return reply.len;
}

int ioc_write(const char *file, const char *buff, size_t len, off_t position,
	      struct fuse_file_info *fi)
{
	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;
	int rc;

	STAT_ADD(handle->fs_handle->stats, write);

	if (FS_IS_OFFLINE(handle->fs_handle))
		return -handle->fs_handle->offline_reason;

	if (!IOF_IS_WRITEABLE(handle->fs_handle->flags)) {
		IOF_TRACE_INFO(handle, "Attempt to modify Read-Only File "
			       "System");
		return -EROFS;
	}

	IOF_TRACE_INFO(handle, "%#zx-%#zx " GAH_PRINT_STR, position,
		       position + len - 1, GAH_PRINT_VAL(handle->common.gah));

	if (!handle->common.gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		return -EIO;
	}

	if (len >= handle->fs_handle->proj.max_iov_write)
		rc = ioc_write_bulk(buff, len, position, handle);
	else
		rc = ioc_write_direct(buff, len, position, handle);

	if (rc > 0)
		STAT_ADD_COUNT(handle->fs_handle->stats, write_bytes, rc);

	return rc;
}

void ioc_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buff, size_t len,
		  off_t position, struct fuse_file_info *fi)
{	int rc;

	rc = ioc_write(NULL, buff, len, position, fi);

	if (rc < 0)
		IOF_FUSE_REPLY_ERR(req, -rc);
	else
		fuse_reply_write(req, rc);
}
