import json
import statistics
import sys
import time
import urllib.request

def get(port=18080):
    return json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/api/status', timeout=5))

samples = []
for i in range(int(sys.argv[1])//2+1):
    samples.append((time.monotonic(), get()))
    if i < int(sys.argv[1])//2:
        time.sleep(2)
a,b = samples[0], samples[-1]
dt = b[0]-a[0]
results = []
for c in b[1]['cameras']:
    key=c['camera_key']
    rows=[next(x for x in s['cameras'] if x['camera_key']==key) for t,s in samples]
    old=rows[0]
    r=dict(camera=key,source_ip=c.get('sender_source_ip'),all_live=all(x['media_live'] for x in rows))
    for stream in ('rgb','depth'):
        r[stream+'_fps']=round((c[stream+'_packets']-old[stream+'_packets'])/dt,3)
        values=[x[stream+'_receive_delay_us']/1000 for x in rows]
        r[stream+'_delay_ms_median']=round(statistics.median(values),2)
        r[stream+'_delay_ms_max']=round(max(values),2)
    for f in ('sender_rgb_dropped_frames','sender_depth_dropped_frames','sender_rgb_send_failures',
              'sender_depth_send_failures','record_backpressure_waits','record_write_errors'):
        r[f+'_delta']=c[f]-old[f]
    r['record_queue_peak']=max(x['record_queue_packets'] for x in rows)
    results.append(r)
print(json.dumps(dict(seconds=dt,recording_state=b[1]['recording_state'],cameras=results,
                     audio=get(18083)),indent=2))
