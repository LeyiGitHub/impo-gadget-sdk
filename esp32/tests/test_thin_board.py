"""A board runs only what the cloud prepared, and answers once it is done.

CAPABILITIES.md, "What a board does": speech is made in the cloud and played
with speaker.play; nods, shakes and dances are frames the agent writes for
head.play; a touch or a shake is only an event. A command whose work takes
time is an impo_later_command_t, answered when the work has finished.
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
    def setUp(self) -> None:
        self.source = STACKCHAN.read_text()

    def test_moves_are_the_agents(self) -> None:
        names = table(self.source, "s_commands") + table(self.source, "s_later_commands")
        for gone in ("head.look", "head.nod", "head.shake", "head.dance", "head.center"):
            self.assertNotIn(gone, names)
        self.assertEqual(table(self.source, "s_later_commands"), ["head.move", "head.play"])

    def test_a_move_answers_with_what_the_servos_measured(self) -> None:
        move = function(self.source, "cJSON *run_move(head_job_t *job)")
        after = move[move.index("aim(f);"):]
        self.assertLess(after.index("settle(job)"), after.index("rest_reading(&yaw, &pitch, &revived)"))
        self.assertIn("isMoving()", function(self.source, "bool settle(const head_job_t *job)"))
        self.assertIn('"not_reached"', move)
        self.assertIn('"servos_silent"', move)
        self.assertIn('revive(', function(self.source, 'bool rest_reading('))
        self.assertEqual(move.count("aim(f);"), 2)   # once more when it stopped short
        play = function(self.source, "cJSON *run_play(head_job_t *job)")
        self.assertIn('"not_reached"', play)
        read = function(self.source, "bool read_head(int *yaw, int *pitch)")
        self.assertIn("hal_impo_servo_short_reads() == before", read)

    def test_a_touch_or_a_shake_is_only_an_event(self) -> None:
        touch = function(self.source, "void on_touch(HeadPetGesture g, uint32_t now)")
        for reflex in ("moveWithSpeed", "show(", "impo_state_make_happy", "setModifyLock"):
            self.assertNotIn(reflex, touch)
        self.assertIn('impo_link_send_event("head_touched", data)', touch)
        self.assertNotIn("m5stack_cores3_imu_on_shake", self.source)
        self.assertNotIn("setModifyLock", self.source)


if __name__ == "__main__":
    unittest.main()
