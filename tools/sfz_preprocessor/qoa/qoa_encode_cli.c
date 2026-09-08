// Minimal command-line wrapper around qoa.h's encoding primitives --
// there's no official QOA CLI shipped upstream (just the qoa.h library
// and a separate demo player), so this is purpose-built for
// sfz_to_nib.py.
//
// Takes a stereo 16-bit WAV and a `prime_samples` count. The first
// prime_samples samples are NOT encoded to QOA output at all (sfz_to_nib.py
// stores them separately as raw, uncompressed PCM -- see its module
// docstring's "hybrid" section) -- they're run through the real
// qoa_encode_frame() pipeline first (output discarded), purely to *prime*
// the QOA encoder's 4-tap LMS predictor before real encoding starts, so
// frame 0 of the real, output-producing encode loop below starts already
// adapted to this specific note instead of cold. This is possible because
// qoa_encode_frame() (the lower-level, exposed-in-qoa.h primitive), unlike
// the all-in-one qoa_encode(), never resets its qoa_desc's LMS state
// itself -- whatever state it's left in after priming carries straight
// into frame 0 below. Confirmed on real hardware that this (plus storing
// the prime span itself losslessly) fixes an audible click at every
// note's attack: QOA has no ADPCM-style "first sample of a block is
// stored directly" shortcut, so a predictor that's never seen this
// specific note before produces real, audible quantization error on a
// piano hammer strike's sharp transient.
//
// (qoa_encode() itself acknowledges a milder version of this same
// problem in its own source comment -- it seeds fresh LMS weights to a
// fixed {0, 0, -8192, 16384} specifically "to help with the prediction
// of the first few ms of a file" -- this goes further by adapting those
// weights on this specific note's own real content instead of using a
// one-size-fits-all static default.)
//
// Priming runs through qoa_encode_frame() itself, rather than driving
// the LMS predict/update recurrence directly with the exact (unquantized)
// prediction error, specifically to inherit that function's built-in
// "weights penalty" stability term (see its own comment: "prevents pops/
// clicks in certain problem cases"). An earlier version of this file did
// use the raw unquantized error, on the theory that lossless priming
// data should adapt the filter better than encoding it for real would --
// but a raw error during a sharp attack transient can be tens of
// thousands in magnitude (far larger than any *quantized* error
// qoa_encode_frame ever produces), and applying that directly to
// qoa_lms_update over a wide priming window drove the weights into an
// unstable regime, corrupting frame 0's decode into an audible runaway
// oscillation. Priming through the real, penalty-guarded encode path
// avoids that: the discarded output costs a little CPU at build time,
// nothing at runtime.
//
// Output is just the QOA frame bytes for the post-prime span -- no file
// header (magic + sample count), since sfz_to_nib.py already tracks
// sample_length itself and qoa_decode.c never needs to re-derive it from
// a container.
//
// QOA_SLICES_PER_FRAME is overridden at compile time (see this
// directory's qoa.h and sfz_to_nib.py's ensure_qoa_encoder_built()) to a
// much smaller frame than upstream's 256-slice default, so loop points
// can land close to a frame boundary without eating a large chunk of a
// short loop -- same reasoning as this project's ADPCM path snapping
// loop_start to a small block boundary.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "qoa.h"

static short *read_wav_stereo16(const char *path, unsigned int *out_samples, unsigned int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        exit(1);
    }

    unsigned int rate = 0, data_size = 0;
    unsigned short channels = 0, bits = 0;
    unsigned char *data = NULL;

    while (1) {
        unsigned char chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;
        unsigned int chunk_size = chunk_hdr[4] | (chunk_hdr[5] << 8) | (chunk_hdr[6] << 16) | ((unsigned int) chunk_hdr[7] << 24);
        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            fread(fmt, 1, 16, f);
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((unsigned int) fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            data_size = chunk_size;
            data = malloc(data_size);
            if (fread(data, 1, data_size, f) != data_size) {
                fprintf(stderr, "%s: truncated data chunk\n", path);
                exit(1);
            }
            break; // data is normally the last chunk we care about
        } else {
            fseek(f, chunk_size, SEEK_CUR); // skip unknown chunk (LIST, fact, etc.)
        }
    }
    fclose(f);

    if (!data || channels != 2 || bits != 16) {
        fprintf(stderr, "%s: expected a 16-bit stereo WAV (got %u channels, %u bits)\n", path, channels, bits);
        exit(1);
    }

    *out_samples = data_size / (2 * channels);
    *out_rate = rate;
    return (short *) data;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s in.wav out.qoaframes prime_samples\n", argv[0]);
        return 1;
    }

    unsigned int samples, rate;
    short *pcm = read_wav_stereo16(argv[1], &samples, &rate);
    unsigned int prime_samples = (unsigned int) atoi(argv[3]);
    if (prime_samples >= samples) {
        fprintf(stderr, "prime_samples (%u) must be less than total samples (%u)\n", prime_samples, samples);
        return 1;
    }
    unsigned int encode_samples = samples - prime_samples;

    qoa_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.channels = 2;
    desc.samplerate = rate;
    desc.samples = encode_samples;

    // Same starting point qoa_encode() itself uses, before priming
    // refines it with this note's own real content below.
    for (int c = 0; c < 2; c++) {
        desc.lms[c].weights[0] = 0;
        desc.lms[c].weights[1] = 0;
        desc.lms[c].weights[2] = -(1 << 13);
        desc.lms[c].weights[3] = (1 << 14);
        for (int i = 0; i < QOA_LMS_LEN; i++) {
            desc.lms[c].history[i] = 0;
        }
    }

    // Prime: run the priming span through the real qoa_encode_frame()
    // pipeline (output discarded into a scratch buffer), so frame 0 of
    // the real encode below starts already adapted to this specific
    // note instead of cold.
    //
    // This used to run the LMS predict+update recurrence directly with
    // the *exact* (unquantized) prediction error, on the theory that
    // lossless priming data should give a better-adapted filter than
    // encoding it for real would. In practice that bypassed a stability
    // safeguard qoa_encode_frame() applies to every real slice it
    // encodes: a "weights penalty" term (see its own comment, "prevents
    // pops/clicks in certain problem cases") that discourages scale
    // factors whose LMS update would blow the weights up. A raw,
    // unquantized error during a sharp attack transient (a piano
    // hammer strike, e.g.) can be tens of thousands in magnitude --
    // far larger than any *quantized* error qoa_encode_frame ever feeds
    // qoa_lms_update -- and repeatedly applying that over a wide
    // (50ms+) priming window drove the LMS weights into an unstable
    // regime. The result: frame 0 of the real encode started from
    // garbage weights and decoded into a growing runaway oscillation
    // right at the raw-PCM-prefix/QOA boundary -- confirmed by
    // simulating the firmware's exact decode path offline against the
    // real .nib file (see conversation/session notes) and finding the
    // divergence starts exactly there, worse the wider the prefix.
    // Routing priming through the same bounded, penalty-guarded update
    // real content always uses keeps the weights in the range the
    // codec was actually designed to remain stable in.
    unsigned int scratch_size = QOA_FRAME_SIZE(2, QOA_SLICES_PER_FRAME);
    unsigned char *scratch = malloc(scratch_size);
    for (unsigned int i = 0; i < prime_samples; i += QOA_FRAME_LEN) {
        int len = qoa_clamp(QOA_FRAME_LEN, 0, (int) (prime_samples - i));
        qoa_encode_frame(pcm + (size_t) i * 2, &desc, (unsigned int) len, scratch);
    }
    free(scratch);

    const short *encode_data = pcm + (size_t) prime_samples * 2;
    unsigned int num_frames = (encode_samples + QOA_FRAME_LEN - 1) / QOA_FRAME_LEN;
    unsigned int max_size = num_frames * (unsigned int) QOA_FRAME_SIZE(2, QOA_SLICES_PER_FRAME);
    unsigned char *out_buf = malloc(max_size);

    unsigned int p = 0;
    int frame_len = QOA_FRAME_LEN;
    for (unsigned int sample_index = 0; sample_index < encode_samples; sample_index += (unsigned int) frame_len) {
        frame_len = qoa_clamp(QOA_FRAME_LEN, 0, (int) (encode_samples - sample_index));
        const short *frame_samples = encode_data + (size_t) sample_index * 2;
        unsigned int frame_size = qoa_encode_frame(frame_samples, &desc, (unsigned int) frame_len, out_buf + p);
        p += frame_size;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "cannot open %s for writing\n", argv[2]);
        return 1;
    }
    fwrite(out_buf, 1, p, out);
    fclose(out);

    free(pcm);
    free(out_buf);
    return 0;
}
