"""This module implements the Toeplitz hash specified by Microsoft
Receive-Side Scaling and implemented by the Intel IXGBE, as well as
the RSS queue-assignment scheme used in Linux's IXGBE driver."""

__all__ = ["RSSHash", "LinuxIXGBE", "PortGenerator"]

import socket

IXGBE_MAX_RSS_INDICES = 16

def bits(bytes):
    for byte in bytes:
        assert 0 <= byte <= 255
        for bit in range(8):
            yield (byte >> (7-bit)) & 1

def ipv4Tuple(hostname):
    s = socket.gethostbyname(hostname)
    return tuple(map(int, s.split(".")))

# See 3.5.2.11.1 of IXGBE datasheet
class RSSHash(object):
    def __init__(self, key):
        self.__key = key

    def hashData(self, data):
        """Compute the RSS hash of data, which must be an array of
        integers between 0 and 255, inclusive."""

        result = 0
        nkey = self.__key
        for bit in list(bits(data)):
            if bit == 1:
                result ^= (nkey >> (320-32)) & 0xFFFFFFFF
            nkey <<= 1
        return result

    def ipv4(self, src, dst):
        """Compute the RSS hash of an IPv4 packet with the given
        source and destination addresses.  The addresses can be IP
        addresses as dotted quad strings, or host names."""

        # Both IP's are in network byte order, as observed on the wire
        data = ipv4Tuple(src) + ipv4Tuple(dst)
        return self.hashData(data)

    def ipv4TCP(self, src, sPort, dst, dPort):
        """Compute the RSS hash of an IPv4 TCP or UDP packet.  src and
        dst are as for ipv4."""

        # IP's are as for ipv4.  Ports are also in network byte order.
        data = (ipv4Tuple(src) + ipv4Tuple(dst) +
                (sPort >> 8, sPort & 0xFF, dPort >> 8, dPort & 0xFF))
        return self.hashData(data)

    # Turns out the TCP and UDP hashes are identical (*really* identical
    # since both protocols put the port numbers in the same places)
    ipv4UDP = ipv4TCP

class LinuxIXGBE(RSSHash):
    # Linux random RSS key.  See
    # drivers/net/ixgbe/ixgbe_main.c:ixgbe_configure_rx.  Because of
    # the order of byte array register in the IXGBE, each word is
    # byte-swapped from how they're written in the kernel.
    KEY = 0x3dd791e26cec05180db3942aec2b4fa57caf49ea3dad14e2beaa55b8ea673e6a174d36140d20ed3b

    def __init__(self, onlineCPUs):
        RSSHash.__init__(self, self.KEY)

        # The hardware will take the bottom 7 bits of the RSS hash as
        # an index into the RSS indirection table stored in the RETA
        # registers.  The way Linux initializes RETA looks insane, but
        # it's *almost* round-robin'ing across the available max(16,
        # numOnlineCPUs) queues.  (It has to fill them in 32 bits at a
        # time, thus the "sliding window" thing; and it fills both the
        # top and bottom 4 bits of each 8 bit entry with the same 4
        # bit value just in case you're using the bit from the VMDq
        # index to control which of the two nibbles in the RSS
        # indirection table to use, thus the 0x11).  In reality, each
        # block of four entries is actually in *reverse* order because
        # of the way the Linux driver constructs the entry.
        queues = max(IXGBE_MAX_RSS_INDICES, onlineCPUs)
        reta = (range(queues) * 128)[:128]
        for i in range(0, 128, 4):
            reta[i:i+4] = reta[i:i+4][::-1]
        self.__reta = reta

    def queueOf(self, h):
        """Given an RSS hash value of a packet, return the IXGBE queue
        that will receive the packet."""

        return self.__reta[h & 0x7F]

def testHash():
    # Example key from Microsoft RSS hash test suite
    h = RSSHash(0x6d5a56da255b0ec24167253d43a38fb0d0ca2bcbae7b30b477cb2da38030f20c6a42b73bbeac01fa)

    # Watch out!  The table in the IXGBE data sheet puts the columns
    # in the opposite order from everywhere else in the text.

    for da, dp, sa, sp, ipv4, ipv4tcp in [
        ("161.142.100.80",1766, "66.9.149.187",2794,    0x323e8fc2, 0x51ccc178),
        ("65.69.140.83",4739,   "199.92.111.2",14230,   0xd718262a, 0xc626b0ea),
        ("12.22.207.184",38024, "24.19.198.95",12898,   0xd2d0a5de, 0x5c2b394a),
        ("209.142.163.6",2217,  "38.27.205.30",48228,   0x82989176, 0xafc7327f),
        ("202.188.127.2",1303,  "153.39.163.191",44251, 0x5d1809c5, 0x10e828a2),
        ]:
        assert(h.ipv4(sa, da) == ipv4)
        assert(h.ipv4TCP(sa, sp, da, dp) == ipv4tcp)

def testQueues():
    h = LinuxIXGBE(16)
    tom = "192.168.42.11"
    josmp = "192.168.42.10"

    # (Based on Alex's hand-crafted 48 client memcached config)
    for n, src in enumerate([8011, 8008, 8013, 8001, 8000, 8009, 8012, 8058,
                             8050, 8075, 8014, 8002, 8003, 8059, 8004, 8072,

                             80107, 80104, 80109, 80119,
                             80118, 80105, 80108, 80174,
                             80156, 80159, 80154, 80150,
                             80151, 80175, 80170, 80166]):
        # Whoa, what?  80107 is not a valid port.
        src &= 0xFFFF
        assert(h.queueOf(h.ipv4UDP(tom, 11211 + n, josmp, src) == n % 16))

class PortGenerator(object):
    """Generate a set of destination port numbers based on desired RSS
    queuing behavior."""

    def __init__(self, onlineCPUs, first):
        self.__h = LinuxIXGBE(onlineCPUs)
        self.__first = first
        self.__used = set()

    def genIPv4UDP(self, src, srcPort, dst, dstQueue):
        h = self.__h
        for dp in range(self.__first, 65536):
            if dp in self.__used:
                continue
            if h.queueOf(h.ipv4UDP(src, srcPort, dst, dp)) == dstQueue:
                self.__used.add(dp)
                return dp
        raise ValueError("Failed to find port from %s:%d to queue %d of %s" %
                         (src, srcPort, dstQueue, dst))

if __name__ == "__main__":
    testHash()
    testQueues()
