import csv
import datetime
import json
from pathlib import Path
import subprocess
import sys

out=Path(sys.argv[1])
session=json.loads((out/'start.json').read_text())['recording_session_id']
root=Path.home()/'Desktop/nas'
results=[]
for path in root.glob('*/'+datetime.datetime.now().strftime('%Y-%m-%d')+'/*/meta.json'):
    meta=json.loads(path.read_text())
    if meta.get('recording_session_id')!=session:continue
    folder=path.parent
    ready=folder/'recording_ready.json'
    item={'directory':str(folder),'camera_key':meta['camera_key'],'ready':ready.exists() and json.loads(ready.read_text()).get('ready'), 'quality':meta.get('recording_quality_status'),'reason':meta.get('recording_quality_reason'), 'streams':{}}
    rows=list(csv.DictReader((folder/meta.get('frames_file','frames.csv')).open()))
    for stream in ('rgb','depth'):
        selected=[r for r in rows if r['stream_type']==stream]
        ts=[int(r['global_timestamp_us']) for r in selected]
        gaps=[b-a for a,b in zip(ts,ts[1:])]
        info={'frames':len(selected),'first_global_us':ts[0] if ts else None,'last_global_us':ts[-1] if ts else None,'max_gap_us':max(gaps,default=0),'gaps_over_50ms':sum(g>50000 for g in gaps),'gaps_over_500ms':sum(g>500000 for g in gaps),'nonincreasing':sum(g<=0 for g in gaps),'invalid_clock':sum(r['clock_sync_valid']!='1' for r in selected),'pair_ids_zero':sum(r['pair_id']=='0' for r in selected)}
        info['outside_segment_window'] = sum(
            not (int(r['segment_window_start_global_us']) <= int(r['global_timestamp_us'])
                 < int(r['segment_window_end_global_us']))
            for r in selected if r.get('segment_window_valid') == '1')
        if stream=='rgb': info['contiguous_video_indices']=all(int(r['rgb_video_frame_index'])==i for i,r in enumerate(selected))
        media=folder/meta[stream+'_file']
        data=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0','-show_packets','-show_entries','packet=pts_time,dts_time','-of','json',str(media)],timeout=90))
        packets=data['packets'];pts=[float(p['pts_time']) for p in packets if 'pts_time' in p]
        info.update(media_packets=len(packets),pts_nonincreasing=sum(b<=a for a,b in zip(pts,pts[1:])),media_first_pts=pts[0] if pts else None,media_last_pts=pts[-1] if pts else None)
        item['streams'][stream]=info
    results.append(item)
    print(json.dumps(item),flush=True)
summary={'session':session,'segments':results,'boundaries':[]}
for key in sorted(set(r['camera_key'] for r in results)):
    group=sorted([r for r in results if r['camera_key']==key],key=lambda r:r['streams']['rgb']['first_global_us'] or 0)
    for before,after in zip(group,group[1:]):
        summary['boundaries'].append({'camera_key':key,**{s+'_gap_us':after['streams'][s]['first_global_us']-before['streams'][s]['last_global_us'] for s in ('rgb','depth')}})
(out/'analysis.json').write_text(json.dumps(summary,indent=2))
print(json.dumps({'segments':len(results),'boundaries':summary['boundaries']}),flush=True)
