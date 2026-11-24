# Copyright (c) Impo contributors.
# SPDX-License-Identifier: Apache-2.0
"""The StackChan's manifest (devices/m5stack-stackchan/manifest.py) names only
commands the firmware registers, the CoreS3's first and the body's after them,
and the events the firmware sends."""
from pathlib import Path
import json
import re
import subprocess
import sys
import unittest

ESP32 = Path(__file__).resolve().parent.parent
STACKCHAN = ESP32 / "devices/m5stack-stackchan"
CORES3 = ESP32 / "devices/m5stack-cores3"


def registered(path):
    m = re.search(r's_commands\[\]\s*=\s*\{(.*?)\n\};', path.read_text(), re.S)
    return re.findall(r'\{\s*"([^"]+)"', m.group(1))


def sent_events(path):
    return re.findall(r'impo_link_send_event\("([a-z_]+)"', path.read_text())


class StackChanManifestTest(unittest.TestCase):
    def setUp(self):
        made = subprocess.run([sys.executable, str(STACKCHAN / "manifest.py")],
                              capture_output=True, text=True)
        self.assertEqual(made.returncode, 0, made.stderr)
        self.manifest = json.loads(made.stdout)

    def test_commands(self):
        names = [c["name"] for c in self.manifest["commands"]]
        own = registered(CORES3 / "board.c") + registered(STACKCHAN / "stackchan.cpp")
        self.assertLessEqual(set(own), set(names))
        self.assertEqual(names, sorted(names))   # a kind's commands sit together where people read them
        self.assertTrue(all("result" in c for c in self.manifest["commands"] if c["name"] != "device.set_name"))
        self.assertIn("camera.capture", names)
        self.assertEqual(len(names), len(set(names)))
        self.assertLessEqual(len(self.manifest["systemPrompt"]), 4000)

    def test_events(self):
        sent = set(sent_events(CORES3 / "imu.cpp")) | set(sent_events(STACKCHAN / "stackchan.cpp"))
        self.assertEqual({e["name"] for e in self.manifest["events"]}, sent)

    def test_product(self):
        product = re.search(r'^CONFIG_GADGET_PRODUCT_ID="([^"]+)"', (STACKCHAN / "sdkconfig").read_text(), re.M)
        self.assertEqual(self.manifest["product"], product.group(1))


if __name__ == "__main__":
    unittest.main()
