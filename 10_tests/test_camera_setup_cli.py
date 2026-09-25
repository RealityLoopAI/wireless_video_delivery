"""Check setup arguments without opening a camera; pass a native helper binary."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True)
    args = parser.parse_args()
    help_result = subprocess.run([args.helper, "--help"], capture_output=True, text=True, timeout=10)
    assert help_result.returncode == 0, help_result
    assert "--denoise-level" in help_result.stdout, "Native color denoising option is missing"
    assert "0 = automatic" in help_result.stdout, "Level zero must be documented as automatic"
    invalid = [
        [], ["--serial", "CV275610004S"], ["--reference", "1"],
        ["--serial", "", "--reference", "1"],
        ["--serial", "CV275610004S", "--reference", "2"],
    ]
    base = ["--serial", "CV275610004S", "--reference", "1"]
    invalid += [base + ["--denoise-level", value] for value in ["-1", "9", "4x", "1.5", "", "999999999999"]]
    invalid += [base + ["--denoise-level"], base + ["--denoise-level", "4", "--denoise-level", "4"]]
    invalid += [base + ["--reference", "1"], base + ["--serial", "another-device"]]
    for case in invalid:
        result = subprocess.run([args.helper, *case], capture_output=True, text=True, timeout=10)
        assert result.returncode == 2, (case, result.returncode, result.stdout, result.stderr)
        assert not result.stdout, (case, "Invalid arguments reached SDK setup", result.stdout)
    print(f"PASS: help and {len(invalid)} invalid-argument cases")


if __name__ == "__main__":
    main()
