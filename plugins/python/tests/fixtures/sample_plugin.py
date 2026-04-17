"""Sample addon used by the loader integration test.

Registers a /sample command and an unload callback. Designed to be
dead simple so a single exec is enough to exercise the whole load
path: metadata discovery, hook registration with owner attribution,
and unload-callback delivery.
"""

import hexchat

__module_name__ = "sample"
__module_version__ = "0.1"
__module_description__ = "loader integration fixture"
__module_author__ = "tests"

_state = {"greetings": 0, "parted": False}


def _on_sample(word, word_eol, userdata):
    _state["greetings"] += 1
    hexchat.print(f"sample cmd heard: {word[1] if len(word) > 1 else '<no arg>'}")
    return hexchat.EAT_ALL


def _on_unload(userdata):
    _state["parted"] = True
    hexchat.print("sample saying goodbye")


hexchat.hook_command("sample", _on_sample)
hexchat.hook_unload(_on_unload)


def __module_deinit__():
    # Proves the loader invokes this function during unload, separately
    # from hook_unload callbacks.
    hexchat.print("sample __module_deinit__ called")
