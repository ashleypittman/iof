
#ifdef IOF_USE_FUSE3
#include <fuse3/fuse.h>
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <pthread.h>
#include <sys/xattr.h>
#ifdef __APPLE__
#include <mach/clock.h>
#include <mach/mach.h>
#else
#include <time.h>
#endif
#include <mcl_event.h>
#include <process_set.h>

#include "iof_test_log.h"
#include "rpc_handler.h"
#include "server_backend.h"


int shutdown;
struct rpc_state {
	struct rpc_id *rpc_id;
	hg_addr_t dest_addr;
	struct mcl_context *mcl_context;
};
struct thread_args {
	char *mountpoint;
	int argc;
	char **argv;
	struct rpc_state rpc_state;
	int error_code;
};

struct getattr_r_t {
	struct stat *stbuf;
	int err_code;
	struct mcl_event event;
};

struct readdir_r_t {
	uint64_t complete;
	uint64_t err_code;
	char name[255];
	struct stat stat;
	struct mcl_event event;
};

struct rpc_cb_basic {
	uint64_t err_code;
	struct mcl_event event;
};

struct readlink_r_t {
	char *dst;
	uint64_t err_code;
	struct mcl_event event;
};

void get_my_time(struct timespec *ts)
{
#ifdef __APPLE__
	clock_serv_t cclock;
	mach_timespec_t mts;

	host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
#else
	clock_gettime(CLOCK_MONOTONIC, ts);
#endif

}
/*
 * Usage of this function does not prototype the product functionality and is a
 * temporary arrangement to handle server death and unmount fuse mount.
 *
 */
int iof_progress(struct mcl_context *mcl_context, struct mcl_event *cb_event,
		int iof_timeout)
{
	int ret;
	struct timespec start, end;
	double time_spent;
	double timeout;

	timeout = (double)iof_timeout/1000;

	ret = 0;
	get_my_time(&start);
	while (!mcl_event_test(cb_event)) {
		mcl_progress(mcl_context, NULL);
		get_my_time(&end);
		time_spent = (double) end.tv_sec - start.tv_sec;
		if ((timeout - time_spent) <= 0) {
			ret = 1;
			break;
		}
	}

	return ret;
}

struct rpc_id *create_id(hg_class_t *rpc_class)
{
	struct rpc_id *rpc_id;

	rpc_id = malloc(sizeof(struct rpc_id));
	if (rpc_id == NULL) {
		IOF_TESTLOG_ERROR("Cant allocate memory");
		exit(1);
	}
	rpc_id->getattr_id = getattr_register(rpc_class);
	rpc_id->readdir_id = readdir_register(rpc_class);
	rpc_id->mkdir_id = mkdir_register(rpc_class);
	rpc_id->rmdir_id = rmdir_register(rpc_class);
	rpc_id->symlink_id = symlink_register(rpc_class);
	rpc_id->readlink_id = readlink_register(rpc_class);
	rpc_id->unlink_id = unlink_register(rpc_class);
	return rpc_id;
}

static hg_return_t getattr_callback(const struct hg_cb_info *info)
{
	struct getattr_r_t *reply;
	struct getattr_out_t out = {0};
	uint64_t ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	memcpy(reply->stbuf, &out.stbuf, sizeof(struct stat));
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	mcl_event_set(&reply->event);

	return HG_SUCCESS;
}

static int fs_getattr(const char *name, struct stat *stbuf)
{
	struct fuse_context *context;
	uint64_t ret;
	struct rpc_state *state;
	hg_handle_t handle;
	struct mcl_context *mcl_context;
	struct rpc_request_string in;
	struct getattr_r_t reply = {0};

	in.name = name;
	reply.stbuf = stbuf;
	mcl_event_clear(&reply.event);
	context = fuse_get_context();
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->getattr_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create getattr handle");

	/* Send RPC */
	IOF_TESTLOG_INFO("Reply is at %p", (void *)&reply);
	ret =
	    HG_Forward(handle, getattr_callback, &reply,
		       &in);
	assert(ret == 0);

	ret = iof_progress(state->mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}
	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);

	return reply.err_code;
}

static hg_return_t readdir_callback(const struct hg_cb_info *info)
{
	struct readdir_out_t out = {0};
	struct readdir_r_t *reply = info->arg;
	uint64_t ret;

	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	IOF_TESTLOG_INFO("%s", out.name);
	strcpy(reply->name, out.name);
	memcpy(&reply->stat, &out.stat, sizeof(out.stat));
	reply->complete = out.complete;
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	mcl_event_set(&reply->event);

	return HG_SUCCESS;
}

static int fs_opendir(const char *dir, struct fuse_file_info *fi)
{
	fi->fh = (uint64_t)strdup(dir);

	if (!fi->fh)
		return -ENOMEM;

	return 0;
}

static int
fs_readdir(const char *dir_name, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi
#ifdef IOF_USE_FUSE3
	, enum fuse_readdir_flags flags
#endif
	)
{
	struct fuse_context *context;
	struct readdir_r_t reply = {0};
	uint64_t ret;
	struct readdir_in_t in;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	if (dir_name && strcmp(dir_name, (char *)fi->fh) != 0)
		return -EINVAL;

	if (!dir_name)
		dir_name = (char *)fi->fh;

	mcl_event_clear(&reply.event);

	context = fuse_get_context();
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->readdir_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create readdir handle");
	in.offset = 0;
	in.dir_name = dir_name;
	do {
		ret =
		    HG_Forward(handle, readdir_callback,
			       &reply, &in);
		assert(ret == 0);
		ret = iof_progress(state->mcl_context, &reply.event, 6000);
		if (ret) {
			IOF_TESTLOG_ERROR("Exiting Fuse loop");
			fuse_session_exit(fuse_get_session(context->fuse));
			return -EINTR;
		}

		if (reply.err_code != 0)
			return reply.err_code;
		IOF_TESTLOG_INFO("Calling filler %s", reply.name);
		filler(buf, reply.name, &reply.stat, 0
#ifdef IOF_USE_FUSE3
			, 0
#endif
			);
		in.offset++;
		mcl_event_clear(&reply.event);
	} while (!reply.complete);

	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return 0;
}

static int fs_closedir(const char *dir, struct fuse_file_info *fi)
{
	free((void *)fi->fh);
	return 0;
}

static hg_return_t basic_callback(const struct hg_cb_info *info)
{
	struct rpc_reply_basic out = {0};
	struct rpc_cb_basic *reply;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	mcl_event_set(&reply->event);

	return HG_SUCCESS;
}

static int fs_mkdir(const char *name, mode_t mode)
{
	struct fuse_context *context;
	struct mkdir_in_t in;
	struct rpc_cb_basic reply = {0};
	int ret;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	context = fuse_get_context();
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->mkdir_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create mkdir handle");
	in.name = name;
	in.mode = mode;

	mcl_event_set(&reply.event);

	ret = HG_Forward(handle, basic_callback, &reply, &in);
	assert(ret == 0);

	ret = iof_progress(state->mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}

	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return reply.err_code;
}

static int fs_rmdir(const char *name)
{
	struct fuse_context *context;
	struct rpc_cb_basic reply = {0};
	struct rpc_request_string in;
	int ret;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	mcl_event_clear(&reply.event);
	context = fuse_get_context();
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->rmdir_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create rmdir handle");
	in.name = name;
	ret = HG_Forward(handle, basic_callback, &reply, &in);
	assert(ret == 0);

	ret = iof_progress(mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}


	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return reply.err_code;
}

static int fs_symlink(const char *dst, const char *name)
{
	struct fuse_context *context;
	struct symlink_in_t in;
	struct rpc_cb_basic reply = {0};
	int ret;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	mcl_event_clear(&reply.event);
	context = fuse_get_context();
	in.name = name;
	in.dst = dst;
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->symlink_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create symlink handle");

	ret =
	    HG_Forward(handle, basic_callback, &reply, &in);
	assert(ret == 0);

	ret = iof_progress(mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}

	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return reply.err_code;
}

hg_return_t readlink_callback(const struct hg_cb_info *info)
{
	struct readlink_r_t *reply;
	struct readlink_out_t out = {0};
	int ret;

	reply = info->arg;

	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == HG_SUCCESS);

	reply->err_code = out.error_code;
	strncpy(reply->dst, out.dst, strlen(out.dst)+1);

	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == 0);

	mcl_event_set(&reply->event);

	return HG_SUCCESS;
}

static int fs_readlink(const char *name, char *dst, size_t length)
{
	struct fuse_context *context;
	struct rpc_request_string in;
	struct readlink_r_t reply = {0};
	int ret;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	mcl_event_clear(&reply.event);
	context = fuse_get_context();
	in.name = name;
	reply.dst = dst;
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->readlink_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create readlink handle");
	ret =
	    HG_Forward(handle, readlink_callback, &reply,
		       &in);
	assert(ret == 0);

	ret = iof_progress(mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}

	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return reply.err_code;
}

hg_return_t unlink_callback(const struct hg_cb_info *info)
{
	struct rpc_cb_basic *reply;
	struct rpc_reply_basic out;
	int ret;

	reply = info->arg;
	ret = HG_Get_output(info->info.forward.handle, &out);
	assert(ret == 0);

	reply->err_code = out.error_code;

	ret = HG_Free_output(info->info.forward.handle, &out);
	assert(ret == 0);

	mcl_event_set(&reply->event);

	return HG_SUCCESS;
}

static int fs_unlink(const char *name)
{
	struct fuse_context *context;
	struct rpc_request_string in;
	struct rpc_cb_basic reply = {0};
	int ret;
	struct rpc_state *state;
	struct mcl_context *mcl_context;
	hg_handle_t handle;

	mcl_event_clear(&reply.event);
	context = fuse_get_context();
	state = (struct rpc_state *)context->private_data;
	mcl_context = state->mcl_context;
	ret = HG_Create(mcl_context->context, state->dest_addr,
			state->rpc_id->unlink_id, &handle);
	if (ret)
		IOF_TESTLOG_ERROR("Couldnt create unlink handle");
	in.name = name;
	ret =
	    HG_Forward(handle, unlink_callback, &reply, &in);
	assert(ret == 0);

	ret = iof_progress(mcl_context, &reply.event, 6000);
	if (ret) {
		IOF_TESTLOG_ERROR("Exiting Fuse loop");
		fuse_session_exit(fuse_get_session(context->fuse));
		return -EINTR;
	}

	ret = HG_Destroy(handle);
	assert(ret == HG_SUCCESS);
	return reply.err_code;
}

static int string_to_bool(const char *str, int *value)
{
	if (strcmp(str, "1") == 0) {
		*value = 1;
		return 1;
	} else if (strcmp(str, "0") == 0) {
		*value = 0;
		return 1;
	}
	return 0;
}
#ifdef __APPLE__

static int fs_setxattr(const char *path,  const char *name, const char *value,
			size_t size, int options, uint32_t position)
#else
static int fs_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
#endif
{
	struct fuse_context *context;
	int ret;

	context = fuse_get_context();
	if (strcmp(name, "user.exit") == 0) {
		if (string_to_bool(value, &ret)) {
			if (ret) {
				IOF_TESTLOG_INFO("Exiting fuse loop");
				fuse_session_exit(
					fuse_get_session(context->fuse));
				return -EINTR;
			} else
				return 0;
		}
		return -EINVAL;
	}
	return -ENOTSUP;
}

static struct fuse_operations op = {
#ifdef IOF_USE_FUSE3
	.flag_nopath = 1,
#endif
	.opendir = fs_opendir,
	.readdir = fs_readdir,
	.releasedir = fs_closedir,
	.getattr = fs_getattr,
	.mkdir = fs_mkdir,
	.rmdir = fs_rmdir,
	.symlink = fs_symlink,
	.readlink = fs_readlink,
	.unlink = fs_unlink,
	.setxattr = fs_setxattr,
};

static void my_handler(int sig)
{
	shutdown = 1;
}

static void *thread_function(void *data)
{
	int res;
	struct thread_args *t_args = (struct thread_args *)data;
	struct fuse_args args;
	struct fuse_chan *ch;
	struct fuse *fuse;

	args.argc = t_args->argc;
	args.argv = t_args->argv;
	args.allocated = 0;

	ch = fuse_mount(t_args->mountpoint, &args);
	if (!ch) {
		fuse_opt_free_args(&args);
		IOF_TESTLOG_ERROR("Could not successfully mount");
		t_args->error_code = 1;
		return NULL;
	}
	fuse = fuse_new(ch, &args, &op, sizeof(op), &t_args->rpc_state);
	if (!fuse) {
		IOF_TESTLOG_ERROR("Could not initialize fuse");
		t_args->error_code = 1;
		fuse_opt_free_args(&args);
		return NULL;
	}
	fuse_opt_free_args(&args);
	/*Blocking*/
	res = fuse_loop(fuse);
	if (res != 0) {
		IOF_TESTLOG_ERROR("Fuse loop exited with ret = %d", res);
		t_args->error_code = res;
	}
	mcl_remove_context(t_args->rpc_state.mcl_context);

	fuse_unmount(t_args->mountpoint, ch);
	fuse_destroy(fuse);
	return NULL;
}

int main(int argc, char **argv)
{
	na_class_t *na_class = NULL;
	hg_class_t *rpc_class = NULL;
	char *uri;
	struct mcl_set *set = NULL;
	struct mcl_state *proc_state;
	pthread_t *worker_thread;
	struct thread_args *t_args;
	char *base_mount = NULL;
	int new_argc = 0;
	int i;
	char **new_argv = NULL;
	struct rpc_id *rpc_id;

	int is_service = 0;
	char *name_of_set = "client";
	char *name_of_target_set = "server";
	struct mcl_set *dest_set = NULL;
	int ret;

	iof_testlog_init("client_main");
	proc_state = mcl_init(&uri);
	na_class = NA_Initialize(uri, NA_FALSE);
	mcl_startup(proc_state, na_class, name_of_set, is_service, &set);
	rpc_class = proc_state->hg_class;
	rpc_id = create_id(rpc_class);
	ret = mcl_attach(proc_state, name_of_target_set, &dest_set);
	if (ret != MCL_SUCCESS) {
		IOF_TESTLOG_ERROR("attach failed");
		exit(1);
	}
	shutdown = 0;
	new_argv = (char **)malloc(argc * sizeof(char *));
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-mnt") == 0) {
			i++;
			base_mount = strdup(argv[i]);
		} else
			new_argv[new_argc++] = argv[i];
	}
	if (!base_mount) {
		IOF_TESTLOG_ERROR(
		"Please provide a valid base mount point with -mnt option");
		exit(1);
	}
	if (((signal(SIGTERM, my_handler)) == SIG_ERR) ||
	    ((signal(SIGINT, my_handler)) == SIG_ERR) ||
	    ((signal(SIGHUP, my_handler)) == SIG_ERR) ||
	    ((signal(SIGPIPE, SIG_IGN)) == SIG_ERR)) {
		perror("could not set signal handler");
		exit(1);
	}
	t_args = calloc(dest_set->size, sizeof(struct thread_args));
	if (!t_args)
		exit(1);
	worker_thread = calloc(dest_set->size, sizeof(pthread_t));
	if (!worker_thread)
		exit(1);

	for (i = 0; i < dest_set->size; i++) {
		/*create directories for mounting*/
		t_args[i].mountpoint = malloc(MAX_NAME_LEN * sizeof(char));
		sprintf(t_args[i].mountpoint, "%s/Rank%d", base_mount, i);
		if ((mkdir(t_args[i].mountpoint, 0755) && errno != EEXIST))
			exit(1);
		t_args[i].argc = new_argc;
		t_args[i].argv = new_argv;
		t_args[i].error_code = 0;
		t_args[i].rpc_state.rpc_id = rpc_id;
		t_args[i].rpc_state.mcl_context = mcl_get_context(proc_state);

		ret = mcl_lookup(dest_set, i, t_args[i].rpc_state.mcl_context,
				&t_args[i].rpc_state.dest_addr);
		if (ret != MCL_SUCCESS) {
			IOF_TESTLOG_ERROR("Server address lookup failed");
			exit(1);
		}
		pthread_create(&worker_thread[i], NULL, thread_function,
			       (void *)&t_args[i]);
	}

	for (i = 0; i < dest_set->size; i++)
		pthread_join(worker_thread[i], NULL);

	for (i = 0; i < dest_set->size; i++) {
		if (t_args[i].error_code != 0)
			ret = t_args[i].error_code;
	}

	for (i = 0; i < argc; i++)
		free(new_argv[i]);
	free(new_argv);
	for (i = 0; i < dest_set->size; i++)
		free(t_args[i].mountpoint);
	free(t_args);
	free(worker_thread);
	mcl_detach(proc_state, dest_set);
	mcl_finalize(proc_state);
	NA_Finalize(na_class);
	iof_testlog_close();
	return ret;
}
