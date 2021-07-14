#!/usr/bin/env python3
import os
import re
import subprocess
import tempfile
import optparse

CUR_DIR = os.path.abspath(os.path.dirname(__file__))

class Rocksdb(object):
    
    def __init__(self, type_, ncore_, duration_, root_, profbegin_, profend_, proflog_):
        self.bench_out = None
        self.rocksdb_path = os.path.normpath(os.path.join(CUR_DIR, "../vbench/rocksdb/db_bench"))
        self.type = type_
        self.ncore = int(ncore_)
        self.duration = int(duration_)
        self.root = root_
        self.profbegin = profbegin_
        self.profend = profend_
        self.proflog = proflog_
        self.profenv = ' '.join(["PERFMON_LEVEL=%s" %
                                 os.environ.get('PERFMON_LEVEL', "x"),
                                 "PERFMON_LDIR=%s" %
                                 os.environ.get('PERFMON_LDIR', "x"),
                                 "PERFMON_LFILE=%s" %
                                 os.environ.get('PERFMON_LFILE', "x")])
        self.perf_msg = None

    def __del__(self):
        try:
            if self.bench_out:
                os.unlink(self.bench_out.name)
        except:
            pass

    def run(self):
        # start performance profiling
        self._exec_cmd("%s %s" % (self.profenv, self.profbegin)).wait()
        # run filebench
        self._exec_cmd("ulimit -n 65536").wait()
        self._run_rocksdb()
        # stop performance profiling
        self._exec_cmd("%s %s" % (self.profenv, self.profend)).wait()
        return 0

    def _exec_cmd(self, cmd, out=None):
        p = subprocess.Popen(cmd, shell=True, stdout=out, stderr=out)
        return p

    def _run_rocksdb(self):
        with tempfile.NamedTemporaryFile(delete=False) as self.bench_out:
            cmd = "%s --benchmarks %s --db %s --duration %s --disable_wal true --disable_data_sync true " \
                  "--compression_type \"none\" --compression_level 0 --threads 2 --readwritepercent 50 " \
                  "--max_background_compactions %d --max_background_flushes %d --value_size 8192" \
                  % (self.rocksdb_path, self.type, self.root, self.duration,
                     self.ncore, self.ncore)
            p = self._exec_cmd(cmd, subprocess.PIPE)
            while True:
                for l in p.stdout.readlines():
                    self.bench_out.write("#@ ".encode("utf-8"))
                    self.bench_out.write(l)
                    l_str = str(l)
                    idx = l_str.find(self.type)
                    if idx is not -1:
                        self.perf_msg = l_str[idx + len(self.type):]
                if self.perf_msg:
                    break
            self.bench_out.flush()

    def report(self):
        ms = re.findall("\d+[.]?\d* ops", self.perf_msg)
        profile_name = ""
        profile_data = ""
        try:
            with open(self.proflog, "r") as fpl:
                l = fpl.readlines()
                if len(l) >= 2:
                    profile_name = l[0]
                    profile_data = l[1]
        except:
            pass
        print("# ncpu secs works works/sec %s" % profile_name)
        print("%s %s %s %s %s" % (self.ncore, self.duration, 0, int(ms[0].split()[0]), profile_data))


if __name__ == "__main__":
    parser = optparse.OptionParser()
    parser.add_option("--type", help="workload name")
    parser.add_option("--ncore", help="number of core")
    parser.add_option("--nbg", help="not used")
    parser.add_option("--duration", help="benchmark time in seconds")
    parser.add_option("--root", help="benchmark root directory")
    parser.add_option("--profbegin", help="profile begin command")
    parser.add_option("--profend", help="profile end command")
    parser.add_option("--proflog", help="profile log path")
    parser.add_option("--times", help="limited op times")
    parser.add_option("--directio", help="dicrect io")
    (opts, args) = parser.parse_args()
    for opt in vars(opts):
        val = getattr(opts, opt)
        if val == None:
            print("Missing options: %s" % opt)
            parser.print_help()
            exit(1)
    rocksdb = Rocksdb(opts.type, opts.ncore, opts.duration, opts.root,
                      opts.profbegin, opts.profend, opts.proflog)
    rc = rocksdb.run()
    rocksdb.report()
    exit(rc)
