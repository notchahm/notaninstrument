# Multi-instrument soundbanks: file format, channel routing, runtime swap

Status: planned, not yet implemented. Builds on the single-soundbank design in
CLAUDE.md (architecture decisions #1-#2) — this doc extends that to multiple
simultaneous instruments, user-suppliable libraries, and swapping at runtime
instead of only at boot.

## Goal

This is what makes the device a real instrument *library*, not a piano with
extra steps. Salamander Grand Piano is the first `.nib` file this project
builds, not the ceiling on what it can play — drums, guitar, strings, and
whatever else gets converted (or recorded, see `docs/dynamic-sampling.md`)
are equally first-class soundbanks, differing only in content.

Instead of one soundbank loaded once at boot, support:
- Multiple soundbank files on the SD card, each assignable to a MIDI channel
- Swapping a channel's instrument at runtime (via MIDI Program Change or an
  on-device menu), without a reboot or reflash
- A small built-in default (Salamander piano, heavily reduced) stored in
  flash, so the device works with no SD card present and as a fallback if a
  configured file is missing

## SD card layout

```
/soundbanks/
    salamander_piano.nib
    avl_drumkit.nib
    rhodes_ep.nib
    nylon_guitar.nib
    my_guitar_sample.nib      # built on-device, see docs/dynamic-sampling.md
    ...
/notaninstrument.cfg
```

## The `.nib` file format

"Notaninstrument Bank" — a fixed binary layout, not SFZ. The device never
parses SFZ or does string-based opcode interpretation at runtime; all of that
happens ahead of time in `tools/sfz_preprocessor`. At runtime this is a
header read, a binary search over a fixed-size region table, and a pointer
into a sample data blob.

### Header (fixed size, first N bytes)
| Field | Type | Notes |
|---|---|---|
| magic | 4 bytes | e.g. `"NIB1"` — also encodes format version |
| sample_rate | uint32 | e.g. 32000 |
| bit_depth | uint8 | 16 |
| channels | uint8 | 1 (mono) or 2 (stereo) |
| compression | uint8 | 0 = PCM, 1 = IMA ADPCM |
| region_count | uint16 | number of entries in the region table |
| display_name | 32 bytes | for on-device menu / OLED, null-padded |
| region_table_offset | uint32 | |
| sample_data_offset | uint32 | |

### Region table (one fixed-size record per note/velocity-layer mapping)
| Field | Type | Notes |
|---|---|---|
| key_lo, key_hi | uint8, uint8 | MIDI key range this region covers |
| vel_lo, vel_hi | uint8, uint8 | velocity range |
| root_key | uint8 | key the sample was recorded/tuned at |
| sample_offset | uint32 | byte offset into the sample data blob |
| sample_length | uint32 | in samples, not bytes |
| loop_start, loop_end | uint32, uint32 | 0/0 if the region doesn't loop |

### Sample data
Raw PCM or ADPCM, packed back-to-back, referenced only by offset/length from
the region table above. No per-sample framing or metadata — the region table
is the only source of truth for where each sample lives.

This is deliberately close to what the offline preprocessor already needs to
produce per architecture decision #2 (CLAUDE.md) — this doc just formalizes
it as a named, versioned format rather than an implementation detail, so the
preprocessor, the firmware, and (eventually) any third-party tooling agree on
the same layout.

## `notaninstrument.cfg`

Plain text, human-editable, sits at the SD card root.

```ini
[channel1]
file=soundbanks/salamander_piano.nib

[channel10]
file=soundbanks/avl_drumkit.nib

[programs]
0=soundbanks/salamander_piano.nib
1=soundbanks/rhodes_ep.nib
2=soundbanks/avl_drumkit.nib
```

- `[channelN]` sections set what loads into each MIDI channel's slot at boot.
- The optional `[programs]` section maps MIDI Program Change numbers to
  files — lets a controller's own patch buttons, or a DAW/sequencer, drive
  instrument swapping with no custom protocol needed on top of standard MIDI.
- Channels with no `[channelN]` entry, or whose file is missing/corrupt, fall
  back to the built-in ROM default (see below).

## PSRAM slot model

Fixed-size slots, not one dynamic allocation per instrument — avoids heap
fragmentation from repeated load/free cycles over a long uptime, and forces
an explicit, known-in-advance memory budget.

- Decide slot count up front (e.g. 4 or 8 simultaneous instruments)
- Divide the PSRAM budget across that many slots
- More slots = smaller budget per instrument = more aggressive
  compression/layer-reduction needed per soundbank (same trade-off math as
  architecture decision #2, just divided across N instruments instead of 1)

## Voice manager changes

- `Voice` struct gains a slot/channel reference so the mixer knows which
  soundbank's region table and sample data to read for that voice
- A small channel -> slot routing table, consulted by `start_note`/
  `stop_note`/a new `handle_program_change(channel, program_number)`
- `handle_program_change` triggers a slot reload (see below) rather than
  playing a note

## Runtime hot-swap

Loading a multi-MB file from SD takes real time (seconds on SDMMC) — must
not block the I2S audio callback for other channels while it happens.

Proposed split across the P4's two cores:
- **Core A**: MIDI handling, SD reads, decompression, writing into the
  target PSRAM slot
- **Core B**: purely services the I2S DMA callback, draining voice buffers —
  never blocks on SD or PSRAM writes

Swap sequence for a single channel:
1. Receive Program Change (or menu selection) for channel N
2. Let any voices currently active on channel N finish their release
   envelope (or fade quickly) — don't yank sample data out from under an
   active voice
3. Core A loads the new file into channel N's slot
4. Update the channel -> slot routing table
5. New notes on channel N now use the new instrument; other channels were
   never interrupted

## Built-in ROM default

A heavily-reduced Salamander piano (target: 1-2MB), stored in a separate
flash partition, not embedded in the firmware binary itself. Two purposes:
- Device works with no SD card present at all (useful for bring-up/testing
  before the SD path works, and as a demo mode)
- Automatic fallback if a configured channel's file is missing or fails to
  load, so a bad SD card doesn't silently kill a channel

Worth investigating: since flash is XIP-mappable on the P4, the ROM default
may be playable directly from flash without ever copying it into a PSRAM
slot, keeping the dynamic slot budget free for SD-loaded instruments.

## Relationship to `tools/sfz_preprocessor`

The preprocessor gets a second responsibility beyond SFZ -> `.nib`
conversion: writing/updating `notaninstrument.cfg` when the user assigns an
instrument to a channel or program number. Keeps the whole authoring
workflow (convert a library, assign it, done) in one tool rather than
requiring hand-edited binary files or a separate config utility.

## Suggested phasing

Treat as a phase-9 addition to `docs/bring-up-plan.md`, after single-
instrument polyphony (phases 6-7) is solid — voice-stealing, envelope
timing, and cross-core PSRAM swapping are each nontrivial; debugging all
three at once is harder than doing them in sequence.

1. Define and freeze the `.nib` format (v1) — even before multi-instrument
   support, this replaces whatever ad hoc format the single-soundbank
   version used
2. `notaninstrument.cfg` parsing + boot-time multi-slot loading (no runtime
   swap yet) — proves the slot model and channel routing
3. ROM default + fallback logic
4. Runtime hot-swap via Program Change, with the dual-core split above
5. On-device menu for manual browsing/assignment (optional, can follow later)

## Open questions

- Exact slot count vs. per-slot budget trade-off — depends on how many
  channels the intended use case actually needs simultaneously (piano +
  drums = 2 is the obvious minimum; is more needed?)
- Whether Bank Select (in addition to Program Change) is worth supporting
  for organizing a large SD library into banks, GM-style
- Corruption/validation strategy for `.nib` files — a checksum in the header
  would let the fallback logic detect a bad file rather than crashing on a
  malformed region table
- Whether the on-device menu (phase 5 above) is worth building at all if
  Program Change + `notaninstrument.cfg` cover the real use cases

