"""Bounded, session-scoped retransmission cache for archive RTP only."""
from collections import OrderedDict
import struct
import threading
import time


class AudioPacketCache:
    def __init__(self, clock=time.monotonic, max_packets=1500, max_bytes=3*1024*1024):
        self.clock = clock
        self.max_packets = max_packets
        self.max_bytes = max_bytes
        self.lock = threading.Lock()
        self.packets = OrderedDict()
        self.size = 0
        self.instance = ''
        self.last_request = -float('inf')
        self.counters = dict(cached=0, evicted=0, requests=0, cache_misses=0,
                             retransmitted=0, send_errors=0, forwarded=0)

    def _prune(self, now):
        while self.packets:
            key, (stamp, data) = next(iter(self.packets.items()))
            if now-stamp <= 30 and len(self.packets) <= self.max_packets and self.size <= self.max_bytes:
                break
            self.packets.pop(key)
            self.size -= len(data)
            self.counters['evicted'] += 1

    def put(self, packet, instance):
        if len(packet) < 12 or len(packet) > 2048 or packet[0] >> 6 != 2:
            return
        _, _, seq, timestamp, ssrc = struct.unpack('!BBHII', packet[:12])
        now = self.clock()
        with self.lock:
            if self.instance != instance:
                self.packets.clear()
                self.size = 0
                self.instance = instance
            key = (seq, timestamp, ssrc)
            old = self.packets.pop(key, None)
            if old:
                self.size -= len(old[1])
            self.packets[key] = (now, packet)
            self.size += len(packet)
            self.counters['cached'] += 1
            self._prune(now)

    def requested(self, message):
        keys = message.get('packets')
        if not isinstance(keys, list) or not 1 <= len(keys) <= 64:
            return []
        if any(not isinstance(k, list) or len(k) != 3 or
               any(type(v) is not int for v in k) or
               not (0 <= k[0] <= 65535 and 0 <= k[1] <= 0xffffffff and 0 <= k[2] <= 0xffffffff)
               for k in keys):
            return []
        with self.lock:
            now = self.clock()
            if message.get('stream_instance_id') != self.instance or now-self.last_request < .1:
                return []
            self.last_request = now
            self.counters['requests'] += 1
            self._prune(now)
            packets = []
            for key in dict.fromkeys(tuple(k) for k in keys):
                value = self.packets.get(key)
                if value:
                    packets.append(value[1])
                else:
                    self.counters['cache_misses'] += 1
            return packets

    def count(self, key):
        with self.lock:
            self.counters[key] += 1

    def snapshot(self):
        with self.lock:
            return dict(self.counters, cache_packets=len(self.packets), cache_bytes=self.size)
