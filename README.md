# Notaninstrument

A standalone USB-MIDI polyphonic sampler — plug in any class-compliant
MIDI controller, get real multi-sampled instrument sound out, no computer
required.

See `CLAUDE.md` for full project context, architecture decisions, and the
current bring-up plan. Start there.

## Build verification

```
make test          # incremental build check of every firmware target
make test-clean    # full clean rebuild of every firmware target first
```

Compiles every firmware target (Arduino sketches via `arduino-cli`, the
ESP-IDF spike via `idf.py`) and checks the result against what's expected --
including treating `spike-usb-midi-arduino`'s known compile failure as an
expected failure that gets flagged if it ever changes. See
`firmware/test-builds.sh` and each `firmware/*/README.md` for detail.

