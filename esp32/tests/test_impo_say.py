"""speaker.say after the chat session has closed for being idle.

The session closes after IDLE_CLOSE_US unused and only a voice turn opened it
again, so a speaker.say in between was answered "fetching" and then dropped
("say: not connected"): 74 of 83 on a StackChan in one day. A say now connects
first, as a voice turn does.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/impo/impo_chat_session.cpp"


class SayConnects(unittest.TestCase):
    def setUp(self) -> None:
        self.source = SOURCE.read_text()

    def case_body(self, label: str) -> str:
        start = self.source.index(label)
        end = self.source.index("break;", start)
        return self.source[start:end]

    def test_say_connects_first_like_a_voice_turn(self) -> None:
        say = self.case_body("    case CMD_SAY:")
        self.assertIn("if (ensure_connected()) {", say)
        self.assertLess(say.index("ensure_connected()"), say.index("say_begin(cmd.text);"))
        self.assertNotIn("s_conn.session", say)
        self.assertNotIn("say: not connected", self.source)

    def test_the_idle_close_still_leaves_the_next_use_to_connect(self) -> None:
        idle = re.search(r'disconnect\("idle"\);.*?s_auto_next_us = INT64_MAX;', self.source, re.S)
        self.assertIsNotNone(idle)
        turn = self.source[self.source.index("static bool turn_start("):]
        self.assertIn("if (!ensure_connected()) {", turn[:turn.index("\n}\n")])


if __name__ == "__main__":
    unittest.main()
