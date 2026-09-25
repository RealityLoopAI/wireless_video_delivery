import csv
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

p = Path(__file__).resolve().parent
cap = p / 'capture'
groups = ['autoA', 'level4', 'level8', 'autoB', 'level2', 'autoC']
rois = {
    'dark_bench': (5, 405, 60, 480),
    'gray_scale_front': (180, 675, 270, 720),
    'light_wall': (1140, 140, 1250, 320),
    'bottle_text': (55, 130, 150, 230),
}
report = {'rois': rois, 'groups': {}, 'method': 'Median adjacent-frame temporal RMS / sqrt(2); includes motion and illumination changes. Spatial residual includes texture. Not a calibrated sensor noise measurement.'}
canvas = Image.new('RGB', (1800, 880), '#202020')
draw = ImageDraw.Draw(canvas)
for col, tag in enumerate(groups):
    rows = list(csv.DictReader((cap / (tag + '_snapshots.csv')).open()))
    files = [cap / Path(r['path']).name for r in rows]
    result = {'frames': len(files), 'exposure_raw': sorted(set(r['exposure_raw'] for r in rows)), 'gain': sorted(set(r['gain'] for r in rows)), 'rois': {}}
    for row, (label, roi) in enumerate(rois.items()):
        tiles = [Image.open(f).convert('RGB').crop(roi) for f in files]
        stack = np.stack([np.array(im.convert('YCbCr'), dtype=np.float32) for im in tiles])
        diff = np.diff(stack, axis=0)
        temporal = np.sqrt(np.mean(diff ** 2, axis=(1, 2)) / 2)
        residual = np.stack([np.array(im.convert('YCbCr'), dtype=np.float32) - np.array(im.filter(ImageFilter.MedianFilter(3)).convert('YCbCr'), dtype=np.float32) for im in tiles])
        average = stack.mean(axis=0)[:, :, 0]
        gx = average[1:-1, 2:] - average[1:-1, :-2]
        gy = average[2:, 1:-1] - average[:-2, 1:-1]
        gradient = np.sqrt(gx * gx + gy * gy)
        result['rois'][label] = {
            'mean_y': float(stack[:, :, :, 0].mean()),
            'temporal_rms_y_cb_cr_median': np.median(temporal, axis=0).tolist(),
            'spatial_median_residual_y_cb_cr_rms': np.sqrt(np.mean(residual ** 2, axis=(0, 1, 2))).tolist(),
            'mean_frame_luma_gradient_p95': float(np.percentile(gradient, 95)),
        }
        tile = tiles[len(tiles) // 2]
        tile.thumbnail((290, 190))
        scale = min(290 / tile.width, 190 / tile.height)
        canvas.paste(tile.resize((int(tile.width * scale), int(tile.height * scale)), Image.Resampling.NEAREST), (col * 300, row * 220 + 25))
        draw.text((col * 300 + 8, row * 220 + 3), f'{tag} | {label} | gain {result["gain"]}', fill='white')
    report['groups'][tag] = result
canvas.save(p / 'comparison-crops.png')
(p / 'comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
for tag, result in report['groups'].items():
    print(tag, 'gain', result['gain'])
    for roi, v in result['rois'].items():
        print(roi, 'Y,Cb,Cr', [round(x, 3) for x in v['temporal_rms_y_cb_cr_median']], 'edge', round(v['mean_frame_luma_gradient_p95'], 3))
