# Recording control buttons

This service maps the two non-power buttons on the LubanCat RK3576 carrier to
receiver-wide recording controls after Linux has booted:

- `RECOVERY` / SARADC channel 1: hold for 1 second to start all cameras.
- `MASKROM` / SARADC channel 0: hold for 1 second to stop all cameras.
- Start queues the existing `ding` prompt; stop queues the existing `deng`
  prompt through the local Xiaohuan speech service.
- `ON/OFF` / RK805 `KEY_POWER`: hold for 5 seconds to stop active recording,
  wait for the current speech to finish, play `ding + 100ms + deng`, and power
  off. Releasing before 5 seconds cancels the action.
- Each Linux boot queues `deng + 100ms + ding` once after the speech service
  becomes available. It gives up after 30 seconds and never delays boot.

Short presses and simultaneous holds are ignored. A held button fires once and
must be released before it can fire again. Receiver requests are retried up to
three times and the prompt is only queued after a successful receiver response.
The service calls the receiver's LAN-facing Web proxy on port `8080`; the
loopback-only receiver admin port `18080` must remain private.

The boot-time Recovery and Maskrom behavior is not changed. The service only
reads the SARADC channels after Linux is running.

The power key is handled by the root system service
`gwv3-power-button.service`; the SARADC keys remain in the unprivileged user
service `gwv3-recording-buttons.service`. `systemd-logind` must use
`HandlePowerKey=ignore` so a short press cannot bypass the 5-second policy.

## Install

```bash
./install_service.sh
systemctl --user status gwv3-recording-buttons.service
```

## Probe

```bash
python3 recording_button_service.py \
  --config config_lubancat-52d2ef0c.json --probe
```

Released values should be above `released_above`; pressed values should be
below `pressed_below`.

## Runtime checks

```bash
systemctl --user is-active gwv3-recording-buttons.service
journalctl --user -u gwv3-recording-buttons.service -f
sudo systemctl is-active gwv3-power-button.service
sudo journalctl -u gwv3-power-button.service -f
curl -sS http://192.168.66.196:8080/api/status
curl -sS http://127.0.0.1:18082/healthz
```

The speech health response must list `ding`, `deng`, `startup`, and `shutdown`
in `cue_names`.
The cue submission endpoint is intentionally loopback-only so LAN clients
cannot make the device play these control sounds directly.
