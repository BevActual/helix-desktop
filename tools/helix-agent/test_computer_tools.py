"""The computer-use tools of the Helix MCP server: how they turn into `display-*` commands, what they refuse before asking, and how a request for help waits.
Run with the server's own environment:  .venv/bin/python -m unittest test_computer_tools -v
`helix_msg` is replaced, so nothing reaches a desktop."""

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import server


class Recorder:
    def __init__(self, replies=None):
        self.calls = []
        self.replies = list(replies or [])

    def __call__(self, *args, literal=(), timeout=30.0):
        self.calls.append((args, literal))
        return self.replies.pop(0) if self.replies else "ok"


class ComputerTools(unittest.TestCase):
    def test_each_tool_becomes_its_display_command(self):
        rec = Recorder()
        with mock.patch.object(server, "helix_msg", rec):
            server.computer_launch("Firefox")
            server.computer_click(640, 360)
            server.computer_click(1, 2, "right")
            server.computer_move(5, 6)
            server.computer_scroll(3)
            server.computer_scroll(-2, across=4)
            server.computer_key("Control_L+a")
            server.computer_type("hello world")
            server.computer_state()
        self.assertEqual(
            rec.calls,
            [
                (("display-launch",), ("Firefox",)),
                (("display-click", "640", "360", "left"), ()),
                (("display-click", "1", "2", "right"), ()),
                (("display-move", "5", "6"), ()),
                (("display-scroll", "3"), ()),
                (("display-scroll", "4", "-2"), ()),
                (("display-key", "Control_L+a"), ()),
                (("display-type",), ("hello world",)),
                (("display-state",), ()),
            ],
        )

    def test_nothing_is_asked_for_that_is_obviously_wrong(self):
        rec = Recorder()
        with mock.patch.object(server, "helix_msg", rec):
            self.assertTrue(server.computer_click(1, 2, "back").startswith("error"))
            self.assertTrue(server.computer_type("").startswith("error"))
            self.assertTrue(server.computer_key("  ").startswith("error"))
            self.assertTrue(server.computer_launch(" ").startswith("error"))
            self.assertTrue(server.computer_ask_for_help("").startswith("error"))
        self.assertEqual(rec.calls, [], "refused before anything was sent")

    def test_a_screenshot_is_returned_as_an_image_and_an_error_as_text(self):
        with tempfile.TemporaryDirectory() as folder:
            png = Path(folder) / "p.png"
            png.write_bytes(b"\x89PNG\r\n\x1a\n")
            reply = server.UNTRUSTED_NOTE + f"ok {png}"
            with mock.patch.object(server, "helix_msg", Recorder([reply])):
                image = server.computer_screenshot()
            self.assertEqual(type(image).__name__, "Image")
            self.assertEqual(Path(image.path), png)
        with mock.patch.object(server, "helix_msg", Recorder(["error: the agent display is not running"])):
            self.assertEqual(server.computer_screenshot(), "error: the agent display is not running")

    def test_asking_for_help_waits_until_the_person_is_done(self):
        waiting = json.dumps({"running": True, "help": "sign in", "windows": []})
        done = json.dumps({"running": True, "help": None, "windows": []})
        rec = Recorder(["ok", waiting, waiting, done])
        with mock.patch.object(server, "helix_msg", rec), mock.patch("time.sleep"):
            answer = server.computer_ask_for_help("sign in to the portal")
        self.assertIn("done", answer)
        self.assertEqual(rec.calls[0], (("display-help",), ("sign in to the portal",)))
        self.assertEqual([c[0] for c in rec.calls[1:]], [("display-state",)] * 3)

    def test_asking_for_help_gives_up_politely(self):
        waiting = json.dumps({"help": "x"})
        clock = iter([0.0, 1.0, 2.0, 100000.0])
        with mock.patch.object(server, "helix_msg", Recorder(["ok", waiting, waiting])), mock.patch("time.sleep"), mock.patch("time.monotonic", lambda: next(clock)):
            answer = server.computer_ask_for_help("x", wait_minutes=1)
        self.assertIn("Still waiting", answer)

    def test_the_tools_are_listed_and_allowed(self):
        names = {t for t in server.ALLOWLIST}
        for tool in ["computer_launch", "computer_screenshot", "computer_click", "computer_move", "computer_scroll", "computer_key", "computer_type", "computer_state", "computer_ask_for_help"]:
            self.assertIn(tool, names)
            self.assertTrue(callable(getattr(server, tool)))


if __name__ == "__main__":
    unittest.main()
