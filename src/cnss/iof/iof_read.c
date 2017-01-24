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

#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#else
#include <fuse/fuse.h>
#endif

#include "iof_common.h"
#include "iof.h"
#include "log.h"
#include "ios_gah.h"

struct read_cb_r {
	struct iof_data_out *out;
	struct iof_file_handle *handle;
	crt_rpc_t *rpc;
	int complete;
	int err;
	int rc;
};

static int read_cb(const struct crt_cb_info *cb_info)
{
	struct read_cb_r *reply = NULL;
	struct iof_data_out *out;
	int rc;

	reply = (struct read_cb_r *)cb_info->cci_arg;

	if (cb_info->cci_rc != 0) {
		/*
		 * Error handling.  On timeout return EAGAIN, all other errors
		 * return EIO.
		 *
		 * TODO: Handle target eviction here
		 */
		IOF_LOG_INFO("Bad RPC reply %d", cb_info->cci_rc);
		if (cb_info->cci_rc == -CER_TIMEDOUT)
			reply->err = EAGAIN;
		else
			reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	out = crt_reply_get(cb_info->cci_rpc);
	if (!out) {
		IOF_LOG_ERROR("Could not get reply");
		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	if (out->err) {
		IOF_LOG_ERROR("Error from target %d", out->err);

		if (out->err == IOF_GAH_INVALID)
			reply->handle->gah_valid = 0;

		reply->err = EIO;
		reply->complete = 1;
		return 0;
	}

	if (out->rc) {
		reply->rc = out->rc;
		reply->complete = 1;
		return 0;
	}

	rc = crt_req_addref(cb_info->cci_rpc);
	if (rc) {
		IOF_LOG_ERROR("could not take reference on query RPC, rc = %d",
			      rc);
		reply->err = EIO;
	} else {
		reply->out = out;
		reply->rpc = cb_info->cci_rpc;
	}

	reply->complete = 1;
	return 0;
}

int ioc_read(const char *file, char *buff, size_t len, off_t position,
	     struct fuse_file_info *fi)
{
	struct fuse_context *context;
	struct iof_read_in *in;
	struct read_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct iof_state *iof_state = NULL;
	crt_rpc_t *rpc = NULL;
	int rc;

	struct iof_file_handle *handle = (struct iof_file_handle *)fi->fh;

	IOF_LOG_INFO("path %s %s handle %p", file, handle->name, handle);

	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	iof_state = fs_handle->iof_state;
	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		return -EIO;
	}

	if (!handle->gah_valid) {
		/* If the server has reported that the GAH is invalid
		 * then do not send a RPC to close it
		 */
		free(handle);
		return -EIO;
	}

	rc = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			    READ_OP, &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->gah = handle->gah;
	in->base = position;
	in->len = len;

	reply.handle = handle;

	rc = crt_req_send(rpc, read_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send open rpc, rc = %u", rc);
		return -EIO;
	}
	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);
	if (rc)
		return -rc;

	if (reply.err)
		return -reply.err;

	if (reply.rc != 0)
		return -reply.rc;

	len = reply.out->data.iov_len;

	if (len > 0)
		memcpy(buff, reply.out->data.iov_buf, reply.out->data.iov_len);

	rc = crt_req_decref(reply.rpc);
	if (rc)
		IOF_LOG_ERROR("decref returned %d", rc);

	return len;
}

