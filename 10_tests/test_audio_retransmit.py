#!/usr/bin/env python3
"""Deterministic holes/wrap tests plus real UDP archive recovery, without audio hardware."""
import ast
import importlib.util
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
VOICE = ROOT / '12_apps/xiaohuan_voice_photo'
sys.path.insert(0, str(VOICE))
from audio_packet_cache import AudioPacketCache
from audio_capture_recovery import CaptureRebuildGuard
from test_audio_archive_receiver import load_module, free_port

audio = load_module()


def unit():
    grid = audio.RtpSlotMapper(20000, 960)
    def mapped(i, jitter=0, epoch='one'):
        p = audio.parse_rtp(audio.make_rtp(i & 65535, (0xffff0000+i*960)&0xffffffff,
                                         9, 111, audio.OPUS_SILENCE_20_MS), 0)
        p.global_us = 20000000 + i*20000 + jitter
        return grid.assign(p, epoch)
    # Independent rounding alternates between a skipped and occupied slot.
    for i in range(200):
        assert mapped(i, 11000 if i % 2 else 9000) == 1000+i
    assert mapped(199) is None  # Actual duplicate, not a grid collision.
    assert mapped(203, 9000) == 1203  # Real missing packets stay missing.
    assert mapped(201, 9000) == 1201  # Retransmission/reordering remains placeable.
    assert mapped(204, 200000) == 1214  # Do not hide a real clock discontinuity.
    assert grid.rebases == 1
    assert mapped(0, 0, 'two') == 1000  # Restart with the same SSRC.
    # Long-term clock drift is bounded, never silently pinned to the first anchor.
    grid = audio.RtpSlotMapper(20000, 960)
    for i in range(10000):
        slot = mapped(i, i*10)
        assert abs(slot*20000-(20000000+i*20000+i*10)) <= 20000
    assert grid.rebases > 0 and len(grid.seen) <= 1500
    receiver_config=json.loads((ROOT/'06_configs/receiver_loop.json').read_text())
    audio_config=json.loads((ROOT/'06_configs/audio_archive_receiver.json').read_text())
    wait=receiver_config['task_audio']['finalize_wait_ms']
    for stream in audio_config['streams']:
        if stream.get('repair_enabled'):
            assert stream['repair_wait_ms']+2000 <= wait <= 10000
    now = [1.0]
    cache = AudioPacketCache(clock=lambda: now[0], max_packets=200)
    repair = audio.RtpGapRepair(clock=lambda: now[0])
    packets = [audio.make_rtp((65500+i)&65535, (0xffff0000+i*960)&0xffffffff,
                             9,111,audio.OPUS_SILENCE_20_MS) for i in range(180)]
    for i,p in enumerate(packets):
        cache.put(p,'one')
        if not 10 <= i < 130:
            repair.observe(audio.parse_rtp(p,0),'one',960)
    assert repair.snapshot()['detected']==120
    request=None
    for attempt in range(10):
        now[0]+=.21
        request=repair.request('s')
        if attempt == 0:
            continue  # The first NACK is lost; later requests must recover it.
        if request:
            request=json.loads(json.dumps(request))
            for p in cache.requested(request):
                repair.observe(audio.parse_rtp(p,0),'one',960)
    assert repair.snapshot()['recovered']==120 and not repair.snapshot()['pending']
    assert cache.requested(dict(request or {},stream_instance_id='wrong'))==[]
    assert cache.requested(dict(stream_instance_id='one',packets=[['invalid']]))==[]
    now[0]+=31
    cache.requested(dict(stream_instance_id='one',packets=[[65500,0xffff0000,9]]))
    assert cache.snapshot()['cache_packets']==0
    cache.put(packets[0],'two')
    assert cache.requested(dict(stream_instance_id='one',packets=[[65500,0xffff0000,9]]))==[]
    small=AudioPacketCache(max_packets=2,max_bytes=100)
    for p in packets: small.put(p,'s')
    assert small.snapshot()['cache_packets']<=2 and small.snapshot()['cache_bytes']<=100
    orphan=audio.RtpGapRepair(clock=lambda:now[0])
    orphan.observe(audio.parse_rtp(packets[0],0),'one',960)
    orphan.observe(audio.parse_rtp(packets[5],0),'one',960)
    now[0]+=7
    assert orphan.request('s') is None and orphan.snapshot()['expired']==4
    print('PASS cache bounds, malformed requests, 16/32-bit wrap, 2.4s gap, epoch, expiry')


def integration():
    # Compile the real gate class without loading Vosk models or audio libraries.
    tree=ast.parse((VOICE/'vosk_wake.py').read_text())
    cls=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name=='UdpPacketGate')
    env=dict(socket=socket,threading=threading,time=time,uuid=uuid,struct=struct,json=json,
             AudioPacketCache=AudioPacketCache,CaptureRebuildGuard=CaptureRebuildGuard,runtime_log=lambda *a:None)
    exec(compile(ast.Module(body=[cls],type_ignores=[]),'real_gate','exec'),env)
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        (root/'nas').mkdir()
        cfg=root/'config.json'
        port=free_port(socket.SOCK_DGRAM)
        control=free_port(socket.SOCK_DGRAM)
        timing=free_port(socket.SOCK_DGRAM)
        cfg.write_text(json.dumps(dict(bind_ip='127.0.0.1', timing_port=timing,
            admin_bind_ip='127.0.0.1',admin_port=free_port(socket.SOCK_STREAM),
            receiver_admin_url='http://127.0.0.1:1',staging_root=str(root/'staging'),
            nas_root=str(root/'nas'),nas_require_mount=False,nas_low_space_warning_mb=0,min_free_disk_mb=0,
            input_warning_seconds=10**12,input_rebuild_seconds=10**12,
            task_allowed_roots=[str(root/'video')],
            streams=[dict(sender_id='s',ssrc=9,port=port,control_host='127.0.0.1',
                          control_port=control,repair_enabled=True)])))
        service=audio.AudioArchiveService(audio.load_config(cfg))
        original_map = service.timing.map_packet
        def jittered_map(sender, packet):
            packet = original_map(sender, packet)
            packet.global_us += 11000 if packet.sequence % 2 else 9000
            return packet
        service.timing.map_packet = jittered_map
        proxy=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        proxy.bind(('127.0.0.1',0)); proxy.settimeout(.1)
        sink=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        sink.bind(('127.0.0.1',0))
        gate=env['UdpPacketGate']('127.0.0.1',sink.getsockname()[1],
            secondary_remote=proxy.getsockname(),timing_remote=('127.0.0.1',timing),
            sender_id='s',control_port=control)
        gate._last_timing_monotonic=time.monotonic()+60
        stop=threading.Event(); attempts={}
        def relay():
            while not stop.is_set():
                try: p,_=proxy.recvfrom(2048)
                except socket.timeout: continue
                seq=struct.unpack('!H',p[2:4])[0]
                attempts[seq]=attempts.get(seq,0)+1
                if 10<=seq<130 and attempts[seq] <= (2 if seq%7==0 else 1):
                    continue
                proxy.sendto(p,('127.0.0.1',port))
        thread=threading.Thread(target=relay)
        service.start(); service.set_all_enabled(False); gate.start(); thread.start()
        try:
            start=((audio.now_us()+19999)//20000)*20000
            service.timing.update_anchor('s',dict(stream_instance_id=gate._stream_instance_id,
                rtp_timestamp=0,sender_system_timestamp_us=start,sample_rate=48000,ssrc=9,
                audio_retransmit={}),audio.now_us())
            directory=root/'video/segment'; directory.mkdir(parents=True)
            spec=audio.TaskAudioSpec(sender_id='s',camera_id='cam01',recording_session_id=99,
                directory=directory,window_start_us=start,window_end_us=start+4000000)
            service.streams['s'].start_task(spec)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as source:
                for i in range(200):
                    source.sendto(audio.make_rtp(i,i*960,9,111,audio.OPUS_SILENCE_20_MS),
                                  ('127.0.0.1',gate.local_port))
                    time.sleep(.002)
                source.sendto(audio.make_rtp(199,199*960,9,111,audio.OPUS_SILENCE_20_MS),
                              ('127.0.0.1',gate.local_port))
            deadline=time.monotonic()+18
            while time.monotonic()<deadline:
                if service.streams['s'].finalize_task(directory,start+4000000,'test'):
                    break
                time.sleep(.05)
            meta=json.loads((directory/'audio_meta.json').read_text())
            assert meta['received_packets']==200 and meta['silence_packets']==0,meta
            assert meta['duplicate_packets'] >= 1, meta
            grid_status = service.streams['s'].status()['audio_slot_mapping']
            assert grid_status['jitter_adjusted_packets'] > 0, grid_status
            assert grid_status['distinct_packet_collisions'] == 0, grid_status
            replay_directory = root/'video/replay'; replay_directory.mkdir()
            replay = audio.TaskAudioSpec(sender_id='s',camera_id='cam02',recording_session_id=99,
                directory=replay_directory,window_start_us=start,window_end_us=start+4000000)
            service.streams['s'].start_task(replay)
            assert service.streams['s'].finalize_task(replay_directory,start+4000000,'test')
            replay_meta = json.loads((replay_directory/'audio_meta.json').read_text())
            for key in ('received_packets', 'silence_packets', 'duplicate_packets', 'late_packets'):
                assert replay_meta[key] == meta[key], (key, replay_meta, meta)
            assert service.streams['s']._repair.snapshot()['recovered']==120
            assert gate._packet_cache.snapshot()['retransmitted']>=120
            print('PASS real UDP: jitter, duplicates, replay counters, 120 dropped packets recovered; 200/200')
        finally:
            stop.set(); thread.join(1); gate.stop(); proxy.close(); sink.close(); service.stop()
            assert not thread.is_alive()


if __name__=='__main__':
    unit()
    integration()
