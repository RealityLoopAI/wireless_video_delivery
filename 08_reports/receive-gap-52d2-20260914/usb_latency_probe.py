import collections
import json
import os
import select
import time

# Keep only transfer metadata for Wi-Fi USB device 3:004, never payload.
fd = os.open('/sys/kernel/debug/usb/usbmon/3u', os.O_RDONLY | os.O_NONBLOCK)
pending = {}
latencies = collections.defaultdict(list)
errors = collections.Counter()
slow = []
buffer = b''
start = time.monotonic()
print('start_epoch', time.time(), flush=True)
try:
    while time.monotonic() - start < 60:
        if not select.select([fd], [], [], 0.25)[0]:
            continue
        try:
            buffer += os.read(fd, 65536)
        except BlockingIOError:
            continue
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            fields = line.decode('ascii', errors='replace').split()
            if len(fields) < 6 or not fields[3].startswith(('Bo:3:004:', 'Bi:3:004:')):
                continue
            tag, stamp, event, address, status = fields[:5]
            stamp = int(stamp)
            if event == 'S':
                pending[tag] = (stamp, address)
            elif event in ('C', 'E'):
                if status != '0':
                    errors[address + ':' + status] += 1
                prior = pending.pop(tag, None)
                if prior:
                    delay = stamp - prior[0]
                    latencies[address].append(delay)
                    if delay > 100000 and len(slow) < 40:
                        slow.append([stamp, address, delay, status])
finally:
    os.close(fd)
for address, values in latencies.items():
    values.sort()
    print(address, json.dumps({'count': len(values), 'p95_us': values[int(len(values)*0.95)], 'max_us': values[-1], 'over100ms': sum(x > 100000 for x in values)}))
print('errors', dict(errors), 'slow', slow, 'pending', len(pending), 'end_epoch', time.time())
