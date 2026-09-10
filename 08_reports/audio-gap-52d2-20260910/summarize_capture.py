import json,re
from pathlib import Path
root=Path(__file__).parent
def read(name, interface):
    packets=[]
    for line in (root/name).read_text().splitlines():
        if interface not in line:
            continue
        m=re.search(r'c111\s+(\d+)\s+(\d+)$',line)
        if m:
            packets.append((float(line.split()[0]),int(m[1]),int(m[2])))
    return packets
local=read('sender-rtp.txt',' lo ')
out=read('sender-rtp.txt',' wlan0 ')
incoming=read('receiver-rtp.txt',' ens3 ')
assert local and out and incoming
lo=max(x[0][0] for x in (local,out,incoming))+1
hi=min(x[-1][0] for x in (local,out,incoming))-1
assert hi>lo
source={p[1:] for p in local if lo<=p[0]<=hi}
sent={p[1:] for p in out}
received={p[1:] for p in incoming}
result=dict(window_seconds=hi-lo,local_packets_in_inner_window=len(source),
    local_not_at_wlan=len(source-sent),wlan_not_at_receiver=len((source&sent)-received),
    counts=dict(local=len(local),wlan=len(out),receiver=len(incoming)))
print(json.dumps(result,indent=2))
