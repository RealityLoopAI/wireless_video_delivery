import os
from pathlib import Path
import subprocess
import sys

pid = subprocess.check_output(['pgrep','-x','gemini_sender'],text=True).split()[0]
env = os.environ.copy()
for item in Path('/proc',pid,'environ').read_bytes().split(b'\0'):
    if b'=' in item:
        key,value=item.split(b'=',1)
        if key.decode() in ('LD_LIBRARY_PATH','GST_PLUGIN_PATH','GST_PLUGIN_PATH_1_0'):
            env[key.decode()]=value.decode()
for mode in ('bgr','jpeg','dual'):
    subprocess.run([sys.argv[1], 'mpph264enc', mode, 'timing'],check=True,env=env,timeout=20)
