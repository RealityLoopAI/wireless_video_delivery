import os
import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HOOK = ROOT / '05_tools/sender_wifi_dispatcher.sh'


class WifiDispatcherTests(unittest.TestCase):
    def invoke(self, iface, action, fail=False):
        script = '''
systemd-escape() { printf 'gwv3-sender-wifi-tuning@%s.service\\n' "${@: -1}"; }
systemctl() { printf 'systemctl %s\\n' "$*"; return "$FAIL"; }
source "$HOOK" "$IFACE" "$ACTION"
'''
        env = dict(os.environ, HOOK=str(HOOK), IFACE=iface, ACTION=action, FAIL=str(int(fail)))
        return subprocess.run(['bash', '-c', script], env=env, capture_output=True, text=True)

    def test_up_and_reapply_schedule_nonblocking_per_interface(self):
        for action in ('up', 'reapply'):
            result = self.invoke('wlan0', action)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(),
                             'systemctl --no-block start gwv3-sender-wifi-tuning@wlan0.service')

    def test_other_events_do_not_reset_queues(self):
        for action in ('down', 'pre-up', 'dhcp4-change', 'connectivity-change', ''):
            result = self.invoke('wlan0', action)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '')

    def test_invalid_interface_is_ignored(self):
        for iface in ('', '../wlan0', '--all', 'wlan0;reboot', 'x'*16):
            result = self.invoke(iface, 'up')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '')

    def test_tuning_failure_does_not_fail_network_activation(self):
        result = self.invoke('wlan0', 'up', fail=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('could not schedule', result.stderr)

    def test_unit_does_not_remain_active_and_is_bounded(self):
        unit = (ROOT / '06_configs/gwv3-sender-wifi-tuning@.service').read_text()
        self.assertIn('Type=oneshot', unit)
        self.assertNotIn('RemainAfterExit=yes', unit)
        self.assertIn('TimeoutStartSec=10', unit)
        self.assertIn('/usr/local/sbin/gwv3-apply-sender-wifi-tuning %I', unit)

    def test_queue_is_replaced_only_when_not_already_configured(self):
        examples = (
            ('[{"kind":"pfifo","root":true,"options":{"limit":128}}]', False),
            ('[{"kind":"pfifo","root":true,"options":{"limit":1000}}]', True),
            ('[{"kind":"noqueue","root":true,"options":{}}]', True),
            ('[{"kind":"pfifo","root":false,"options":{"limit":128}}]', True),
            ('invalid-json', True),
            ('[]', True),
        )
        for queues, replaced in examples:
            result = subprocess.run(['bash', '-c', '''
source "$APPLY"
tc() {
  if [[ "$1" == "-j" ]]; then printf '%s' "$QUEUES";
  else printf 'replace %s\\n' "$*"; fi
}
sender_wifi_ensure_queue wlan0 128
'''], env=dict(os.environ, APPLY=str(ROOT/'05_tools/apply_sender_wifi_tuning.sh'),
               QUEUES=queues), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual('replace' in result.stdout, replaced, queues)

    def test_only_validated_driver_families_are_tuned(self):
        for driver in ('rtw_8821cu', 'rtl8821cu', 'rtw89_8852be', 'virtio_net', ''):
            result = subprocess.run(['bash', '-c',
                'source "$APPLY"; sender_wifi_driver_supported "$DRIVER"'],
                env=dict(os.environ, APPLY=str(ROOT/'05_tools/apply_sender_wifi_tuning.sh'),
                         DRIVER=driver))
            self.assertEqual(result.returncode == 0, driver in ('rtw_8821cu', 'rtl8821cu'))


if __name__ == '__main__':
    unittest.main()
