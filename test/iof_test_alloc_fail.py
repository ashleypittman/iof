#!/usr/bin/env python3
# Copyright (C) 2016-2018 Intel Corporation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted for any purpose (including commercial purposes)
# provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions, and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions, and the following disclaimer in the
#    documentation and/or materials provided with the distribution.
#
# 3. In addition, redistributions of modified forms of the source or binary
#    code must carry prominent notices stating that the original code was
#    changed and the date of the change.
#
#  4. All publications or advertising materials mentioning features or use of
#     this software are asked, but not required, to acknowledge that it was
#     developed by Intel Corporation and credit the contributors.
#
# 5. Neither the name of Intel Corporation, nor the name of any Contributor
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""
Memory allocation failure testing for IOF.

Intercept memory allocations in CaRT to cause them to fail, and run the CNSS
to see how it behaves.  For each allocation point as the program runs fail
and see if the program exits cleanly and shuts down properly without leaking
any memory.

This test targets the startup procedure of CNSS, so does not yet require
a IONSS process, or indeed any network connectivity at all.
"""

import os
import sys
import yaml
import tempfile
import subprocess
import common_methods
import iof_cart_logparse
import iofcommontestsuite
import rpctrace_common_methods
import iof_test_local

def unlink_file(file_name):
    """Unlink a file without failing if it doesn't exist"""

    try:
        os.unlink(file_name)
    except FileNotFoundError:
        pass

class EndOfTest(Exception):
    """Raised when no injected fault is found"""
    pass

def run_once(prefix, cmd, log_top_dir, floc):
    """Run a single instance of the command"""
    print("Testing {}".format(floc))

    log_file = os.path.join(log_top_dir, 'af', 'fail_{}.log'.format(floc))
    internals_file = os.path.join(log_top_dir, 'af',
                                  'fail_{}.internals.log'.format(floc))
    unlink_file(log_file)

    y = {}
    y['fault_config'] = [{'id': 0,
                          'interval': floc,
                          'max_faults': 1}]

    fd = open(os.path.join(prefix, 'fi.yaml'), 'w+')
    fd.write(yaml.dump(y))
    fd.close()
    os.environ['D_FI_CONFIG'] = os.path.join(prefix, 'fi.yaml')
    os.environ['D_LOG_FILE'] = log_file
    os.environ['CRT_PHY_ADDR_STR'] = 'ofi+sockets'
    os.environ['OFI_INTERFACE'] = 'lo'
    rc = subprocess.call(cmd)
    print('Return code was {}'.format(rc))
    # This is a valgrind error code and means memory leaks
    if rc == 42:
        for line in iof_cart_logparse.IofLogIter(log_file):
            if not line.endswith('fault_id 0, injecting fault.'):
                continue
            common_methods.show_line(line, 'error',
                                     'Valgrind error when fault injected here')
            common_methods.show_bug(line, 'IOF-887')
        print("Alloc test failing with valgrind errors")
        sys.exit(1)
    # This means abnormal exit, probably a segv.
    if rc < 0:
        sys.exit(1)
    # This means it's not one of the CNSS_ERR error codes.
    if rc > 10:
        sys.exit(1)

    ifd = open(internals_file, 'w+')
    trace = rpctrace_common_methods.RpcTrace(log_file, ifd)
    pid = trace.pids[0]
    trace.rpc_reporting(pid)
    trace.descriptor_rpc_trace(pid)

    have_inject = False
    for line in trace.lf.new_iter():
        if not line.endswith('fault_id 0, injecting fault.'):
            continue
        have_inject = True
        print(line.to_str())
        break

    ifd.close()

    tl = iof_test_local.Testlocal()
    tl.check_log_file(log_file)

    if trace.have_errors:
        print("Internals tracing code found errors: {}".format(internals_file))
        sys.exit(1)

    if not have_inject:
        raise EndOfTest

def main():
    """Main function"""

    iofcommontestsuite.load_config()
    export_tmp_dir = os.getenv("IOF_TMP_DIR", '/tmp')
    prefix = tempfile.mkdtemp(prefix='iof_allocf_',
                              dir=export_tmp_dir)
    ctrl_fs_dir = os.path.join(prefix, '.ctrl')

    use_valgrind = os.getenv('TR_USE_VALGRIND', default=None)

    log_top_dir = os.getenv("IOF_TESTLOG",
                            os.path.join(os.path.dirname(
                                os.path.realpath(__file__)),
                                         'output',
                                         'alloc_fail',
                                         'cnss'))

    try:
        os.makedirs(os.path.join(log_top_dir, 'af'))
    except FileExistsError:
        pass

    crt_suppressfile = iofcommontestsuite.valgrind_cart_supp_file()
    iof_suppressfile = iofcommontestsuite.valgrind_iof_supp_file()

    os.environ['D_LOG_MASK'] = 'DEBUG'
    floc = 0

    while True:
        floc += 1
        subprocess.call(['fusermount', '-q', '-u', ctrl_fs_dir])
        cmd = ['orterun', '-n', '1']
        cmd.extend(['valgrind',
                    '--quiet',
                    '--leak-check=full',
                    '--show-leak-kinds=all',
                    '--suppressions={}'.format(iof_suppressfile),
                    '--error-exitcode=42'])
        if crt_suppressfile:
            cmd.append('--suppressions={}'.format(crt_suppressfile))

        # If running in a CI environment then output to xml.
        if use_valgrind == 'memcheck':
            cmd.extend(['--xml=yes',
                        '--xml-file={}'.format(
                            os.path.join(log_top_dir, 'af',
                                         "valgrind-{}.xml".format(floc)))])
        cmd.extend(['cnss', '-p', prefix])
        try:
            run_once(prefix, cmd, log_top_dir, floc)
        except EndOfTest:
            print("Ran without injecting error")
            break



if __name__ == '__main__':
    main()