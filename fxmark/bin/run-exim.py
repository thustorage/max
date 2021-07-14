#!/usr/bin/env python3
import optparse
import os
import subprocess
import tempfile

CUR_DIR = os.path.abspath(os.path.dirname(__file__))

class Exim(object):
    PERF_STR = "messages"

    def __init__(self, ncore_, duration_, root_,
                 profbegin_, profend_, proflog_):
        self.bench_out = None
        self.exim_path = os.path.normpath(os.path.join(CUR_DIR, "../vbench/exim/"))
        self.ncore = int(ncore_)
        self.duration = int(duration_)
        self.test_root = root_
        self.profbegin = profbegin_
        self.profend = profend_
        self.prolog = proflog_
        self.profenv = ' '.join(["PERFMON_LEVEL=%s" %
                                 os.environ.get('PERFMON_LEVEL', "x"),
                                 "PERFMON_LDIR=%s" %
                                 os.environ.get('PERFMON_LDIR', "x"),
                                 "PERFMON_LFILE=%s" %
                                 os.environ.get('PERFMON_LFILE', "x")])
        self.perf_msg = None
    def __del__(self):
        # clean up
        try:
            if self.bench_out:
                os.unlink(self.bench_out.name)
        except:
            pass

    def run(self):
        # gen config
        # cmd = ' '.join(["/home/lxj/fxmark/vbench/exim/mkconfig",
                        # "/home/lxj/fxmark/vbench/exim/exim-mod", self.test_root, self.test_root,
                        # "> /home/lxj/fxmark/vbench/exim/config"])
        cmd = ''.join([self.exim_path, "mkconfig ", self.exim_path, "exim-mod ", self.test_root, " ", self.test_root, " ", "> ", self.test_root, "config"])
        self.exec_cmd(cmd)
        cmd = ''.join([self.exim_path, "/exim-mod/bin/exim -bd -oX 2526 -C ", self.exim_path, "/config"])
        self._exec_cmd(cmd)
        # start performance profiling
        self._exec_cmd("%s %s" % (self.profenv, self.profbegin)).wait()
        # run exim
        self.run_exim()
        # stop performance profiling
        self._exec_cmd("%s %s" % (self.profenv, self.profend)).wait()

        self.report()

        self._exec_cmd("killall exim")

        return 0

    def report(self):
        print("# ncpu result")
        print("%s %s" % (self.ncore, self.perf_msg))

    def run_exim(self):
        with tempfile.NamedTemporaryFile(delete=False) as self.bench_out:
            cmd = ''.join([self.exim_path, "/run-smtpbm \"%s\" \"%s\"" % (str(self.ncore), 2526)])
            p = self._exec_cmd(cmd, subprocess.PIPE)
            while True:
                for l in p.stdout.readlines():
                    self.bench_out.write("#@ ".encode("utf-8"))
                    self.bench_out.write(l)
                    l_str = str(l)
                    idx = l_str.find(Exim.PERF_STR)
                    if idx is not -1:
                        self.perf_msg = l_str[idx + len(Exim.PERF_STR):]
                if self.perf_msg:
                    break
            self.bench_out.flush()

    def exec_cmd(self, cmd, out=None):
        p = subprocess.Popen(cmd, shell=True, stdout=out, stderr=out)
        p.wait()
        return p

    def _exec_cmd(self, cmd, out=None):
        p = subprocess.Popen(cmd, shell=True, stdout=out, stderr=out)
        return p


if __name__ == "__main__":
    parser = optparse.OptionParser()
    parser.add_option("--ncore", help="number of core")
    parser.add_option("--duration", help="benchmark time in seconds")
    parser.add_option("--root", help="benchmark root directory")
    parser.add_option("--profbegin", help="profile begin command")
    parser.add_option("--profend", help="profile end command")
    parser.add_option("--proflog", help="profile log path")
    (opts, args) = parser.parse_args()
    # check options
    for opt in vars(opts):
        val = getattr(opts, opt)
        if val == None:
            print("Missing options: %s" % opt)
            parser.print_help()
            exit(1)
    
    exim = Exim(opts.ncore, opts.duration, opts.root,
                      opts.profbegin, opts.profend, opts.proflog)
    rc = exim.run()
    exit(rc)
