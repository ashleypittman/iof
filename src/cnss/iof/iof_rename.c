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

int ioc_rename(const char *src, const char *dst)
{
	struct fuse_context *context;
	struct iof_rename_in *in = NULL;
	struct status_cb_r reply = {0};
	struct fs_handle *fs_handle;
	struct iof_state *iof_state = NULL;
	crt_rpc_t *rpc = NULL;
	int rc;

	IOF_LOG_INFO("src %s dst %s", src, dst);

	context = fuse_get_context();
	fs_handle = (struct fs_handle *)context->private_data;
	iof_state = fs_handle->iof_state;
	if (!iof_state) {
		IOF_LOG_ERROR("Could not retrieve iof state");
		return -EIO;
	}

	rc = crt_req_create(iof_state->crt_ctx, iof_state->dest_ep,
			    FS_TO_OP(fs_handle, rename), &rpc);
	if (rc || !rpc) {
		IOF_LOG_ERROR("Could not create request, rc = %u",
			      rc);
		return -EIO;
	}

	in = crt_req_get(rpc);
	in->src = (crt_string_t)src;
	in->dst = (crt_string_t)dst;
	in->my_fs_id = (uint64_t)fs_handle->my_fs_id;

	reply.complete = 0;

	rc = crt_req_send(rpc, ioc_status_cb, &reply);
	if (rc) {
		IOF_LOG_ERROR("Could not send rpc, rc = %u", rc);
		return -EIO;
	}
	rc = ioc_cb_progress(iof_state->crt_ctx, context, &reply.complete);

	if (rc)
		return -rc;

	IOF_LOG_DEBUG("path rc %d", IOC_STATUS_TO_RC(reply));

	return IOC_STATUS_TO_RC(reply);
}

#if IOF_USE_FUSE3
int ioc_rename3(const char *src, const char *dst, unsigned int flags)
{
	if (flags) {
		IOF_LOG_INFO("Unsupported rename flags %x", flags);
		return -ENOTSUP;
	}
	return ioc_rename(src, dst);
}
#endif