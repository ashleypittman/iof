#!/bin/sh

set -e
set -x

. ./setup_local.sh

echo Trying to run Mercury ping test.

if [ -n "$IOF_USE_VALGRIND" ]; then
CMD_PREFIX="valgrind --xml=yes --xml-file=valgrind.%q{PMIX_ID}.xml "
CMD_PREFIX="$CMD_PREFIX --leak-check=yes --suppressions=memcheck-pmix.supp"
fi

echo `hostname` slots=8 > hostfile
orterun --hostfile hostfile --tag-output -np 1  $CMD_PREFIX test_rpc_server : \
	-np 1 $CMD_PREFIX test_rpc_client

echo Trying to run process set tests.

orterun --hostfile hostfile --tag-output -np 4 $CMD_PREFIX test_ps \
        --name service_set --is_service 1 : \
	    -np 4 $CMD_PREFIX test_ps --name client_set --is_service 0 \
        --attach_to service_set

orterun --hostfile hostfile --tag-output -np 1 $CMD_PREFIX test_ps \
    --name a --is_service 1 : \
	-np 1 $CMD_PREFIX test_ps --name b --attach-to c : \
	-np 1 $CMD_PREFIX test_ps --name a --is_service 1 : \
	-np 2 $CMD_PREFIX test_ps --name c --is_service --attach-to a

orterun --hostfile hostfile --tag-output -np 4 $CMD_PREFIX test_ps \
    --name test_srv_set --is_service 1

# Disable automatic exit whilst running FUSE so that we can attempt to
# shutdown correctly.  Instead for FUSE verify that we can create
# and then read back the target of a sym link.
set +e

[ -d child_fs ] || mkdir child_fs

orterun --hostfile hostfile -np 1 $CMD_PREFIX client_main -f child_fs \
	: -np 1 $CMD_PREFIX server_main &

ORTE_PID=$!
sleep 2
ls
ls child_fs
cd child_fs
mkdir d e
rm -r e
ls
ln -s d d_sym
ls
rm -r d
ls
rm d_sym
ls
ln -s target origin
LINK=`readlink origin`

cd ..

/bin/kill -TERM $ORTE_PID
sleep 2

if [ "$os" = "Darwin" ];then
    umount child_fs
else
    fusermount -u child_fs
fi

if [ -h child_fs/origin ]
then
    exit 1
fi

/bin/kill -TERM $ORTE_PID
sleep 1
/bin/kill -TERM $ORTE_PID
wait

if [ "$LINK" != "target" ]
then
    exit 1
fi
