"""Read-only audit of published files for one explicit recording session."""
import csv
import json
from pathlib import Path
import subprocess
import sys

root, session, destination = Path(sys.argv[1]), int(sys.argv[2]), Path(sys.argv[3])
results = []
for marker in sorted(root.glob('*/2026-09-10/145225/*recording_ready.json')):
    ready = json.loads(marker.read_text())
    if int(ready.get('recording_session_id', 0)) != session:
        continue
    directory = marker.parent
    meta = json.loads((directory / marker.name.replace('recording_ready.json', 'meta.json')).read_text())
    with next(directory.glob('*frames.csv')).open() as f:
        rows = list(csv.DictReader(f))
    rgb = [r for r in rows if r['stream_type'] == 'rgb' and r.get('rgb_recorded') == '1']
    depth = [r for r in rows if r['stream_type'] == 'depth']
    video = next(directory.glob('*.mp4'))
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_packets', '-show_format', '-show_entries', 'packet=pts_time,dts_time:format=duration',
        '-of', 'json', str(video)], timeout=60))
    pts = [float(p['pts_time']) for p in probe['packets']]
    origin = int(meta['rgb_pts_origin_global_us'])
    expected = [(int(r['global_timestamp_us'])-origin)/1e6 for r in rgb]
    errors = [abs(a-b)*1e6 for a,b in zip(pts, expected)]
    start = int(meta.get('recording_window_start_global_us', origin))
    end = int(meta.get('recording_window_end_global_us', 0))
    duration = (end-start)/1e6
    def stats(frames):
        stamps = [int(r['global_timestamp_us']) for r in frames]
        gaps = [(b-a)/1e6 for a,b in zip(stamps, stamps[1:])]
        return dict(frames=len(frames), max_gap_seconds=max(gaps, default=0),
            gaps_over_500ms=sum(g>.5 for g in gaps), regressions=sum(g<0 for g in gaps),
            clock_invalid=sum(r.get('clock_sync_valid') not in ('1', 'true') for r in frames),
            coverage_percent=len(frames)/(30*duration)*100 if duration>0 else None)
    decodes = {}
    for media in [video, next(directory.glob('*.mkv'))]:
        p = subprocess.run(['ffmpeg','-v','error','-xerror','-threads','1','-i',str(media),
                            '-map','0:v:0','-fps_mode','passthrough','-enc_time_base','1:1000000',
                            '-f','null','-'], capture_output=True, text=True, timeout=180)
        decodes[media.name] = dict(returncode=p.returncode, stderr=p.stderr[-3000:])
    item = dict(directory=str(directory), ready=ready, window_seconds=duration,
        rgb=stats(rgb), depth=stats(depth), mp4_duration=probe['format'].get('duration'),
        pts_count=len(pts), pts_error_max_us=max(errors, default=None),
        index_contiguous=[int(r['rgb_video_frame_index']) for r in rgb]==list(range(len(rgb))),
        timestamp_mode=meta.get('rgb_timestamp_mode'), decodes=decodes,
        pts_pass=len(pts)==len(expected)>0 and max(errors)<1000,
        first_pts=pts[0] if pts else None, last_pts=pts[-1] if pts else None)
    results.append(item)
    destination.write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print(json.dumps({k:v for k,v in item.items() if k not in ('ready','decodes')}, ensure_ascii=False), flush=True)
assert len(results)==6, f'expected six published directories, got {len(results)}'
assert all(r['pts_pass'] and r['index_contiguous'] and all(d['returncode']==0 and not d['stderr'] for d in r['decodes'].values()) for r in results)
