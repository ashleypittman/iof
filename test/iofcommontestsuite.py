#!/usr/bin/env python3
# Copyright (C) 2016 Intel Corporation
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
iof common test suite

Usage:

The common module is imported and executed by the IOF test suites.
The module contains the common/supporting functions for the tests:
setUp, tearDown, create the input/command string and set the log dir.

The results are placed in the testLogs/nss directory.
Any test_ps output is under
<file yaml>_loop#/<module.name.execStrategy.id>/1(process set)/rank<number>.
There you will find anything written to stdout and stderr. The output from
memcheck and callgrind are in the nss directory. At the end of a test run,
the last nss directory is renamed to nss<date stamp>

To use valgrind memory checking
set TR_USE_VALGRIND in iof_test_ionss.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in iof_test_ionss.yml to callgrind

To redirect output to the log file,
set TR_REDIRECT_OUTPUT in iof_test_ionss.yml
The test_runner wil set it to redirect the output to the log file.
By default the output is displayed on the screen.
"""
#pylint: disable=too-many-locals
#pylint: disable=broad-except
import os
import unittest
import shlex
import subprocess
import time
import getpass
import tempfile
import logging

def commonSetUpModule():
    """ set up test environment """

    print("\nTestnss: module setup begin")
    tempdir = tempfile.mkdtemp()
    os.environ["CNSS_PREFIX"] = tempdir
    cts = CommonTestSuite()
    allnode_cmd = cts.common_get_all_cn_cmd()
    testmsg = "create %s on all CNs" % tempdir
    cmdstr = "%smkdir -p %s " % (allnode_cmd, tempdir)
    cts.common_launch_test(testmsg, cmdstr)
    cts.common_manage_ionss_dir()
    print("Testnss: module setup end\n\n")

class CommonTestSuite(unittest.TestCase):
    """Attributes common to the IOF tests"""
    fs_list = []
    logger = logging.getLogger("TestRunnerLogger")
    __ion_dir = None

    @staticmethod
    def common_get_all_cn_cmd():
        """Get prefix to run command to run all all CNs"""
        cn = os.getenv('IOF_TEST_CN')
        if cn:
            return "pdsh -S -R ssh -w %s " % cn
        return ""

    @staticmethod
    def common_get_all_ion_cmd():
        """Get prefix to run command on all CNs"""
        ion = os.getenv('IOF_TEST_ION')
        if ion:
            return "pdsh -S -R ssh -w %s " % ion
        return ""

    def common_manage_ionss_dir(self):
        """create dirs for IONSS backend"""
        ion_dir = tempfile.mkdtemp()
        self.__ion_dir = ion_dir
        allnode_cmd = self.common_get_all_ion_cmd()
        testmsg = "create base ION dirs %s" % ion_dir
        cmdstr = "%smkdir -p %s " % (allnode_cmd, ion_dir)
        self.common_launch_test(testmsg, cmdstr)
        i = 2
        while i > 0:
            fs = "FS_%s" % i
            abs_path = os.path.join(ion_dir, fs)
            self.fs_list.append(abs_path)
            testmsg = "creating dirs to be used as Filesystem backend"
            cmdstr = "%smkdir -p %s" % (allnode_cmd, abs_path)
            self.common_launch_test(testmsg, cmdstr)
            i = i - 1

    def common_launch_test(self, msg, cmdstr):
        """Launch a test and wait for it to complete"""
        self.logger.info("Testnss: start %s - input string:\n %s\n", \
          msg, cmdstr)
        cmdarg = shlex.split(cmdstr)
        if not os.getenv('TR_REDIRECT_OUTPUT', ""):
            procrtn = subprocess.call(cmdarg, timeout=180)
        else:
            procrtn = subprocess.call(cmdarg, timeout=180,
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
        return procrtn

    def common_launch_process(self, msg, cmdstr):
        """Launch a process"""
        self.logger.info("Testnss: start %s - input string:\n %s\n", \
          msg, cmdstr)
        cmdarg = shlex.split(cmdstr)
        if not os.getenv('TR_REDIRECT_OUTPUT', ""):
            proc = subprocess.Popen(cmdarg)
        else:
            proc = subprocess.Popen(cmdarg,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
        return proc

    def common_stop_process(self, proc):
        """wait for processes to terminate

        Wait for up to 60 seconds for the process to die on it's own, then if
        still running attept to kill it.

        Return the error code of the process, or -1 if the process was killed.
        """
        self.logger.info("Test: stopping processes :%s", proc.pid)
        i = 60
        procrtn = None
        while i:
            proc.poll()
            procrtn = proc.returncode
            if procrtn is not None:
                break
            else:
                time.sleep(1)
                i = i - 1

        if procrtn is None:
            procrtn = -1
            try:
                proc.terminate()
                proc.wait(2)
            except ProcessLookupError:
                pass
            except Exception:
                self.logger.info("Killing processes: %s", proc.pid)
                proc.kill()

        self.logger.info("Test: return code: %s\n", procrtn)
        return procrtn

    @staticmethod
    def common_logdir_name(fullname):
        """create the log directory name"""
        names = fullname.split('.')
        items = names[-1].split('_', maxsplit=2)
        return "/" + items[2]

    def common_add_prefix_logdir(self, testcase_name):
        """add the log directory to the prefix"""
        prefix = ""
        ompi_bin = os.getenv('IOF_OMPI_BIN', "")
        log_path = os.getenv("IOF_TESTLOG", "nss") + \
          self.common_logdir_name(testcase_name)
        os.makedirs(log_path, exist_ok=True)
        use_valgrind = os.getenv('TR_USE_VALGRIND', default="")
        if use_valgrind == 'memcheck':
            suppressfile = os.path.join(os.getenv('IOF_CART_PREFIX', ".."), \
                           "etc", "memcheck-cart.supp")
            prefix = "valgrind --xml=yes" + \
                " --xml-file=" + log_path + "/valgrind.%q{PMIX_ID}.xml" + \
                " --leak-check=yes --gen-suppressions=all" + \
                " --suppressions=" + suppressfile + " --show-reachable=yes"
        elif use_valgrind == "callgrind":
            prefix = "valgrind --tool=callgrind --callgrind-out-file=" + \
                     log_path + "/callgrind.%q{PMIX_ID}.out"

        if os.getenv('TR_USE_URI', ""):
            dvmfile = " --hnp file:%s " % os.getenv('TR_USE_URI')
        else:
            dvmfile = " "
        if getpass.getuser() == "root":
            allow_root = " --allow-run-as-root"
        else:
            allow_root = ""
        cmdstr = "%sorterun%s--output-filename %s%s" % \
                 (ompi_bin, dvmfile, log_path, allow_root)

        return (cmdstr, prefix)

    @staticmethod
    def common_add_server_client():
        """create the server and client prefix"""
        cn = os.getenv('IOF_TEST_CN')
        if cn:
            local_cn = " -H %s -n 1 " % cn
        else:
            local_cn = " -np 1 "
        ion = os.getenv('IOF_TEST_ION')
        if ion:
            local_ion = " -H %s -n 1 " % ion
        else:
            local_ion = " -np 1"

        return (local_cn, local_ion)

    def commonTearDownModule(self):
        """teardown module for test"""

        print("Testnss: module tearDown begin")
        testmsg = "terminate any cnss processes"
        cmdstr = "pkill cnss"
        testmsg = "terminate any ionss processes"
        cmdstr = "pkill ionss"
        cts = CommonTestSuite()
        cts.common_launch_test(testmsg, cmdstr)
        allnode_cmd = cts.common_get_all_cn_cmd()
        testmsg = "remove %s on all CNs" % os.environ["CNSS_PREFIX"]
        cmdstr = "%srm -rf %s " % (allnode_cmd, os.environ["CNSS_PREFIX"])
        cts.common_launch_test(testmsg, cmdstr)
        allnode_cmd = cts.common_get_all_ion_cmd()
        if self.__ion_dir is not None:
            testmsg = "remove %s on all IONs" % self.__ion_dir
            cmdstr = "%srm -rf %s " % (allnode_cmd, self.__ion_dir)
            cts.common_launch_test(testmsg, cmdstr)
        print("Testnss: module tearDown end\n\n")