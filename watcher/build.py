#!/usr/bin/env python3
"""Build the accepted Watcher 0.1.1 profile. No hardware writes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
LOCK = json.loads((ROOT / "SOURCES.json").read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_sources():
    for name, expected in LOCK["source_files"].items():
        if digest(REPO / name) != expected:
            raise SystemExit("Reviewed source differs: " + name)
    if digest(ROOT / "dependencies.lock") != LOCK["component_lock_sha256"]:
        raise SystemExit("Component lock checksum mismatch")


def verify(build):
    settings = {}
    for line in (build / "sdkconfig").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            settings[key] = value
    wanted = {"CONFIG_IDF_TARGET": '"esp32s3"',
              "CONFIG_IMPO_BOARD_SENSECAP_WATCHER": "y", "CONFIG_IMPO_BOARD_ID": '"sensecap_watcher"',
              "CONFIG_SPIRAM": "y", "CONFIG_SPIRAM_MODE_OCT": "y",
              "CONFIG_ESPTOOLPY_FLASHSIZE": '"32MB"',
              "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": '"partitions_impo.csv"',
              "CONFIG_PARTITION_TABLE_OFFSET": "0x10000", "CONFIG_IMPO_CJK_FONT": "y",
              "CONFIG_IMPO_DISABLE_SAVED_NOTES": "y"}
    disabled = ["CONFIG_IMPO_WATCHER_CAMERA", "CONFIG_HOMEHUB_TUNNEL", "CONFIG_HOMEHUB_OTA_ENABLED",
                "CONFIG_HOMEHUB_NVS_ENCRYPTION", "CONFIG_NVS_ENCRYPTION", "CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH",
                "CONFIG_SECURE_BOOT", "CONFIG_SECURE_BOOT_ENABLED", "CONFIG_SECURE_FLASH_ENC_ENABLED"]
    failures = [key for key, value in wanted.items() if settings.get(key) != value]
    failures += [key for key in disabled if settings.get(key) not in (None, "n")]
    if failures:
        raise SystemExit("Unexpected build configuration: " + ", ".join(failures))
    image = build / "impo-gadget.bin"
    if not image.is_file() or image.stat().st_size > 4 * 1024 * 1024 * .9:
        raise SystemExit("Application missing or has less than 10 percent slot margin")
    if (REPO / "esp32/components/impo/watcher_description.json").read_bytes() != (ROOT / "description.json").read_bytes():
        raise SystemExit("Embedded device description differs from the reviewed source")
    report = {"configuration_verified": True, "firmware": LOCK["firmware_version"],
              "bytes": image.stat().st_size, "sha256": digest(image),
              "flash_performed": False}
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["build", "test", "verify"])
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    verify_sources()
    esp32 = REPO / "esp32"
    build = args.build.resolve() if args.build else esp32 / "build-impo-sensecap-watcher"
    if args.action == "verify":
        verify(build)
    elif args.action == "test":
        subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_*.py"], cwd=esp32, check=True)
    else:
        idf = shutil.which("idf.py")
        if not idf:
            raise SystemExit("Activate ESP-IDF 6.0.1 with its export.sh first")
        version = subprocess.run([idf, "--version"], text=True, capture_output=True, check=True).stdout
        if "v6.0.1" not in version and " 6.0.1" not in version:
            raise SystemExit("ESP-IDF 6.0.1 is required")
        dependency_lock = esp32 / "dependencies.lock"
        if dependency_lock.exists() and digest(dependency_lock) != LOCK["component_lock_sha256"]:
            raise SystemExit("Another board's component lock is present; use a clean checkout")
        shutil.copyfile(ROOT / "dependencies.lock", dependency_lock)
        defaults = ";".join(["sdkconfig.defaults", "devices/sdkconfig.impo", "devices/sdkconfig.impo-sensecap-watcher", str(ROOT / "sdkconfig.watcher")])
        subprocess.run([idf, "-B", str(build), "-DIDF_TARGET=esp32s3", "-DPROJECT_VER=" + LOCK["firmware_version"],
                        "-DSDKCONFIG=" + str(build / "sdkconfig"), "-DSDKCONFIG_DEFAULTS=" + defaults, "build"], cwd=esp32, check=True)
        verify(build)


if __name__ == "__main__":
    main()
