from mparts.manager import Task
from mparts.host import HostInfo, CHECKED, UNCHECKED
from support import ResultsProvider, SetCPUs, FileSystem, SystemMonitor, \
        waitForLog, PerfRecording, Sleep

import os, signal, re

__all__ = []

__all__.append("RocksDBLoad")
class RocksDBLoad(Task, ResultsProvider):
    __info__ = ["host", "trial", "rocksdbPath", "*sysmonOut"]

    def __init__(self, host, trial, rocksdbPath, cores,
		 benchName, duration, dataSync, disableWAL,
		 compressionType, compressionLevel, clientThreads,
		 valueSize, rdwrPercent, benchDir, sysmon,
		 perfRecord, resultPath, perfBin, perfProbe):
        Task.__init__(self, host = host, trial = trial)
        ResultsProvider.__init__(self, cores)
        self.host = host
        self.trial = trial
        self.rocksdbPath = rocksdbPath
	self.benchName = benchName
	self.duration = duration
	self.dataSync = str(dataSync).lower()
	self.disableWAL = str(disableWAL).lower()
	self.compressionType = compressionType
	self.compressionLevel = compressionLevel
	self.clientThreads = clientThreads
	self.valueSize = valueSize
	self.benchDir = benchDir
	self.rdwrPercent = rdwrPercent
        self.sysmon = sysmon
        self.perfRecord = perfRecord
        self.resultPath = resultPath
        self.perfBin = perfBin
        self.perfProbe = perfProbe
        self.cores = self.cores

    def wait(self):
        cmd = [os.path.join(self.rocksdbPath, "db_bench"),
               "--benchmarks", self.benchName,
	       "--db", self.benchDir,
	       "--duration", str(self.duration),
	       "--disable_wal", self.disableWAL,
	       "--disable_data_sync", self.dataSync,
	       "--compression_type", self.compressionType,
	       "--compression_level", str(self.compressionLevel),
	       "--threads", str(self.clientThreads),
	       "--max_background_compactions", str(self.cores),
	       "--max_background_flushes", str(self.cores),
	       "--value_size", str(self.valueSize),
	       "--readwritepercent", str(self.rdwrPercent)]
	cmd = self.sysmon.wrap(cmd)

        # Run
        logPath = self.host.getLogPath(self)
        perfFile = os.path.join(os.path.abspath("."), self.resultPath,
                "perf-rocksdb-%s.dat" % self.cores)
        with PerfRecording(self.host, perfFile,
                self.perfRecord, self.perfBin, perfProbe = self.perfProbe):
            self.host.r.run(cmd, stdout = logPath,
			    wait = CHECKED)

        # Get result
        log = self.host.r.readFile(logPath)
        self.sysmonOut = self.sysmon.parseLog(log)
        ms = re.findall("\d+[\.]?\d* ops", log)
        if len(ms) != 1:
            raise RuntimeError("Expected 1 ops count in log, got %d",
                               len(ms))
        self.setResults(int(ms[0].split()[0]), "ops", "ops",
                        self.sysmonOut["time.real"])

class RocksDBRunner(object):
    def __str__(self):
        return "rocksdb"

    @staticmethod
    def run(m, cfg):
        if not cfg.hotplug:
            raise RuntimeError("The RocksDB benchmark requires hotplug = True.  "
                               "Either enable hotplug or disable the RocksDB "
                               "benchmark in config.py.")

        host = cfg.primaryHost
        m += host
        m += HostInfo(host)
        # sleep for 30 seconds, let the system gets stable
        m += Sleep(host = host, stime = 0)
        fs = FileSystem(host, cfg.fs, clean = True)
        m += fs
        rocksdbPath = os.path.join(cfg.benchRoot, "rocksdb")
        m += SetCPUs(host = host, num = cfg.cores)
        sysmon = SystemMonitor(host)
        m += sysmon
        for trial in range(cfg.trials):
            # XXX It would be a pain to make clients dependent on
            # cfg.cores.
            m += RocksDBLoad(host, trial, rocksdbPath, cfg.cores,
                          cfg.benchName, cfg.duration, cfg.dataSync,
			  cfg.disableWAL, cfg.compressionType,
			  cfg.compressionLevel, cfg.clientThreads,
			  cfg.valueSize, cfg.rdwrPercent,
			  os.path.join(fs.path, "0"),
			  sysmon, cfg.precord, m.tasks()[0].getPath(),
                          cfg.perfBin, cfg.perfProbe)
        m.run()

__all__.append("runner")
runner = RocksDBRunner()
