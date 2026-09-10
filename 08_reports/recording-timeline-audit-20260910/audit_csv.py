import csv
import json
import sys


def quantiles(values):
    values = sorted(values)
    if not values:
        return {}
    return {"min": values[0], "p50": values[len(values)//2], "p95": values[int((len(values)-1)*.95)], "max": values[-1]}


def number(row, key):
    return int(row.get(key) or 0)


for path in sys.argv[1:]:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    result = {"path": path, "streams": {}}
    for stream in ("rgb", "depth"):
        selected = [r for r in rows if r["stream_type"] == stream and (stream != "rgb" or r.get("rgb_recorded") == "1")]
        invalid = [r for r in selected if r["clock_sync_valid"] not in ("1", "true")]
        global_times = [number(r, "global_timestamp_us") for r in selected]
        ids = [number(r, "frame_id") for r in selected]
        index = [number(r, "rgb_video_frame_index") for r in selected] if stream == "rgb" else []
        s = {"frames": len(selected), "clock_invalid": len(invalid), "global_regressions": sum(b < a for a,b in zip(global_times,global_times[1:])), "max_global_gap_us": max((b-a for a,b in zip(global_times,global_times[1:])),default=0), "source_id_missing": sum(max(0,b-a-1) for a,b in zip(ids,ids[1:])), "video_index_gaps": sum(b!=a+1 for a,b in zip(index,index[1:]))}
        for key in ("sender_capture_to_timing_bound_us", "sender_timing_bound_to_encode_start_us", "sender_encode_duration_us", "sender_packet_queued_to_receiver_us", "receiver_minus_frame_system_us", "sender_delay_us"):
            s[key] = quantiles([number(r,key) for r in selected if r.get(key)])
        for label,part in (("valid", [r for r in selected if r["clock_sync_valid"] in ("1", "true")]), ("invalid", invalid)):
            s[label+"_model_age_us"] = quantiles([number(r,"frame_system_timestamp_us")-number(r,"clock_model_reference_timestamp_us") for r in part])
        result["streams"][stream]=s
    print(json.dumps(result))
