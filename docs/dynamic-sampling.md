# Dynamic sampling: turn a live-recorded sound into a playable instrument

Status: stretch goal, not yet implemented. Shares its audio-input hardware
requirement with `docs/recording-and-looping.md` (the PCM1808 ADC) but is a
distinct capability — that doc loops and layers a *performance*; this one
builds a new *instrument* out of a single recording. Its output is a
regular `.nib` file, so it hands off directly into the routing/hot-swap
machinery `docs/multi-instrument-soundbanks.md` already defines.

## Goal

Record a real sound on the device itself — a guitar pluck, a voice, a
found sound, anything into the mic/line-in — and play it back chromatically
across the keyboard as a new instrument, entirely standalone: no computer,
no offline preprocessing step, no `tools/sfz_preprocessor` involved. The
classic hardware-sampler workflow (SP-404, MPC, the sampling half of the
EP-133 K.O. II — see `docs/vision.md` for how this project's goals compare
to that device), done as one of this project's own instrument sources
rather than a separate mode bolted on top.

## Why this doesn't need new plumbing for playback

The offline pipeline (`tools/sfz_preprocessor`) and this on-device path are
two different ways of arriving at the same destination: a `.nib` file
(`docs/multi-instrument-soundbanks.md`) sitting in a PSRAM slot, played by
the same voice pool and phase-accumulator mixer as everything else
(CLAUDE.md architecture decision #3). A dynamically-sampled instrument is
just a `.nib` file with one region instead of many — the routing, hot-swap,
and channel-assignment logic doesn't know or care whether a soundbank came
from an SD card written by a laptop or from the device's own microphone.
The new work here is entirely on the *recording and encoding* side, not the
playback side.

## Recording workflow

1. **Trigger.** A dedicated button, a menu action, or a MIDI CC from the
   controller (consistent with the transport-surface mapping in
   `docs/recording-and-looping.md`) arms recording.
2. **Capture.** Record raw PCM from the PCM1808 into a scratch PSRAM buffer
   for a bounded max duration (a few seconds — this is one sample, not a
   loop). Same I2S/DMA path as audio-in for looping, different consumer.
3. **Trim.** Detect and strip leading/trailing silence so the sample starts
   right at the transient, the way a real sampler does — otherwise every
   dynamically-sampled instrument has a dead-air delay before the note
   speaks.
4. **Encode.** Resample from the ADC's native rate down to this project's
   standard storage format (16-bit, project sample rate — see CLAUDE.md
   architecture decision #2) and write it as a single-region `.nib`:
   `key_lo`/`key_hi` spanning the full keyboard, one `root_key` (see below),
   no velocity layers (`vel_lo`/`vel_hi` spanning the full range), no loop
   points for v1 — one-shot playback with a release envelope, not a true
   sustain loop.
5. **Save & assign.** Write the file to `/soundbanks/` and offer to assign
   it to a channel via `notaninstrument.cfg`, the same as any other
   soundbank.

## Root key: the actual hard part

Everything else above is mechanical. Assigning the right root key — the
pitch the recording gets played back at unmodified, with other keys
pitch-shifting up/down from it — determines whether the result sounds like
a real instrument or an obviously-pitched-up-and-down toy. Two approaches,
not mutually exclusive:

- **Manual assignment**: play/sing the reference pitch while recording, and
  the user tells the device what note that was (or just picks a root key
  arbitrarily and accepts whatever the recording actually was for a
  deliberately toy-ish effect).
- **Automatic pitch detection**: run a pitch-detection algorithm (e.g.
  autocorrelation or YIN) on the captured audio to estimate its fundamental
  frequency and set the root key automatically. More convenient, adds real
  DSP work and won't be reliable on inharmonic or noisy sources (a drum hit
  has no clean pitch to detect) — probably worth offering manual override
  regardless of whether auto-detection is built.

## Honest limitation: this is not multi-sampling

A single recording pitch-shifted across the full keyboard range will sound
less natural than a properly multi-sampled instrument (Salamander piano's
16 velocity layers per key region, for contrast) — formants shift
unnaturally at pitch extremes, a real piano doesn't sound like one pitched
guitar sample. That's an inherent tradeoff of one-shot sampling, not a bug
to fix — it's the same tradeoff every classic hardware sampler makes, and
it's fine for what this rung is actually for (turn any sound into something
playable, on the spot) rather than a substitute for the pre-processed
instrument library in rung 2.

## Suggested phasing

Treat as a later addition, after `docs/recording-and-looping.md`'s MIDI
looping (capability 1) and audio-input hardware (capability 2) are both
working — this reuses that hardware path and adds an encoding step on top,
rather than being buildable in isolation first.

1. Recording + silence trim + manual root-key assignment + one-shot
   playback via a single-region `.nib` — proves the whole path end to end
2. Save/load through `notaninstrument.cfg` alongside pre-built soundbanks —
   proves this is really "just another soundbank," not a special case
3. Automatic pitch detection for root key, as a convenience layer on top of
   manual assignment, not a replacement for it
4. Basic loop-point detection for sustained sounds, if one-shot-with-release
   proves too limiting in practice

## Open questions

- Recording trigger UX in more detail — a dedicated hardware button implies
  a BOM change; a menu action or MIDI CC needs no new hardware but is
  slower to reach mid-performance
- Whether to support layering multiple recordings into one instrument
  (e.g. a few velocity-ish takes at different playing dynamics) or keep v1
  strictly single-recording-per-instrument
- Memory budget: scratch recording buffer + the resulting `.nib` both come
  out of the same PSRAM pool as soundbank slots, effect buffers
  (`docs/dsp-effects-chain.md`), and loop-recording buffers
  (`docs/recording-and-looping.md`) — another entry in that same
  cross-feature budget reconciliation, not a separate pool
- Whether pitch detection is worth the DSP budget given manual assignment
  already solves the problem, just less conveniently
