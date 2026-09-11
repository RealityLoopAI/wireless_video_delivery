import json
import os
from pathlib import Path
import subprocess

def run(*args):
    subprocess.run(args, check=True)

pids = subprocess.check_output(['pgrep', '-x', 'gemini_sender'], text=True).split()
assert len(pids) == 1, pids
proc = Path('/proc') / pids[0]
binary = (proc / 'exe').resolve()
args = (proc / 'cmdline').read_bytes().decode().split('\0')
config = Path(args[args.index('--config') + 1])
libs = sorted(set(line.split()[-1] for line in (proc / 'maps').read_text().splitlines() if 'libOrbbecSDK.so.' in line))
assert len(libs) == 1, libs
sdk = Path(libs[0]).parent.parent
source = Path.home() / 'wvd-native-source-8569b21'
repo = binary.parents[2]
bundle = Path.home() / 'wvd-native-release.bundle'
run('git', '-C', str(repo), 'fetch', str(bundle), 'HEAD')
if not source.exists():
    run('git', '-C', str(repo), 'worktree', 'add', '--detach', str(source), '8569b21')
assert subprocess.check_output(['git', '-C', str(source), 'rev-parse', '--short=7', 'HEAD'], text=True).strip() == '8569b21'
build = Path.home() / 'wvd-native-build-8569b21'
run('cmake', '-S', str(source), '-B', str(build), '-DGWV3_BUILD_SENDER=ON', '-DGWV3_BUILD_RECEIVER=OFF', '-DORBBEC_SDK_ROOT='+str(sdk), '-DCMAKE_BUILD_TYPE=RelWithDebInfo')
run('nice', '-n', '10', 'cmake', '--build', str(build), '-j1')
run('ctest', '--test-dir', str(build/'01_sender_linux'), '--output-on-failure')
info = {'binary': str(binary), 'config': str(config), 'sdk': str(sdk), 'source': str(source), 'candidate': str(build/'bin/gemini_sender'), 'sender_id': json.loads(config.read_text())['sender_id']}
(Path.home()/'wvd-native-build-info.json').write_text(json.dumps(info, indent=2))
print(json.dumps(info), flush=True)
