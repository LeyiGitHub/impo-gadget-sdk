# Copyright (c) Impo contributors.
# SPDX-License-Identifier: Apache-2.0
"""The StackChan describes itself (CAPABILITIES.md, "A gadget describes itself"):
main/platform.json for the platform's commands and
devices/m5stack-stackchan/description.json for the device's. Each must be what
the Impo platform accepts (contracts/gadget-identity.md in impoai/impo), and
must describe exactly the commands the firmware runs and the events it sends.
tools/impo/board.sh runs this before it builds the StackChan."""
from pathlib import Path
import json
import re
import unittest

ESP32 = Path(__file__).resolve().parent.parent
STACKCHAN = ESP32 / "devices/m5stack-stackchan"
CORES3 = ESP32 / "devices/m5stack-cores3"
COMMON = ESP32 / "components/impo/impo_commands.c"
NOISE = ESP32 / "main/noise_control.cpp"

# The platform's commands, which Link registers itself (noise_control.cpp).
PLATFORM = {"device.health", "device.discover"}
# Device commands Link registers for a board that offers them (noise_control.cpp).
LINK_FOR_DEVICE = {"camera.capture", "display.draw_url", "display.show_animation"}

# contracts/gadget-identity.md, "The manifest": the schema subset and its limits.
TYPES = {"object", "string", "number", "integer", "boolean", "array"}
KEYWORDS = {"type", "description", "properties", "required", "additionalProperties", "items", "enum",
            "minimum", "maximum", "minLength", "maxLength", "minItems", "maxItems", "default"}
COMMAND_NAME = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z0-9_]+){0,3}")
EVENT_NAME = re.compile(r"[a-z_]{1,32}")


def table(path, name):
    """The command names in `name[] = { {"a.b", run}, ... };`."""
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", path.read_text(), re.S)
    assert m, f"{name} not found in {path}"
    return re.findall(r'\{\s*"([^"]+)"', m.group(1))


def schema_error(schema, depth=1):
    if not isinstance(schema, dict):
        return "a schema must be an object"
    if depth > 4:
        return "schemas nest at most 4 levels"
    unknown = set(schema) - KEYWORDS
    if unknown:
        return f"{sorted(unknown)} not supported"
    if schema.get("type") not in TYPES:
        return "type must be one of " + ", ".join(sorted(TYPES))
    if "additionalProperties" in schema and schema["additionalProperties"] is not False:
        return "additionalProperties may only be false"
    if "enum" in schema and not (isinstance(schema["enum"], list) and schema["enum"]):
        return "enum must be a nonempty list"
    properties = schema.get("properties")
    if properties is not None:
        if schema["type"] != "object" or not isinstance(properties, dict):
            return "properties belong to an object schema"
        for name, child in properties.items():
            error = schema_error(child, depth + 1)
            if error:
                return f"{name}: {error}"
    if "required" in schema and not all(name in (properties or {}) for name in schema["required"]):
        return "required must list names from properties"
    if "items" in schema:
        if schema["type"] != "array":
            return "items belong to an array schema"
        error = schema_error(schema["items"], depth + 1)
        if error:
            return f"items: {error}"
    return None


class StackChanDescriptionTest(unittest.TestCase):
    def setUp(self):
        self.platform = json.loads((ESP32 / "main/platform.json").read_text())
        self.device = json.loads((STACKCHAN / "description.json").read_text())

    def check_commands(self, commands):
        names = [c["name"] for c in commands]
        self.assertEqual(len(names), len(set(names)), "command names are unique")
        for c in commands:
            self.assertLessEqual(set(c), {"name", "description", "parameters", "result"}, c["name"])
            self.assertRegex(c["name"], COMMAND_NAME)
            self.assertTrue(1 <= len(c["description"]) <= 500, (c["name"], len(c["description"])))
            self.assertEqual(c["parameters"].get("type"), "object", c["name"])
            self.assertIn("properties", c["parameters"], f"{c['name']}: parameters describe their properties")
            self.assertIn("result", c, f"{c['name']}: says what an ok answer holds")
            for part in ("parameters", "result"):
                self.assertIsNone(schema_error(c[part]), f"{c['name']} {part}")
                self.assertLessEqual(len(json.dumps(c[part])), 8 * 1024, f"{c['name']} {part}")
        return names

    def test_format(self):
        self.assertEqual(set(self.platform), {"commands"})
        self.assertEqual(set(self.device), {"about", "commands", "events"})
        self.assertTrue(1 <= len(self.device["about"]) <= 4000)
        both = self.check_commands(self.platform["commands"]) + self.check_commands(self.device["commands"])
        self.assertEqual(len(both), len(set(both)), "the platform and the device name different commands")
        self.assertLessEqual(len(both), 64)
        for e in self.device["events"]:
            self.assertEqual(set(e), {"name", "description", "data"}, e["name"])
            self.assertRegex(e["name"], EVENT_NAME)
            self.assertTrue(1 <= len(e["description"]) <= 200, (e["name"], len(e["description"])))
            self.assertIsNone(schema_error(e["data"]), e["name"])

    def test_platform_commands_are_the_ones_link_registers(self):
        registered = set(re.findall(r'add_command\(commands, "([^"]+)"\)', NOISE.read_text()))
        self.assertEqual({c["name"] for c in self.platform["commands"]}, PLATFORM)
        self.assertLessEqual(PLATFORM, registered)

    def shared(self):
        """The body's shared_commands: what it offers of the commands outside its own table."""
        m = re.search(r'"((?:[a-z_]+\.[a-z_]+,)*[a-z_]+\.[a-z_]+)",\s*\};', (STACKCHAN / "stackchan.cpp").read_text())
        self.assertTrue(m, "the StackChan lists the shared commands it offers")
        return m.group(1).split(",")

    def test_device_commands_are_the_ones_the_firmware_runs(self):
        own = table(STACKCHAN / "stackchan.cpp", "s_commands")
        elsewhere = set(table(COMMON, "COMMON") + table(COMMON, "COMMON_LATER") + table(CORES3 / "board.c", "s_commands")) | LINK_FOR_DEVICE
        self.assertLessEqual(set(self.shared()), elsewhere)
        self.assertEqual({c["name"] for c in self.device["commands"]}, set(own) | set(self.shared()))

    def test_no_events(self):
        """M5Stack's firmware tells its AI of no touch or shake: the StackChan sends no event, and
        leaves out the CoreS3's motion sensor (imu.read, shaken)."""
        self.assertEqual(self.device["events"], [])
        self.assertNotIn("impo_link_send_event", (STACKCHAN / "stackchan.cpp").read_text())
        self.assertNotIn("imu.read", self.shared())

    def test_nothing_names_a_product(self):
        self.assertNotIn("PRODUCT_ID", (STACKCHAN / "sdkconfig").read_text())
        self.assertFalse((STACKCHAN / "manifest.py").exists())


if __name__ == "__main__":
    unittest.main()
