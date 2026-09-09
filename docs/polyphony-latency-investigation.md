# Polyphony latency investigation

Status: **resolved, 2026-09-07.** The dominant ~242ms chord-onset gap was
TinyUSB's own host-stack behavior on this ESP32-P4 DWC2 port (confirmed by
the native-library comparison below: 0-11ms on the same hardware). A
*residual* chording latency that remained after switching to the native
USB Host Library was traced to two things still in the audio path --
debugging + display updates in the hot path, and I2S output going through
a queued task rather than being rendered direct to DMA -- and both are
fixed; chording is clean now. See "Resolution" at the end for the final
state. This doc exists so the next round doesn't re-discover the same dead
ends.

## The symptom

Playing a chord (2-4 near-simultaneous Note On messages) produces audible
staggering between individual notes' onsets -- they sound one at a time
rather than together. Single notes have always triggered promptly. Not
yet reduced to a precise millisecond figure; "noticeable" is the best
current description.

## What's been tried, in order

1. **CPU core pinning** (`audio_output.c`'s audio task on core 1 high
   priority, `main.c`'s USB host task on core 0) -- ruled out scheduling
   contention as the cause. Helped general jitter, didn't fix chord
   staggering.
2. **Voice-pool spinlock** (`portMUX_TYPE` around `s_voices[]` in
   `voice_engine.c`) -- fixed a real, separate bug (stuck/interleaved
   notes from a genuine data race, made worse by the core-pinning above
   turning a latent race into an active one). Not the chord-latency cause
   either.
3. **`CFG_TUSB_DEBUG` left at 3** (`main/tusb_config.h`) -- **confirmed
   real, fixed**. TinyUSB's own internal host-stack logging (~65 call
   sites across `usbh.c`, `midi_host.c`, the DWC2 driver) was printing
   synchronously over blocking UART on nearly every low-level USB event,
   inside the same task that has to service the next incoming MIDI
   transfer. Set to 0. Meaningfully improved responsiveness.
4. **Our own `timing_log_record` diagnostic** (in `voice_engine.c`,
   since removed) -- was itself doing the exact same thing on a smaller
   scale (32 `ESP_LOGI` lines dumped synchronously every 32nd note-on).
   Removed once it had served its purpose.
5. **DMA buffer/queue depth** -- `I2S_CHANNEL_DEFAULT_CONFIG`'s defaults
   (`dma_desc_num=6`, `dma_frame_num=240`) meant up to 1440 frames (30ms
   at 48kHz) of already-decided audio could be queued ahead of any
   freshly rendered buffer. Reduced to `dma_desc_num=3`,
   `dma_frame_num=120` (the driver rounds this up to 128 for alignment)
   -- 384 frames (~8ms), close to Teensy Audio Library's own ~128-sample
   block size. Real, if partial, improvement.
6. **ISR-driven zero-copy rendering** -- tried and reverted, see below.

After all of the above: real, measured improvement, but chording still
has noticeable lag. The remaining cause is unidentified.

## The ISR attempt, and why it doesn't work here

Registered an `on_sent` callback via
`i2s_channel_register_event_callback()` and rendered directly into
`event->dma_buf` (the just-drained DMA buffer, a genuine zero-copy
handoff) instead of a separate FreeRTOS task blocking on
`i2s_channel_write()`. This is the standard pattern for low-latency
embedded audio -- it's how Teensy Audio Library (DMA half/full-complete
interrupt) and GBA Direct Sound (DMA-refilled FIFO, interrupt-swapped
double buffer) both work, and ESP-IDF's own docs describe it as the
tighter-latency alternative to a blocking-write loop.

**Confirmed on real hardware: this crashes immediately.** ESP-IDF's
RISC-V FreeRTOS port hard-aborts with `ERROR: Coprocessors must not be
used in ISRs!` (`freertos/FreeRTOS-Kernel/portable/riscv/port.c`) the
instant any FPU instruction executes inside interrupt context. This is a
deliberate platform design choice, not a config flag -- the port's lazy
per-task coprocessor-ownership tracking (`pxPortUpdateCoprocOwner`) has
no concept of an ISR "owning" the FPU at all, so any float math inside an
ISR is unconditionally fatal. `voice_engine_render` is floating-point
throughout (phase accumulation, pitch ratios, envelope, the
`1/sqrt(active_voices)` mix scaling), so this path is closed as-is.

Reverted to the task+blocking-write pattern, keeping the smaller
buffer/DMA-depth change from step 5 above since that part doesn't depend
on ISR-vs-task at all.

## The identified way to actually unlock ISR-driven rendering

Rewrite the hot per-sample path in fixed-point/integer arithmetic instead
of float -- there's real, long-standing precedent for exactly this,
predating the question of ISR-safety entirely: the Game Boy Advance,
SNES, and most tracker/chiptune engines used fixed-point phase
accumulators and integer mixing specifically because those platforms had
no FPU at all. Concretely, for this codebase:

- **Phase/phase_inc**: Q16.16 fixed-point (upper 16 bits = sample index,
  lower 16 bits = interpolation fraction) instead of `float`.
- **Linear interpolation**: `(frame0 * (65536 - frac) + frame1 * frac) >>
  16` -- integer multiply + shift, no `float` at all.
- **Envelope**: a 16-bit fixed-point gain (0-65535 = 0.0-1.0), decremented
  by an integer step per frame instead of `float -= rate`.
- **`1/sqrt(active_voices)` scaling**: `MAX_POLYPHONY` is 8, so
  `active_voices` only ever ranges 1-8 -- a precomputed 8-entry fixed-
  point lookup table replaces `sqrtf()` entirely, no runtime computation
  needed at all.
- **`powf()` for the pitch ratio**: only ever called once per `note_on`,
  which runs in task context (not the ISR) -- can stay as-is; only the
  per-sample render path needs to leave float behind.

This is a real, scoped rewrite (touches `voice_t`'s layout and every
function in `voice_engine.c` except `note_on`/`note_off`'s one-time
setup), not yet started as of this doc. Once done, the ISR-driven design
from the previous section becomes viable, since nothing in the hot path
would touch the FPU anymore.

## The fixed-point rewrite, completed

The rewrite described above was completed: `voice_engine.c`'s entire
per-sample hot path (`render_locked`, `read_voice_frame`, `advance_voice`,
`voice_engine_render`, `voice_engine_render_isr`, `find_voice_to_use`) is
now pure Q16.16/Q0.32 fixed-point integer arithmetic, verified FPU-free by
`objdump --disassemble=<fn>` on each function individually (a first attempt
at this via sed/regex extraction gave false "no float instructions"
results by silently truncating function bodies -- caught by cross-checking
against `voice_engine_note_on`, which legitimately still uses `powf()` for
the one-time pitch-ratio computation at note-on and should show real FPU
instructions; it initially, incorrectly, showed none, which is what
exposed the extraction bug).

With the hot path FPU-free, the ISR-driven zero-copy render from the
previous section became viable and was re-enabled: `audio_output.c`'s
`on_i2s_sent` callback calls `voice_engine_render_isr()` directly against
`event->dma_buf`, no task hand-off. One more real bug surfaced here:
`i2s_std_config`'s `auto_clear_after_cb` flag memsets the DMA buffer to
zero *immediately after* `on_sent` returns (confirmed by reading
`esp_driver_i2s/i2s_common.c` directly, not just the ambiguous header
comment) -- with ISR rendering, that wiped the audio this callback had
just written. Fixed by removing the flag.

Net effect: real, audible improvement to general responsiveness and
render jitter. **Chord onset lag remained, roughly unchanged, at
~235-243ms.** This ruled out float/ISR overhead as the (or a) cause of the
chord-specific gap -- the render path was no longer a plausible suspect at
all once it became interrupt-driven, integer-only, and no longer sharing
a scheduler with anything else.

## Isolating the gap to USB, not audio or MIDI parsing

With rendering ruled out, a decoupled timing diagnostic
(`main/timing_diag.c/h`) was added to `tuh_midi_rx_cb` itself -- critically,
*not* logging synchronously in the hot path (that was step 4's mistake,
repeated once more here before being caught again): a cheap counter
increment on every raw callback, a ring-buffer timestamp write on every
decoded Note On/Off, and all `ESP_LOGI` dumping deferred to a separate
low-priority task on the otherwise-idle core 1, polled every 500ms.

This confirmed the ~242ms gap is real (not a measurement artifact of
`ESP_LOGI` itself) and, more importantly, showed that **during the gap,
only 0-1 raw USB transfer-completion callbacks fire** -- the host driver
mostly isn't even attempting transfers during that window, not "attempting
and failing/retrying quickly." That pointed at host-side transfer
scheduling, not the device withholding data or a slow application-level
decode.

Also ruled out empirically, each with a real hardware test rather than by
inspection:
- **Controller-side arpeggiator**: same delay appears using the AKAI's own
  built-in sounds directly (bypassing USB entirely for playback, though
  still sending MIDI) -- rules out the controller itself pacing note
  output.
- **Second, unrelated controller** (Korg padKONTROL): same delay.
- **External USB hub / multi-device channel sharing**: same delay,
  unchanged, with a single controller plugged directly into the board's
  one working port (no hub in the path at all).

## The DWC2 periodic-endpoint hypothesis (investigated, then ruled out)

Reading the vendored TinyUSB fork's DWC2 host-controller driver
(`components/tinyusb_host/src/portable/synopsys/dwc2/hcd_dwc2.c`) found a
real, relevant asymmetry: `channel_xfer_in_retry()` retries a NAK'd BULK
IN transfer immediately, but a periodic (INTERRUPT or ISOCHRONOUS)
endpoint instead defers its retry via SOF-interrupt-based rescheduling
keyed to the endpoint's `bInterval` descriptor value
(`channel_is_periodic()` checks `ep_type == HCCHAR_EPTYPE_INTERRUPT ||
ep_type == HCCHAR_EPTYPE_ISOCHRONOUS`). A large `bInterval` on an
interrupt-type MIDI IN endpoint would fully and innocently explain a fixed
per-transfer delay like this, as a correct consequence of the device's own
descriptor rather than a bug.

This hypothesis assumed the MIDI IN endpoints were interrupt-type without
having actually checked -- an assumption inherited from nowhere in
particular, not from a real descriptor read. **It turned out to be wrong.**
The native-host-library test below (which parses and prints real endpoint
descriptors as a side effect of finding the MIDI interface) confirms the
AKAI MPK Mini Play mk3's MIDI Streaming interface uses **bulk** endpoints
on both directions (EP 0x02 OUT, EP 0x83 IN, `bmAttributes=0x2 BULK`,
`bInterval=0`) -- exactly per the USB-MIDI 1.0 spec's own expectation, and
also consistent with `spike-usb-host-native/README.md`'s original,
independent descriptor dump from the first bring-up pass. Bulk endpoints
have no `bInterval`-driven periodic retry path in DWC2 at all, so this
theory cannot explain the delay.

## The TinyUSB-vs-native comparison test

With the periodic-endpoint theory dead, the next step was to check whether
the ~242ms gap is specific to TinyUSB's driver stack at all, by swapping
in a completely different USB host stack talking to the same real
hardware and the same real controller, with nothing else changed.

`firmware/spike-usb-host-native/` already existed as a proven-working
fallback (ESP-IDF's first-party USB Host Library, `usb/usb_host.h` --
confirmed enumerating the same AKAI on real hardware back in the original
bring-up spike) but had never had MIDI *parsing* written for it, only
descriptor dumping. Added the minimum needed to make it a fair timing
comparison, without pulling in the full voice engine:

- `main/midi_native.c/h`: scans the device's config descriptor for an
  Audio-class/MIDIStreaming-subclass interface (`usb_parse_interface_descriptor`
  / `usb_parse_endpoint_descriptor_by_index`), claims it
  (`usb_host_interface_claim`), and keeps **exactly one bulk IN transfer
  perpetually in flight** -- resubmitted from inside its own completion
  callback the instant it completes, so there's no polling interval of our
  own choosing to confound the measurement. Decodes 4-byte USB-MIDI Event
  Packets directly in that callback (no separate parsing library, unlike
  TinyUSB's `midi_host.c`).
- `main/timing_diag.c/h`: the same decoupled counter+ring-buffer design as
  the TinyUSB spike's diagnostic, copied over unchanged (these are two
  separate ESP-IDF projects, not a shared component).
- `class_driver.c`: calls `midi_native_try_claim()` right after fetching
  the device's config descriptor.

Flashed to the same board, same working USB-A port, same AKAI MPK Mini
Play mk3, direct-connected (no hub). Real captured output, playing
repeated 3-note chords (notes 60/64/67):

```
I (6094849) timing_diag: note= 67 ON   +1339ms  (1 raw xfers since prev note)
I (6094849) timing_diag: note= 60 ON   +0ms  (1 raw xfers since prev note)
I (6094849) timing_diag: note= 64 ON   +4ms  (1 raw xfers since prev note)
I (6094859) timing_diag: note= 64 OFF  +220ms  (1 raw xfers since prev note)
I (6094869) timing_diag: note= 60 OFF  +30ms  (1 raw xfers since prev note)
I (6094869) timing_diag: note= 67 OFF  +0ms  (1 raw xfers since prev note)
I (6095379) timing_diag: note= 60 ON   +427ms  (1 raw xfers since prev note)
I (6095379) timing_diag: note= 67 ON   +7ms  (1 raw xfers since prev note)
I (6095379) timing_diag: note= 64 ON   +5ms  (1 raw xfers since prev note)
...
```

(full capture in session history; this pattern repeated consistently
across ~10 chord presses). The `+1339ms`, `+427ms`, etc. gaps are simply
the time *between* chord presses (human playing speed) -- the number that
matters is the gap *within* each chord, between its first and last note:
consistently **0-11ms**, not ~242ms. Note-off timing for the same chords
is equally tight (0-33ms). `raw xfers since prev note` staying at 1
throughout confirms this isn't a busier bus masking the same underlying
delay -- transfers are completing about as fast as notes arrive, on both
stacks; the difference is how long each stack takes to notice and act on a
completed transfer and get the next one back in flight.

**Conclusion: the ~242ms chord-onset gap is a TinyUSB-stack-specific
issue on this ESP32-P4 DWC2 port**, not a property of the MIDI device, the
endpoint type/descriptor, the audio render path, or USB bus contention.
The native USB Host Library, talking to the identical hardware, does not
reproduce it. The exact mechanism inside TinyUSB/DWC2 responsible for the
extra latency (something in bulk IN re-arming/re-queueing after a
transfer completes, most likely -- `channel_xfer_in_retry()`'s immediate
in-driver retry on NAK was read but its behavior *after a successful
completion*, i.e. how promptly the next transfer gets queued, was not yet
instrumented) has not been pinned down further, since a working
alternative now exists.

## Resolution (2026-09-07)

Chording is now resolved on the same real hardware/controller that
previously showed the staggering. Three things had to be true at once, and
all three hold in the current firmware (`firmware/notaninstrument-p4/`):

**1. The primary MIDI path switched to the native USB Host Library**
(option 1 above, now implemented in `usb_midi_host.c`). This is what
eliminated the dominant ~242ms *input-side* gap: the native library keeps
exactly one bulk IN transfer perpetually in flight, resubmitted from its
own completion callback the instant it completes, so a decoded note-on
reaches `usb_midi_on_event()` the moment the device sends it -- the 0-11ms
the comparison test predicted. TinyUSB's vendored fork is kept in-tree
(`components/tinyusb_host/`) as reference/fallback but is no longer built.
The multi-cable / jack-number parsing gap that was option 1's downside is
out of scope for now: the current parser is deliberately minimal
(single MIDIStreaming interface, first IN endpoint), which is all the
single-controller test set needs.

**2. Debugging and display updates were taken out of the audio hot path.**
The note-on/off handling runs in the USB transfer-completion context.
Debug/diagnostic logging there, and display work, delayed chord onset.
Display is now fully decoupled: `usb_midi_on_event()` does the
audio-critical `voice_engine_note_on/off` first, then a non-blocking
`xQueueSend` to a low-priority display task (`main.c`), so nothing
display-related runs in the hot path. The temporary chording diagnostics
(a `voice_diag` ring buffer in `voice_engine.c` logging each note-on/off's
voice allocation, and a `raw_diag` ring buffer in `usb_midi_host.c`
timestamping every raw USB-MIDI packet, each dumped by a low-priority
task) were used to confirm this and have since been removed.

**3. I2S output is rendered direct to DMA, not through a queued task.**
The rendered audio was being handed to a FreeRTOS task blocking on
`i2s_channel_write()` rather than written straight into the DMA buffer, so
a newly-triggered note's audio waited on that task being scheduled and
issuing the write -- onset latency and jitter. It's now rendered
direct-to-DMA: `audio_output.c`'s `on_i2s_sent` ISR callback calls
`voice_engine_render_isr()` straight into `event->dma_buf` (the
just-drained DMA buffer) -- the zero-copy, DMA-interrupt-driven pattern
the fixed-point rewrite above made possible. (The fixed-point section
recorded the ISR render as *enabled* at the time the ~242ms was measured;
that ~242ms was the *input-side* TinyUSB gap, a different axis from this
output-side queued-task hop -- which is why both had to be fixed for
chording to come out clean.)

With the input path on the native library, the hot path free of debug +
display work, and I2S output rendered direct-to-DMA in the ISR, **chording
is clean** on the same hardware that previously staggered.
