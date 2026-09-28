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
    "rgb_01_solid_colors.gcode": {
        "time": 30,
        "desc": "Shows a fixed color on the strip.",
    },
    "rgb_02_blink_red.gcode": {
        "time": 60,
        "desc": "Blinks the strip red.",
    },
    "rgb_03_fade_white.gcode": {
        "time": 90,
        "desc": "Fades the strip through white.",
    },
    "rgb_04_rainbow.gcode": {
        "time": 120,
        "desc": "Cycles the strip through a rainbow.",
    },
    "rgb_05_machine_status.gcode": {
        "time": 15,
        "desc": "Shows machine status colors.",
    },
}
