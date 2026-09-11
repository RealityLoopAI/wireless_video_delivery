import json
from pathlib import Path
import subprocess
import sys
import time

out=Path(sys.argv[1])
summary=json.loads((out/'analysis.json').read_text())
results=[]
for segment in summary['segments']:
    folder=Path(segment['directory'])
    assert json.loads((folder/'recording_ready.json').read_text())['ready'] is True
    meta=json.loads((folder/'meta.json').read_text())
    for stream in ('rgb','depth'):
        media=folder/meta[stream+'_file']
        begin=time.monotonic()
        cmd=['ffmpeg','-v','error','-xerror','-nostats','-progress','pipe:1','-threads','4','-i',str(media),'-map','0:v:0','-fps_mode','passthrough','-enc_time_base','1:1000000','-f','null','-']
        try:
            p=subprocess.run(cmd,text=True,capture_output=True,timeout=240)
            counts=[int(l.split('=',1)[1].strip()) for l in p.stdout.splitlines() if l.startswith('frame=')]
            r={'file':str(media),'exit_code':p.returncode,'frames':counts[-1] if counts else None,'expected_frames':segment['streams'][stream]['frames'],'stderr':p.stderr[-4000:]}
            r['passed']=p.returncode==0 and not p.stderr and r['frames']==r['expected_frames']
        except subprocess.TimeoutExpired:
            r={'file':str(media),'passed':False,'error':'decode timed out after 240 seconds'}
        r['seconds']=round(time.monotonic()-begin,3)
        results.append(r)
        (out/'decode.json').write_text(json.dumps(results,indent=2))
        print(json.dumps(r),flush=True)
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
