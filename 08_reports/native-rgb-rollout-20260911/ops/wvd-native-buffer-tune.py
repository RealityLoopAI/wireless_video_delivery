import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

def status():
    with urllib.request.urlopen('http://192.168.1.196:8080/api/status', timeout=8) as r:return json.load(r)
s=status()
assert s['recording_state']=='idle' and all(not c['recording'] and not c.get('record_tail_draining') for c in s['cameras'])
pid=subprocess.check_output(['pgrep','-x','gemini_sender'],text=True).split()
assert len(pid)==1
proc=Path('/proc',pid[0]);binary=(proc/'exe').resolve();args=(proc/'cmdline').read_bytes().decode().split('\0');path=Path(args[args.index('--config')+1])
c=json.loads(path.read_text());assert c['sender_id']=='orangepi5pro-fe0f7222'
old=Path('/var/lib/gwv3/native-rollout-8569b21/sender-before-buffer-1800.json');assert not old.exists()
assert c['recording_buffer']['enabled'] and c['recording_buffer']['rgb_frames_per_slot']==900
shutil.copy2(path,old)
c['recording_buffer']['rgb_frames_per_slot']=1800
tmp=path.with_name(path.name+'.buffer1800');tmp.write_text(json.dumps(c,indent=2)+'\n');shutil.copystat(path,tmp);st=path.stat();os.chown(tmp,st.st_uid,st.st_gid)
env=os.environ.copy()
for item in (proc/'environ').read_bytes().split(b'\0'):
    if b'=' in item:
        k,v=item.split(b'=',1)
        if k.decode() in ('LD_LIBRARY_PATH','GST_PLUGIN_PATH','GST_PLUGIN_PATH_1_0'):env[k.decode()]=v.decode()
subprocess.run([str(binary),'--config',str(tmp),'--validate-config'],check=True,env=env,timeout=20)
unit='gwv3-gemini-sender.service'
try:
    subprocess.run(['systemctl','stop',unit],check=True,timeout=40)
    os.replace(tmp,path)
    subprocess.run(['systemctl','start',unit],check=True,timeout=40)
    good=0
    for _ in range(30):
        time.sleep(3)
        cams=[x for x in status()['cameras'] if x['sender_id']==c['sender_id']]
        ok=cams and all(x['live'] and x['sender_rgb_input_fps']>25 and 0<=x['rgb_receive_age_ms']<3000 for x in cams)
        good=good+1 if ok else 0
        if good>=3:break
    else:raise RuntimeError('buffer tune live acceptance failed')
except BaseException:
    shutil.copy2(old,tmp);os.chown(tmp,st.st_uid,st.st_gid);os.replace(tmp,path)
    subprocess.run(['systemctl','restart',unit],check=True,timeout=40)
    raise
print('RGB queue capacity 900 -> 1800; other configuration retained',flush=True)
