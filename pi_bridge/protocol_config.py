"""Per-protocol timer/step info shown on the TUNA display's run screen.

Edit this file to set how long each test takes - no restart needed, it's
re-read from disk every time a run starts. Key = the exact .gcode filename
as it appears on the "Choose protocol" screen (with the .gcode extension).

  "some_test.gcode": {
      "time": 90,                              # seconds - drives the countdown
      "steps": ["Warm up", "Run", "Cool down"], # optional - shown as "2 of 3" etc.
      "desc": "One line shown under the title", # optional
  },

An entry here always wins over any `; TUNA_TIME=` comment inside the
.gcode file itself. A protocol with no entry here (and no such comment)
still runs fine - the run screen just counts elapsed time up instead of
counting a known duration down, since there's nothing to count down from.
"""

PROTOCOLS = {
    "Protocol_1.gcode": {
        "time": 20000,  # ~5.5 hours - double check this is really intended
        "desc": "Bead Pickup Protocol.",
    },
    "Protocol_2.gcode": {
        "time": 10000,  # ~2.8 hours - double check this is really intended
        "desc": "Blinks the strip red.",
    },
    "Protocol_3.gcode": {
        "time": 90,
        "desc": "Fades the strip through white.",
    },
    "Protocol_4.gcode": {
        "time": 120,
        "desc": "Cycles the strip through a rainbow.",
    },
    "Protocol_5.gcode": {
        "time": 15,  # placeholder - set the real value
        "desc": "Shows machine status colors.",
    },
}
