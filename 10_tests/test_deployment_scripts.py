#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import tempfile
import unittest


SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[1]


class DeploymentScriptTests(unittest.TestCase):
    def run_bash(self, script: str, *, check: bool = True):
        return subprocess.run(
            ["bash", "-c", script],
            cwd=SOURCE_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
        )

    def test_modified_shell_scripts_parse(self):
        scripts = [
            "05_tools/audit_time_sync_conflicts.sh",
            "05_tools/gwv3_doctor.sh",
            "05_tools/gwv3_sender_service_launcher.sh",
            "05_tools/install_device.sh",
            "05_tools/install_sender_service.sh",
            "05_tools/rollback_sender_install.sh",
            "05_tools/sender_watchdog.sh",
            "05_tools/sender_wifi_guard.sh",
            "05_tools/setup_receiver_chrony_server.sh",
            "05_tools/setup_sender_chrony_client.sh",
        ]
        subprocess.run(["bash", "-n", *scripts], cwd=SOURCE_ROOT, check=True)

    def test_wifi_guard_selects_saved_5ghz_without_fixed_connection(self):
        with tempfile.TemporaryDirectory() as tmp:
            calls = pathlib.Path(tmp) / "calls"
            result = self.run_bash(
                f"""
set -euo pipefail
source 05_tools/sender_wifi_guard.sh
export GEMINI_SENDER_WIFI_MIN_FREQ_MHZ=5000
unset GEMINI_SENDER_WIFI_CONNECTION GEMINI_SENDER_WIFI_REQUIRED_SSID || true
export CALLS={calls}
gemini_sender_wifi_check_policy() {{ return 1; }}
gemini_sender_wifi_find_visible_saved_connection() {{ printf 'saved-5g\n'; }}
gemini_sender_wifi_disable_powersave() {{ return 0; }}
nmcli() {{ printf '%s\n' "$*" >> "$CALLS"; return 0; }}
gemini_sender_wifi_connect_if_configured
cat "$CALLS"
"""
            )
            self.assertIn(
                "connection modify saved-5g 802-11-wireless.band a", result.stdout
            )
            self.assertIn("connection up saved-5g ifname wlan0", result.stdout)

    def test_wifi_scan_uses_highest_frequency_for_dual_band_ssid(self):
        result = self.run_bash(
            """
set -euo pipefail
source 05_tools/sender_wifi_guard.sh
nmcli() {
  if [[ "$*" == *"dev wifi list"* ]]; then
    printf 'camera-net:2412:\ncamera-net:5200:\n'
  fi
}
value="$(gemini_sender_wifi_visible_freq_for_ssid camera-net)"
[[ "$value" == "5200" ]]
printf '%s\n' "$value"
"""
        )
        self.assertEqual(result.stdout.strip(), "5200")

    def test_watchdog_monitors_application_log_with_monotonic_time(self):
        text = (SOURCE_ROOT / "05_tools/sender_watchdog.sh").read_text(encoding="utf-8")
        self.assertIn('APP_LOG_FILE="${GEMINI_SENDER_HEALTH_LOG_FILE', text)
        self.assertIn('tail -c "$new_bytes" "$APP_LOG_FILE"', text)
        self.assertIn("/proc/uptime", text)
        self.assertNotIn('tail -c "$new_bytes" "$WATCHDOG_LOG_FILE"', text)

    def test_chrony_only_steps_during_startup(self):
        for name in ("setup_sender_chrony_client.sh", "setup_receiver_chrony_server.sh"):
            text = (SOURCE_ROOT / "05_tools" / name).read_text(encoding="utf-8")
            self.assertNotIn("makestep 0.05 -1", text)
            self.assertNotIn("makestep 0.1 -1", text)
            self.assertIn("audit_time_sync_conflicts.sh", text)
            self.assertNotIn("192.168.1.196", text)

    def test_generic_sender_unit_uses_installed_launcher(self):
        text = (
            SOURCE_ROOT / "05_tools/systemd/gwv3-gemini-sender.service"
        ).read_text(encoding="utf-8")
        self.assertIn("/usr/local/sbin/gwv3-sender-service-launcher --run", text)
        self.assertNotIn("/home/", text)
        launcher = (
            SOURCE_ROOT / "05_tools/gwv3_sender_service_launcher.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("wait_chrony_sync.sh", launcher)
        self.assertIn("GWV3_CHRONY_STARTUP_TRIES", launcher)

    def test_installer_is_transactional_and_preserves_service_state(self):
        installer = (SOURCE_ROOT / "05_tools/install_sender_service.sh").read_text(
            encoding="utf-8"
        )
        rollback = (SOURCE_ROOT / "05_tools/rollback_sender_install.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("rollback_on_exit", installer)
        self.assertIn("service-state.tsv", installer)
        self.assertIn("service-state.tsv", rollback)
        self.assertIn("enabled_state", rollback)

    def test_installed_doctor_resolves_repository_from_release_metadata(self):
        text = (SOURCE_ROOT / "05_tools/gwv3_doctor.sh").read_text(encoding="utf-8")
        self.assertIn("repository_root", text)
        self.assertIn('ROOT_DIR="$installed_root"', text)
        self.assertIn("chronyc waitsync 1 0.010", text)
        self.assertIn("CLOCK_SYNC is healthy", text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path)
    args, remaining = parser.parse_known_args()
    global SOURCE_ROOT
    if args.source_root:
        SOURCE_ROOT = args.source_root.resolve()
    unittest.main(argv=[__file__, *remaining])


if __name__ == "__main__":
    main()
