"""Merge the verified VIDAA Prime hotkey override into an existing JSON list.

Reads stdin, writes stdout. Preserve a backup before deploying. VIDAA's cloud
refresh can replace this file; this tool does not change that service.
"""
import json
import sys


def remap(entries):
    if not isinstance(entries, list) or any(not isinstance(x, dict) for x in entries):
        raise ValueError("Expected a list of app mapping objects")

    def prime(entry):
        value = entry.get("button")
        try:
            return int(str(value), 16 if str(value).lower().startswith("0x") else 10) == 0xF099
        except (TypeError, ValueError):
            return False

    return [x for x in entries if not prime(x)] + [{
        "button": "0xF099",
        "unifiedAppName": "moonlight-vidaa",
        # Empty overrides let AppControl use the installed app's launch URL
        # and store type. Forcing BROWSER=100 opens the generic browser.
        "videoPlayParam": "",
        "openMode": "",
    }]


if __name__ == "__main__":
    json.dump(remap(json.load(sys.stdin)), sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")
