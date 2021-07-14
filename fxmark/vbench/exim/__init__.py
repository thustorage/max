from mparts.manager import Task
from mparts.host import HostInfo
from support import ResultsProvider, SetCPUs, FileSystem, SystemMonitor, \
        waitForLog, PerfRecording, Sleep

import os, signal, re

__all__ = []

__all__.append("EximDaemon")
class EximDaemon(Task):
    __info__ = ["host", "eximPath", "eximBuild", "mailDir", "spoolDir", "port"]

    def __init__(self, host, eximPath, eximBuild, mailDir, spoolDir, port):
        Task.__init__(self, host = host)
        self.host = host
        self.eximPath = eximPath
        self.eximBuild = eximBuild
        self.mailDir = mailDir
        self.spoolDir = spoolDir
        self.port = port
        self.__proc = None

    def start(self):
        # fix -> http://vincent.bernat.im/en/blog/2014-tcp-time-wait-state-linux.html
        self.host.sysctl("net.ipv4.tcp_tw_recycle", "1")
        # Create configuration
        config = self.host.outDir(self.name + ".configure")
        self.host.r.run(
            [os.path.join(self.eximPath, "mkconfig"),
             os.path.join(self.eximPath, self.eximBuild),
             self.mailDir, self.spoolDir],
            stdout = config)

        # Start Exim
        self.__proc = self.host.r.run(
            [os.path.join(self.eximPath, self.eximBuild, "bin", "exim"),
             "-bdf", "-oX", str(self.port), "-C", config],
            wait = False)
        waitForLog(self.host, os.path.join(self.spoolDir, "log", "mainlog"),
                   "exim", 5, "listening for SMTP")

    def stop(self):
        # Ugh, there's no way to cleanly shut down Exim, so we can't
        # check for a sensible exit code.
        self.__proc.kill(signal.SIGTERM)
        self.host.sysctl("net.ipv4.tcp_tw_recycle", "0")

    def reset(self):
        if self.__proc:
            self.stop()

__all__.append("EximLoad")
class EximLoad(Task, ResultsProvider):
    __info__ = ["host", "trial", "eximPath", "clients", "port", "*sysmonOut"]

    # XXX Control warmup/duration
    def __init__(self, host, trial, eximPath, cores, clients, port, sysmon,
            perfRecord, resultPath, perfBin, perfProbe):
        Task.__init__(self, host = host, trial = trial)
        ResultsProvider.__init__(self, cores)
        self.host = host
        self.trial = trial
        self.eximPath = eximPath
        self.clients = clients
        self.port = port
        self.sysmon = sysmon
        self.perfRecord = perfRecord
        self.resultPath = resultPath
        self.perfBin = perfBin
        self.perfProbe = perfProbe
        self.cores = self.cores

    def wait(self):
        # We may want to wipe out old mail files, but it doesn't seem
        # to make a difference.

        cmd = [os.path.join(self.eximPath, "run-smtpbm"),
               str(self.clients), str(self.port)]
        cmd = self.sysmon.wrap(cmd, "Starting", "Stopped")

        # Run
        logPath = self.host.getLogPath(self)
        perfFile = os.path.join(os.path.abspath("."), self.resultPath,
                "perf-exim-%s.dat" % self.cores)
        with PerfRecording(self.host, perfFile,
                self.perfRecord, self.perfBin, perfProbe = self.perfProbe):
            self.host.r.run(cmd, stdout = logPath)

        # XXX Sanity check no paniclog or rejectlog, non-empty mboxes,
        # non-empty mainlog

        # Get result
        log = self.host.r.readFile(logPath)
        self.sysmonOut = self.sysmon.parseLog(log)
        ms = re.findall("(?m)^([0-9]+) messages", log)
        if len(ms) != 1:
            raise RuntimeError("Expected 1 message count in log, got %d",
                               len(ms))
        self.setResults(int(ms[0]), "message", "messages",
                        self.sysmonOut["time.real"])

class EximRunner(object):
    def __str__(self):
        return "exim"

    @staticmethod
    def run(m, cfg):
        if not cfg.hotplug:
            raise RuntimeError("The Exim benchmark requires hotplug = True.  "
                               "Either enable hotplug or disable the Exim "
                               "benchmark in config.py.")

        host = cfg.primaryHost
        m += host
        m += HostInfo(host)
        # sleep for 30 seconds, let the system gets stable
        m += Sleep(host = host, stime = 0)
        fs = FileSystem(host, cfg.fs, clean = True)
        m += fs
        eximPath = os.path.join(cfg.benchRoot, "exim")
        m += SetCPUs(host = host, num = cfg.cores)
        m += EximDaemon(host, eximPath, cfg.eximBuild,
                        os.path.join(fs.path + "0"),
                        os.path.join(fs.path + "spool"),
                        cfg.eximPort)
        sysmon = SystemMonitor(host)
        m += sysmon
        for trial in range(cfg.trials):
            # XXX It would be a pain to make clients dependent on
            # cfg.cores.
            m += EximLoad(host, trial, eximPath, cfg.cores,
                          cfg.clients, cfg.eximPort, sysmon,
                          cfg.precord, m.tasks()[0].getPath(),
                          cfg.perfBin, cfg.perfProbe)
        # m += cfg.monitors
        m.run()

__all__.append("runner")
runner = EximRunner()
