# sfz_preprocessor

Bring-up step 5 (`docs/bring-up-plan.md`): offline SFZ -> `.nib` converter.
Runs on a development machine, not the device -- the P4 never parses SFZ or
does string-based opcode interpretation at runtime (see
`docs/multi-instrument-soundbanks.md` for the full `.nib` format spec this
tool targets).

## What it expects

The **plain original** Salamander Grand Piano V3 distribution (Alexander
Holm, CC-BY), from
[archive.org](https://archive.org/details/SalamanderGrandPianoV3) --
`SalamanderGrandPianoV3_44.1khz16bit.tar.bz2` or the 48kHz/24bit variant.
Plain per-note velocity-layer regions (`sample=`/`lokey=`/`hikey=`/
`lovel=`/`hivel=`/`pitch_keycenter=`), no macros or keyswitches.

**Not** the [sfzinstruments/SalamanderGrandPiano GitHub
repackaging](https://github.com/sfzinstruments/SalamanderGrandPiano) --
that's a fan-made "ARIA extensions" version with `#define` macros,
keyswitch-selected tunings, and extra CC-triggered noise-layer regions
(hammer/pedal/string resonance) this parser doesn't handle and this
project doesn't model.

The parser also skips the plain original's own `harm*`-prefixed regions
(sympathetic-resonance layers) and `rel*`/`pedalD*`/`pedalU*` regions
(damper/pedal noise, CC64-triggered) -- filtered by requiring both
`pitch_keycenter` and `lovel` present and the sample name not starting
with `harm`, which cleanly selects just the primary struck-note samples.

## Why trim + loop at all

Salamander's raw note recordings are 16-21 seconds each (full natural
decay) -- 530 note regions at that length is 1GB+ raw, 30-50x this
project's 32MB PSRAM budget. This tool keeps a short attack + a
crossfaded loop region instead, so notes still sustain indefinitely while
held, and reduces velocity layers (16-19 in the source -> 4 by default,
re-mapping the survivors' vel_lo/vel_hi to still cover the full 1-127
range) and downsamples to 32kHz/16-bit. Default settings produce roughly
9MB for the full 88-key range -- comfortably single-digit MB.

## Usage

```
python3 -m venv venv && venv/bin/pip install -r requirements.txt

venv/bin/python3 sfz_to_nib.py \
  /path/to/SalamanderGrandPianoV3_44.1khz16bit/SalamanderGrandPianoV3.sfz \
  --wav-root /path/to/SalamanderGrandPianoV3_44.1khz16bit/44.1khz16bit \
  -o salamander_piano.nib
```

`--wav-root` is needed because the SFZ's own `sample=` paths use Windows
backslashes and assume a case/subdirectory layout that doesn't always
survive extraction cleanly cross-platform -- pointing directly at the
actual sample directory sidesteps that.

Tunable via flags: `--velocity-layers`, `--attack-seconds`,
`--loop-seconds`, `--crossfade-ms`, `--sample-rate`. Confirmed output
(defaults, full 88-key range): 116 regions (29 recorded pitches x 4
velocity layers), 8.91MB.

## Drum kit: `muldjordkit_to_nib.py`

Converts a local clone of
[sfzinstruments/DrumGizmo.MuldjordKit](https://github.com/sfzinstruments/DrumGizmo.MuldjordKit)
(CC-BY 4.0 -- see `CREDITS.md` at the repo root) into the `.nib` flashed to
the `drumkit` partition (built-in kit on MIDI channel 10). See its module
docstring for the full scope (single overhead mic pair, core 16 keys,
reduced velocity/round-robin, one-shot only, per-region peak
normalization to correct the raw overhead mic's very uneven per-piece
levels).

```
git clone https://github.com/sfzinstruments/DrumGizmo.MuldjordKit.git
venv/bin/python3 muldjordkit_to_nib.py DrumGizmo.MuldjordKit -o testdata/muldjordkit_drums.nib
```

`virtuosity_to_nib.py` (a different kit, CC0) remains in the repo as a
reference/fallback -- not the active builder.
