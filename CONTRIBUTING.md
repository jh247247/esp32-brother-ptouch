# Contributing

Run `bash host/build.sh` before submitting a pull request. Keep protocol changes
fail-closed and add an independent fixture or invariant for every new command
recipe.

## Hardware validation reports

For an experimental profile, open an issue containing:

- exact printer model, USB VID/PID, firmware/driver version if shown;
- ESP32 board, ESP-IDF version, USB wiring/VBUS arrangement;
- tape width/type and whether the printer was in P-Lite/Editor Lite mode;
- complete structured result (`transfer_rc`, `completion_rc`,
  `stream_submitted`) and sanitized logs;
- photos or measurements confirming orientation, clipping, label length, cut,
  and any printed cut guides;
- one-label result first, then completion and chaining as separate claims.

Do not promote a recipe directly from documentation to hardware-validated.
Inspected output, completion, and chaining are distinct evidence gates.
