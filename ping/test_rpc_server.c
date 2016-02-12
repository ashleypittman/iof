#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pmix.h>
#include <mercury.h>

#include "my_rpc_common.h"

int get_uri(char **uri);

int main(int argc, char **argv)
{
    hg_id_t         my_rpc_id;
    na_class_t      *na_class = NULL;
    na_context_t    *na_context = NULL;
    hg_class_t      *hg_class = NULL;
    hg_context_t    *hg_context = NULL;
    hg_return_t ret;
    unsigned int act_count = 0, total_count = 0;
    pmix_proc_t myproc, proc;
    int rc;
    bool flag;
    pmix_info_t *info;

//    char *uri       = "bmi+tcp://localhost:8888";
    char *uri;
    get_uri(&uri);
    fprintf(stderr, "server uri: %s\n", uri);
    //============= begin PMIx stuff
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n",
                myproc.nspace, myproc.rank, rc);
        exit(0);
    }
    // publish server uri
    PMIX_INFO_CREATE(info, 1);
    (void)strncpy(info[0].key, "server-addr", PMIX_MAX_KEYLEN);
    info[0].value.type = PMIX_STRING;
    info[0].value.data.string = strdup(uri);
    if (PMIX_SUCCESS != (rc = PMIx_Publish(info, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Publish failed: %d\n", myproc.nspace, myproc.rank, rc);
    }
    PMIX_INFO_FREE(info, 1);

    /* call fence to ensure the data is received */
    PMIX_PROC_CONSTRUCT(&proc);
    (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;
    PMIX_INFO_CREATE(info, 1);
    flag = true;
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL)
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, info, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank, rc);
    }
    PMIX_INFO_FREE(info, 1);
    //============ end PMIx stuff

    na_class = NA_Initialize(uri, NA_TRUE);
    assert(na_class);
    na_context = NA_Context_create(na_class);
    assert(na_context);
    hg_class = HG_Init(na_class, na_context, NULL);
    assert(hg_class);
    hg_context = HG_Context_create(hg_class);
    assert(hg_context);

    my_rpc_id = HG_Register(hg_class, "rpc_test", my_in_proc_cb, my_out_proc_cb, my_rpc_test_handler);

    while (1) {
        do {
            ret = HG_Trigger(hg_class, hg_context, 0, 1, &act_count);
            total_count += act_count;
        } while (ret == HG_SUCCESS && act_count);
        HG_Progress(hg_class, hg_context, 100);
        if (total_count == 3) break;
    }

    HG_Context_destroy(hg_context);
    HG_Finalize(hg_class);
    NA_Context_destroy(na_class, na_context);
    NA_Finalize(na_class);

    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Finalize failed: %d\n",
                myproc.nspace, myproc.rank, rc);
    }

    return 0;
}


int get_uri(char **uri)
{
    int socketfd;
    struct sockaddr_in tmp_socket;
    char name[256 + 10 + 1]; // POSIX HOST_NAME_MAX + bmi+tcp://
    char hname[256];
    socklen_t slen = sizeof(struct sockaddr);


    socketfd = socket(AF_INET, SOCK_STREAM, 0);
    tmp_socket.sin_family = AF_INET;
    tmp_socket.sin_addr.s_addr = INADDR_ANY;
    tmp_socket.sin_port = 0;

    bind(socketfd, (const struct sockaddr *) &tmp_socket, sizeof(tmp_socket));
    getsockname(socketfd, (struct sockaddr *) &tmp_socket, &slen);
    gethostname(hname, 256);
    snprintf(name, 256 + 10 + 1, "bmi+tcp://%s:%d", hname, ntohs(tmp_socket.sin_port));
    *uri = strndup(name, 256 + 10);
    close(socketfd);

    return 0;
}
