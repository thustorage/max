import sys, os, time, errno, signal

from mparts.manager import Task
from mparts.host import *

__all__ = []

__all__.append("ResultsProvider")
class ResultsProvider(object):
    """A ResultsProvider is a marker interface searched for by the
    MOSBENCH analysis tools.  Every benchmark trial should produce one
    ResultsProvider object.  This records the number of cores, the
    number of units of work done, what those units are, and how long
    it took to do that work.  In addition, if the benchmark is using
    SystemMonitor, the analysis tools expect to find its logs results
    in the ResultsProvider subclass."""

    __info__ = ["cores", "result", "unit", "units", "real"]

    def __init__(self, cores):
        self.cores = cores

    def setResults(self, result, unit, units, real):
        """Set the results of this object.  result is the number of
        work units completed.  For job-oriented benchmarks, this
        should generally be 1.  unit and units are the name of a unit
        of work, in singular and plural form.  real is the number of
        seconds of real time elapsed while performing this work."""

        self.log("=> %g %s (%g secs, %g %s/sec/core)" %
                 (result, units, real, float(result)/real/self.cores, units))
        self.result = float(result)
        self.unit = unit
        self.units = units
        self.real = real

__all__.append("IXGBE")
class IXGBE(Task, SourceFileProvider):
    __info__ = ["host", "iface", "queues", "flowDirector"]

    # Flow-director policies
    fdOptions = {
        # Pin flows by monitoring every 20th outgoing packet.  This is
        # the policy implemented by the standard IXGBE driver.
        "pin-every-20" : 0,
        # Route traffic for a set of ports to the right core
        # (used by memcached)
        "fixed-port-routing" : 1,
        # Spread incoming flows among all RX queues (used by Apache)
        "spread-incoming" : 2}

    def __init__(self, host, iface, queues, flowDirector = "pin-every-20"):
        Task.__init__(self, host = host, iface = iface)
        self.host = host
        self.iface = iface
        self.queues = queues
        self.flowDirector = flowDirector
        if flowDirector not in IXGBE.fdOptions:
            raise ValueError("Unknown flowDirector policy %r" % flowDirector)

        self.__script = self.queueSrcFile(host, "ixgbe-set-affinity")

    def start(self):
        # We re-insert the module every time not just so we can change
        # the flow-director policy, but because the driver uses the
        # number of online cores at initialization time to set the
        # number of queues in the card, which results in better
        # balancing than any IRQ direction we can do.
        for l in self.host.r.readFile("/proc/modules").splitlines():
            modname = l.split()[0]
            if modname.startswith("ixgbe"):
                self.log("rmmod %s" % modname)
                self.host.sudo.run(["rmmod", modname])
                break
        args = ["modprobe", "ixgbe"]
        if self.flowDirector != "pin-every-20":
            args.append("FDirPolicy=%d" % IXGBE.fdOptions[self.flowDirector])
        self.log(" ".join(args))
        self.host.sudo.run(args)

        self.log("Setting %s queue affinity to %s" % (self.iface, self.queues))
        self.host.sudo.run([self.__script, self.iface, self.queues],
                           stdout = self.host.getLogPath(self))

    def stop(self):
        self.reset()

    def reset(self):
        self.host.sudo.run([self.__script, self.iface, "CPUS"],
                           stdout = self.host.getLogPath(self))
        self.log("Reset IXGBE IRQ masks to all CPUs")

__all__.append("QemuVMDeamon")
class QemuVMDeamon(Task, SourceFileProvider):
    __info__ = ["host", "cores", "sockets", "threads", "qmpPort",
            "sshPort"]

    def __init__(self, host, benchRoot, driveFile, cores,
		 threads, sockets, qmpPort, sshPort, vncPort):
	Task.__init__(self, host = host)
	self.host = host
	self.benchRoot = benchRoot
	self.driveFile = driveFile
	self.cores = cores
	self.threads = threads
	self.sockets = sockets
	self.qmpPort = qmpPort
	self.sshPort = sshPort
	self.vncPort = vncPort
	self.proc = None
        self.seq = "onlinecpu"

        self.__cpuSeq = self.queueSrcFile(host, "cpu-sequences")

    def startVM(self):
        p = self.host.r.run(["uname", "-r"], stdout = CAPTURE)
	kversion = p.stdoutReadline().strip("\n")
	self.__proc = self.host.sudo.run(
            ["%s/../src/qemu/x86_64-softmmu/qemu-system-x86_64" % (self.benchRoot),
	    "--enable-kvm", "-m", "32G", "-device",
	    "virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6",
	    "-drive",
	    "file=%s/vm-scripts/%s,cache=none,if=none,id=drive-virtio-disk0,format=raw" % (self.benchRoot, self.driveFile),
	    "-device",
	    "virtio-blk-pci,scsi=off,bus=pci.0,addr=0x7,drive=drive-virtio-disk0,id=virtio-disk0",
	    "-netdev", "user,id=hostnet0,hostfwd=tcp::%s-:22" % (self.sshPort),
	    "-device", "virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3",
	    "-vnc", ":%s" % (self.vncPort),
	    "-cpu", "host",
	    "-kernel", "/boot/vmlinuz-%s" % (kversion),
	    "--initrd", "/boot/initrd.img-%s" % (kversion),
	    "-append", "root=/dev/vda1",
	    "-qmp", "tcp:localhost:%s,server,nowait" % (self.qmpPort),
	    "-smp", "cores=%s,threads=%s,sockets=%s" % (self.cores, self.threads, self.sockets),
	    "-realtime", "mlock=on"], wait = False)

    def stopVM(self):
	import time
	# lets send poweroff command to the VM
	self.host.r.run(["ssh", "root@localhost", "-p", str(self.sshPort),
	    "poweroff"], wait = None)
	# wait for 5 seconds, in case if the VM does not respond, kill it.
	time.sleep(5)
	if self.__proc:
	    self.__proc.kill(signal.SIGTERM)

    def __getOnlineSeq(self):
        p = self.host.r.run([self.__cpuSeq], stdout = CAPTURE)
        seqs = {}
        for l in p.stdoutRead().splitlines():
            name, cpus = l.strip().split(" ", 1)
	    seqs[name] = map(int, cpus.split(","))

        return seqs[self.seq]

    def pinVCPUs(self):
	from mparts import qmp
	query = qmp.QMPQuery("localhost:%s" % (self.qmpPort))
	response = query.cmd("query-cpus")['return']

	onlineCPUs = self.__getOnlineSeq()

	for i in range(len(response)):
	     self.host.sudo.run([self.__cpuSeq,
				 str(response[i]['thread_id']),
				 str(onlineCPUs[i])])

__all__.append("PerfRecording")
class PerfRecording(Task):
    def __init__(self, host, perfFile, perfRecord, perfBin,
            perfKVMRec = False, guestOnlyRec = False, perfProbe = False):
	Task.__init__(self, host = host)
	self.host = host
	self.perfFile = perfFile
	self.perfBin = perfBin
	self.perfRecord = perfRecord
	self.__perfProc = None
        self.perfKVMRec = perfKVMRec
	self.guestOnlyRec = guestOnlyRec
        self.perfProbe = perfProbe
        self.probeEvents = []
        self.probeFile = os.path.join(os.path.abspath(
            os.path.dirname(__file__)), "perf-probe.conf")

    def __enter__(self):
	if self.perfRecord is True:
            from mparts.util import maybeMakedirs
            resultDir = self.perfFile[:self.perfFile.rfind("/"):]
            maybeMakedirs(resultDir)
            print >> sys.stderr, "perf record begins..."
            if self.perfKVMRec is False and self.guestOnlyRec is False:
                perfArg = [self.perfBin, "record", "-F", "100",
                        "-a", "--call-graph", "dwarf", "-o", self.perfFile]
                if self.perfProbe is True:
                    self._addProbeEvents()
                    for i in self.probeEvents:
                        perfArg.append("-e")
                        perfArg.append("probe:" + i)
                self.__perfProc = self.host.sudo.run(perfArg, wait=False)
	    elif self.perfKVMRec is True and self.guestOnlyRec is False:
                # need to scp both guest.modules and guest.kallsyms
                guestSyms = "guest.kallsyms"
                guestModules = "guest.modules"
                def getFileFromGuest(fileName, dst):
                    self.host.r.run(
                        ["ssh", "-p", str(SSHPORT), "root@localhost",
                        "cat /proc/%s" % fileName, ">", dst])
                    self.host.r.run(
                        ["scp", "-P", str(SSHPORT),
                        "root@localhost:~/%s" % dst, resultDir])

                getFileFromGuest("kallsyms", guestSyms)
                getFileFromGuest("modules", guestModules)
                self.__perfProc = self.host.sudo.run([self.perfBin,
                    "kvm", "--host", "--guest",
                    "--guestkallsyms=%s" %
                        (os.path.join(resultDir, guestSyms)),
                    "--guestmodules=%s" %
                        (os.path.join(resultDir, guestModules)),
                    "record", "-ag", "-F", "100",
                    "-o", self.perfFile], wait=False)
	    elif self.perfKVMRec is False and self.guestOnlyRec is True:
		print "yet to implement"
	    else:
		print "you can't have both apple and orange"

    def __exit__(self, typ, value, traceback):
	if self.perfRecord is True:
	    import signal, time
	    if self.guestOnlyRec is False:
		self.__perfProc.kill(signal.SIGINT)
		time.sleep(2)
		self.host.sudo.run(["chmod", "755", self.perfFile])
                if self.perfProbe is True:
                    self._removeProbeEvents()
	    print >> sys.stderr, "perf record ends..."

    def _addProbeEvents(self):
        a = open(self.probeFile).read().splitlines()
        for i in a:
            if i[-1:] is '1':
                self.probeEvents.append(i[:-2])
                self.__modifyTracePoints(i[:-2], "--add")

    def _removeProbeEvents(self):
        import time
        time.sleep(20)
        for i in self.probeEvents:
            self.__modifyTracePoints(i, "--del")

    def __modifyTracePoints(self, event, arg):
        self.host.sudo.run([self.perfBin,
            "probe", arg, event])

__all__.append("KVMTrace")
class KVMTrace(Task):
    import time
    def __init__(self, host, traceFile, kvmTrace):
	Task.__init__(self, host = host)
	self.host = host
	self.traceFile = traceFile 
	self.kvmTrace = kvmTrace
	self.__traceProc = None

    def __enter__(self):
	if self.kvmTrace is True:
            from mparts.util import maybeMakedirs
            resultDir = self.traceFile[:self.traceFile.rfind("/"):]
            maybeMakedirs(resultDir)
            print >> sys.stderr, "kvm trace begins..."
	    traceCmd = ["trace-cmd", "record", "-b", "200000", "-e", "kvm",
			"-o", self.traceFile]
            self.__traceProc = self.host.sudo.run(traceCmd, wait=False)
	    time.sleep(2)

    def __exit__(self, typ, value, traceback):
	if self.kvmTrace is True:
	    import signal
	    self.__traceProc.kill(signal.SIGINT)
	    time.sleep(10)
	    print >> sys.stderr, "kvm trace ends..."


__all__.append("LockStats")
class LockStats(Task, SourceFileProvider):
    __info__ = ["host"]

    def __init__(self, host, sshPort):
	Task.__init__(self, host = host)
	self.host = host
	self.sshPort = sshPort

    def allowCollectLockStatsVM(self, value):
	self.host.r.run(["ssh", "root@localhost", "-p",
		str(self.sshPort), "echo", str(value),
		">", "/proc/sys/kernel/lock_stat"])

    def allowCollectLockStatsHost(self, value):
	self.host.sudo.run(["echo", str(value),
		">", "/proc/sys/kernel/lock_stat"])

    def cleanLockStats(self):
	allowCollectLockStatsHost(0)
	allowCollectLockStatsVM(0)

    def dumpLockStats(self, targetFile):
	p = self.host.sudo.run(["cat", "/proc/lock_stat"], stdout = CAPTURE)
        # directory will always exist!
        targetDir = os.path.dirname(targetFile)
        try:
            os.makedirs(targetDir)
        except EnvironmentError, e:
            if e.errno != errno.EEXIST:
                raise
        file(targetFile, "w").write(str(p))


CPU_CACHE = {}

__all__.append("SetCPUs")
class SetCPUs(Task, SourceFileProvider):
    __info__ = ["host", "num", "hotplug", "seq"]

    def __init__(self, host, num, hotplug = True, seq = "seq"):
        Task.__init__(self, host = host)
        self.host = host
        self.num = num
        self.hotplug = hotplug
        self.seq = seq

        self.__script = self.queueSrcFile(host, "set-cpus")
        self.__cpuSeq = self.queueSrcFile(host, "cpu-sequences")

    def getSeq(self):
        if self.host not in CPU_CACHE:
            raise ValueError(
                "Cannot get CPU sequences before SetCPUs has started")
        return CPU_CACHE[self.host][1][self.seq][:self.num]

    def getSeqStr(self):
        return ",".join(map(str, self.getSeq()))

    def start(self):
        # oprofile has a habit of panicking if you hot plug CPU's
        # under it
        if self.hotplug:
            try:
                self.host.sudo.run(["opcontrol", "--deinit"],
                                   wait = UNCHECKED)
            except OSError, e:
                if e.errno != errno.ENOENT:
                    raise

        if self.host not in CPU_CACHE:
            # Make sure all CPU's are online.  If we're not allowed to
            # hot plug, we'll just have to assume this is true.
            if self.hotplug:
                self.host.sudo.run([self.__script, "-i"], stdin = DISCARD)

            # Get CPU sequences
            p = self.host.r.run([self.__cpuSeq], stdout = CAPTURE)
            seqs = {}
            for l in p.stdoutRead().splitlines():
                name, cpus = l.strip().split(" ", 1)
                seqs[name] = map(int, cpus.split(","))

            # Start an interactive set-cpus.  We don't actually use
            # this, but when we disconnect from the host, the EOF to
            # this will cause it to re-enable all CPUs.  This way we
            # don't have to online all of the CPU's between each
            # experiment.  We'll do our best to online all of the
            # CPU's at the end before exiting, but even if we die a
            # horrible death, hopefully this will online everything.
            if self.hotplug:
                recover = self.host.sudo.run([self.__script, "-i"],
                                             stdin = CAPTURE, wait = None)
            else:
                recover = None

            CPU_CACHE[self.host] = (recover, seqs)
        else:
            seqs = CPU_CACHE[self.host][1]

        try:
            seq = seqs[self.seq]
        except KeyError:
            raise ValueError("Unknown CPU sequence %r" % self.seq)
        if len(seq) < self.num:
            raise ValueError("Requested %d cores, but only %d are available" %
                             (self.num, len(seq)))

        if self.hotplug:
            cmd = [self.__script, ",".join(map(str, seq[:self.num]))]
            self.host.sudo.run(cmd, wait = CHECKED)

    def reset(self):
        # Synchronously re-enable all CPU's
        if self.host not in CPU_CACHE:
            return

        sc = CPU_CACHE[self.host][0]
        if sc:
            sc.stdinClose()
            sc.wait()
            del CPU_CACHE[self.host]

PREFETCH_CACHE = set()

# this one is for logging into the VM for prefetching
SSHPORT = 5555

__all__.append("PrefetchList")
class PrefetchList(Task, SourceFileProvider):
    __info__ = ["host", "filesPath"]

    def __init__(self, host, filesPath, reuse = False,
		 virt = False, multiVM = False):
        Task.__init__(self, host = host, filesPath = filesPath)
        self.host = host
        self.filesPath = filesPath
        self.reuse = reuse
	self.virt = virt
	self.multiVM = multiVM
	self.sshPort = SSHPORT

        self.__script = self.queueSrcFile(host, "prefetch")

    def start(self):
        if self.reuse:
            # XXX Is this actually useful?  Does it actually take any
            # time to re-fetch something that's already been
            # prefetched?
            if (self.host, self.filesPath) in PREFETCH_CACHE:
                return
            PREFETCH_CACHE.add((self.host, self.filesPath))

	if self.virt is False:
	     self.host.r.run([self.__script, "-l"],
		stdin = self.filesPath)
	else:
	     def __prefetchFiles(sshPort = SSHPORT):
	          self.host.r.run(["ssh", "root@localhost", "-p",
			  str(self.sshPort), "./prefetch", "-l",
			  '<', self.filesPath])
	     __prefetchFiles()
	     if self.multiVM is True:
	          __prefetchFiles(self.sshPort + 1)

__all__.append("PrefetchDir")
class PrefetchDir(Task, SourceFileProvider):
    __info__ = ["host", "topDir", "excludes"]

    def __init__(self, host, topDir, excludes = []):
        Task.__init__(self, host = host, topDir = topDir)
        self.host = host
        self.topDir = topDir
        self.excludes = excludes

        self.__script = self.queueSrcFile(host, "prefetch")

    def start(self):
        cmd = [self.__script, "-r"]
        for x in self.excludes:
            cmd.extend(["-x", x])
        self.host.r.run(cmd + [self.topDir])

__all__.append("CopyDir")
class CopyDir(Task):
    __info__ = ["host"]

    def __init__(self, host, srcPath, dstPath):
	Task.__init__(self, host = host)
	self.host = host
	self.srcPath = srcPath
	self.dstPath = dstPath

    def start(self):
	cmd = ["cp", "-r", self.srcPath, self.dstPath]
	self.host.sudo.run(cmd)
	self.host.sudo.run(["sudo", "chmod", "755",
		self.dstPath + "/", "-R"])

__all__.append("Sleep")
class Sleep(Task):
    def __init__(self, host, stime):
	Task.__init__(self, host = host)
	self.host = host
	self.stime = stime

    def start(self):
	import time
	time.sleep(self.stime)

__all__.append("FileSystem")
class FileSystem(Task, SourceFileProvider):
    __info__ = ["host", "fstype"]

    def __init__(self, host, fstype,
            basepath = "/tmp/mosbench", clean = True):
        Task.__init__(self, host = host, fstype = fstype)
        self.host = host
        self.fstype = fstype
        self.__clean = clean
        assert '/' not in fstype
        self.basepath = basepath
        self.path = "%s/%s/" % (basepath, fstype)
        self.__script = self.queueSrcFile(host, "cleanfs")

    def start(self):
        # Check that the file system exists.  We check the mount table
        # instead of just the directory so we don't get tripped up by
        # stale mount point directories.
        if self.basepath is "":
            mountCheck = self.path.rstrip("/")
            for l in self.host.r.readFile("/proc/self/mounts").splitlines():
                if l.split()[1].startswith(mountCheck):
                    break
            else:
                raise ValueError(
                    "No file system mounted at %s.  Did you run 'mkmounts %s' on %s?" %
                    (mountCheck, self.fstype, self.host))

        # Clean
        if self.__clean:
            self.clean()

    def clean(self):
        self.host.r.run([self.__script, self.fstype])

__all__.append("waitForLog")
def waitForLog(host, logPath, name, secs, string):
    for retry in range(secs*2):
        try:
            log = host.r.readFile(logPath)
        except EnvironmentError, e:
            if e.errno != errno.ENOENT:
                raise
        else:
            if string in log:
                return
        time.sleep(0.5)
    raise RuntimeError("Timeout waiting for %s to start" % name)

# XXX Perhaps this shouldn't be a task at all.  It has to send a
# source file, but doesn't have any life-cycle.
__all__.append("SystemMonitor")
class SystemMonitor(Task, SourceFileProvider):
    __info__ = ["host"]

    def __init__(self, host):
        Task.__init__(self, host = host)
        self.host = host
        self.__script = self.queueSrcFile(host, "sysmon")

    def wrap(self, cmd, start = None, end = None):
        out = [self.__script]
        if start != None:
            out.extend(["-s", start])
        if end != None:
            out.extend(["-e", end])
        out.extend(cmd)
        return out

    def parseLog(self, log):
        """Parse a log produced by a sysmon-wrapped command, returning
        a dictionary of configuration values that should be
        incorporated into the calling object's configuration."""

        mine = [l for l in log.splitlines() if l.startswith("[TimeMonitor] ")]
        if len(mine) == 0:
            raise ValueError("No sysmon report found in log file")
        if len(mine) > 1:
            raise ValueError("Multiple sysmon reports found in log file")

        parts = mine[0].split()[1:]
        res = {}
        while parts:
            k, v = parts.pop(0), parts.pop(0)
            res["time." + k] = float(v)
        return res

__all__.append("ExplicitSystemMonitor")
class ExplicitSystemMonitor(SystemMonitor):
    def __init__(self, *args, **kwargs):
        SystemMonitor.__init__(self, *args, **kwargs)
        self.__p = None
        self.__gen = 0

    def start(self):
        assert self.__p == None
        cmd = self.wrap([], start = "start", end = "end")
        self.__logPath = self.host.getLogPath(self) + ".%d" % self.__gen
        self.__gen += 1
        self.__p = self.host.r.run(cmd, stdin = CAPTURE,
                                   stdout = self.__logPath,
                                   wait = False)

    def __term(self, check):
        if self.__p:
            self.__p.stdinClose()
            self.__p.wait(check)
            self.__p = None

    def stop(self):
        self.__term(True)

    def reset(self):
        self.__term(False)

    def startMonitor(self):
        self.__p.stdinWrite("start\n")

    def stopMonitor(self):
        self.__p.stdinWrite("end\n")
        # Force results out
        self.__term(True)
        # Get the results
        log = self.host.r.readFile(self.__logPath)
        # Start a new monitor, for multiple trials
        self.start()
        return self.parseLog(log)

__all__.append("perfLocked")
def perfLocked(host, cmdSsh, cmdSudo, cmdRun):
    """This is a host command modifier that takes a performance lock
    using 'perflock' while the remote RPC server is running."""

    if cmdSudo:
        # Hosts always make a regular connection before a root
        # connection and we wouldn't want to deadlock with ourselves.
        return cmdSsh + cmdSudo + cmdRun
    # XXX Shared lock option
    return cmdSsh + ["perflock"] + cmdSudo + cmdRun
