"""A board runs only what the cloud prepared.

CAPABILITIES.md, "What a board does": speech is made in the cloud and played
with speaker.play, which answers once the sound has played. The StackChan's
commands are those of M5Stack's own firmware.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = ROOT / "components/impo/impo_commands.c"
STACKCHAN = ROOT / "devices/m5stack-stackchan/stackchan.cpp"


def table(source: str, name: str) -> list[str]:
    body = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    assert body, name
    return re.findall(r'\{\s*"([^"]+)"', body.group(1))


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


class Sound(unittest.TestCase):
    def setUp(self) -> None:
        self.source = COMMANDS.read_text()

    def test_the_board_plays_and_never_speaks(self) -> None:
        names = table(self.source, "COMMON") + table(self.source, "COMMON_LATER")
        self.assertNotIn("speaker.say", names)
        self.assertNotIn("speaker.beep", names)
        self.assertEqual(table(self.source, "COMMON_LATER"), ["speaker.play", "speaker.play_url"])
        self.assertRegex(self.source, r'\{ "speaker.play",\s+play,\s+120000 \}')

    def test_a_sound_answers_after_it_has_played(self) -> None:
        task = function(self.source, "static void play_task(void *arg)")
        self.assertLess(task.index("impo_sound_outcome(n, &played)"), task.index("impo_command_reply(job->reply, result)"))
        self.assertIn("IMPO_SOUND_DONE", task)
        self.assertIn('"stopped"', task)
        self.assertIn('"too_long"', task)

    def test_cloud_bytes_come_a_piece_at_a_time_while_there_is_room(self) -> None:
        fetch = function(self.source, "static cJSON *fetch_cloud(")
        self.assertLess(fetch.index("impo_sound_room() < PIECE_BYTES"), fetch.index("impo_link_req_open"))
        self.assertIn('"%s?offset=%u&length=%u"', fetch)

    def test_a_url_that_redirects_is_followed(self) -> None:
        fetch = function(self.source, "static cJSON *fetch_url(")
        self.assertIn("esp_http_client_set_redirection(http)", fetch)
        self.assertIn('"the URL answered HTTP %d"', fetch)


class StackChan(unittest.TestCase):
    """The StackChan's commands are the tools M5Stack's StackChan firmware gives
    its AI (hal_mcp.cpp, xiaozhi's mcp_server.cc), done as it does them."""

    def setUp(self) -> None:
        self.source = STACKCHAN.read_text()

    def test_only_the_tools_of_m5stacks_firmware(self) -> None:
        self.assertEqual(table(self.source, "s_commands"),
                         ["head.status", "head.move", "lights.set", "speaker.set_volume", "display.set_brightness"])
        self.assertNotIn("s_later_commands", self.source)
        self.assertNotIn("impo_link_send_event", self.source)

    def test_a_move_is_set_head_angles(self) -> None:
        move = function(self.source, "cJSON *head_move(const cJSON *params)")
        self.assertLess(move.index("motion.pitchServo().moveWithSpeed(pitch * 10, speed);"),
                        move.index("motion.yawServo().moveWithSpeed(yaw * 10, speed);"))
        self.assertNotIn("isMoving", move)
        status = function(self.source, "cJSON *head_status(const cJSON *params)")
        self.assertIn("motion.yawServo().getCurrentAngle() / 10", status)


if __name__ == "__main__":
    unittest.main()
