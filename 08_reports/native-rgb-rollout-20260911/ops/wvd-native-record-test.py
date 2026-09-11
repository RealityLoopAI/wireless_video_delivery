import argparse
import datetime
import json
from pathlib import Path
import time
import urllib.request
import signal

p=argparse.ArgumentParser()
p.add_argument('--seconds',type=int,default=960)
a=p.parse_args()
def interrupted(signum, frame):
    raise SystemExit('test controller interrupted by signal '+str(signum))
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
out=Path.home()/('wvd-native-record-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
out.mkdir()
def api(path,post=False):
    req=urllib.request.Request('http://127.0.0.1:18080/api/'+path,method='POST' if post else 'GET')
    with urllib.request.urlopen(req,timeout=15) as r:return json.load(r)
def save(name,d): (out/name).write_text(json.dumps(d,indent=2))
s=api('status');save('before.json',s)
assert s['recording_state']=='idle' and all(not c['recording'] for c in s['cameras'])
cams=[c for c in s['cameras'] if c['live']]
assert len(cams)==6 and all(c.get('sender_build_commit')=='8569b2122b46' for c in cams)
start=api('record/start-all',True);save('start.json',start)
assert start['ok'],start
session=start['recording_session_id']
print(json.dumps({'output':str(out),'start':start}),flush=True)
began=time.monotonic()
try:
    with (out/'samples.jsonl').open('w') as f:
        while time.monotonic()-began<a.seconds:
            time.sleep(min(30,max(0,a.seconds-(time.monotonic()-began))))
            s=api('status')
            f.write(json.dumps({'elapsed':time.monotonic()-began,'status':s})+'\n');f.flush()
            assert s.get('recording_session_id')==session and s.get('recording_all'), 'recording session changed externally'
            print(json.dumps({'elapsed':round(time.monotonic()-began),'cameras':[{k:c.get(k) for k in ('camera_key','live','sender_rgb_input_fps','sender_rgb_dropped_frames','sender_depth_dropped_frames','rgb_receive_delay_us','depth_receive_delay_us','record_write_errors','clock_sync_valid')} for c in s['cameras']]}),flush=True)
finally:
    s=api('status')
    if s.get('recording_session_id')==session and s.get('recording_all'):
        stop_mono=time.monotonic()
        stop=api('record/stop-all',True);save('stop.json',stop)
        print(json.dumps({'stop':stop}),flush=True)
        for _ in range(60):
            time.sleep(3)
            s=api('status');save('after.json',s)
            u=s.get('recording_uploader',{})
            if s['recording_state']=='idle' and not any(c.get('record_tail_draining') or c.get('segment_finalize_pending') or c.get('segment_finalize_active') for c in s['cameras']) and not u.get('local_pending_segments') and not u.get('nas_finalize_pending_segments') and not u.get('active_capture_workers') and not u.get('active_full_copy_workers'):
                print(json.dumps({'drained_after_seconds':time.monotonic()-stop_mono,'output':str(out)}),flush=True)
                break
