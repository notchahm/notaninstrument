# DSP effects chain: reverb, delay, chorus, EQ, compression

Status: planned, not yet implemented. Builds on the voice manager/mixer
design (CLAUDE.md architecture decision #3) — this doc covers adding a
post-mix effects chain on top of the summed voice output.

## Goal

Add classic DSP effects on top of raw sample playback: reverb, delay,
chorus, multiband EQ, compression. Modeled on what the Teensy Audio Library
provides (`AudioEffectFreeverb`, `AudioEffectDelay`, `AudioEffectChorus`,
`AudioFilterBiquad` cascades for EQ, and the community `AudioEffectDynamics`
add-on for compression) — same category of algorithms, implemented directly
for this platform rather than pulled in as a library, since Teensy's Audio
Library targets Teensy's own audio graph/block-processing conventions.

## Architecture: post-mix insert, not per-voice

Effects run once on the already-summed stereo mix, immediately before the
I2S DMA feed — not once per active voice. Per-voice effects would multiply
cost by voice count for no real benefit here (16 voices each running their
own reverb would be both wasteful and would sound wrong — reverb is
naturally a shared-space effect, not a per-note one).

```
[voice mixer] -> [effects chain: EQ -> compressor -> chorus -> delay -> reverb] -> [I2S DMA]
```

Order above is a reasonable default (EQ shapes tone before compression reacts
to it; time-based effects like chorus/delay/reverb go last) but isn't fixed
— may want it user-adjustable eventually, not for v1.

**Per-channel EQ is an explicit non-goal for v1.** A separate filter chain
per channel-slot (so piano and drums can be tonally shaped differently
before summing) is more flexible but doubles/triples effect state for
comparatively little payoff at this stage. Master-bus-only until there's a
concrete reason to split it.

## Effects, in order of implementation cost (cheapest first)

### 1. EQ (biquad cascade)
Not a single "multiband EQ" object — a cascade of 2-3 biquad filter stages
(low-shelf, one or two peaking bands, high-shelf), same approach every
simple EQ actually uses internally. Near-free per sample: a handful of
multiply-adds per stage. Build this first; it's also useful on its own for
correcting tonal issues in the compressed/downsampled soundbank before any
other effect is even in place.

### 2. Delay
A ring buffer with feedback and a wet/dry mix. Buffer size determines max
delay time (e.g. 1 second at 32kHz mono = 64KB, less for shorter delays).
Cheap per sample; the main cost is the buffer allocation, which competes
with PSRAM budget like everything else in this project.

### 3. Compression
Needs an envelope follower (attack/release smoothing of the signal level)
feeding a gain computer (reduce gain above a threshold, by a ratio). A few
extra operations per sample beyond EQ, but not expensive. Reference:
Teensy's `AudioEffectDynamics` (MarkzP) or Chip Audette's
`AudioEffectCompressor_F32` for the standard structure (envelope follower ->
threshold/ratio/knee -> gain smoothing) to adapt from, not to reuse
directly.

### 4. Chorus
A few delay taps (2-4), each modulated by a slow LFO, mixed with the dry
signal. Cheap in CPU (a handful of interpolated delay-line reads per
sample) but needs its own small delay-line buffer per tap.

### 5. Reverb
The most expensive of the five, and the one most worth prototyping carefully
before committing memory. Freeverb-style designs use ~8 comb filters plus
~4 allpass filters per channel, each needing its own delay-line buffer —
tens of KB up to a couple hundred KB total depending on target room size and
sample rate. This is real CPU (dozens of delay-line reads/writes and
multiply-adds per sample) and real memory, competing directly with the
soundbank PSRAM budget from architecture decision #2. Budget this
explicitly rather than adding it last-minute; may want a deliberately
smaller/cheaper reverb (fewer comb/allpass stages, shorter buffers) than a
full Freeverb port, given everything else sharing PSRAM.

## Memory and CPU budget considerations

- Every effect with a delay line (delay, chorus, reverb) needs a buffer
  sized in proportion to its time parameter — these come out of the same
  PSRAM pool as soundbank slots (see
  `docs/multi-instrument-soundbanks.md`). Worth deciding an effects-memory
  budget alongside the soundbank-slot budget, not as an afterthought once
  soundbank sizing is finalized.
- CPU-wise, this runs on top of whatever the voice mixer already costs per
  I2S callback — the P4's FPU/DSP-extended cores give real headroom here
  (per the reasoning that favored this board for audio work in the first
  place), but actual budget should be measured, not assumed, once the mixer
  itself is running (see bring-up plan phase 7).

## Suggested phasing

Treat as a phase-10 addition to `docs/bring-up-plan.md`, after
multi-instrument soundbanks (phase 9) or in parallel with it — effects
don't depend on multi-instrument support, but both are "after core
single-instrument polyphony is solid" additions, so sequencing between them
is a scheduling choice, not a technical dependency.

1. EQ (biquad cascade) — cheapest, also useful for correcting the
   compressed soundbank's tone
2. Delay
3. Compression
4. Chorus
5. Reverb — prototype on a computer first (a quick script or plugin) to
   settle on comb/allpass count and buffer sizes before committing PSRAM on
   device

## Open questions

- Whether effect parameters (reverb room size, delay time, EQ bands) are
  fixed at build time, configurable via `notaninstrument.cfg`, or
  controllable live via MIDI CC — CC control would be the most flexible and
  matches how real synths expose this, but adds another layer of runtime
  state to manage
- Whether a smaller custom reverb (fewer stages than Freeverb) is worth
  designing from scratch versus accepting Freeverb's exact buffer/CPU cost
- Whether per-channel EQ becomes worth it once multi-instrument soundbanks
  (phase 9) are in — revisit after that phase, don't decide now

