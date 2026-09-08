#include <I2S.h>
#include "flac.h"
#include "flac_reader.h"

#define I2S_BCLK 20
#define I2S_WS 21
#define I2S_DATA 22
#define I2S_XSMT 16
#define I2S_FMT 17
#define I2S_FLT 18

#define MAX_ACTIVE_NOTES 4
int active_notes[MAX_ACTIVE_NOTES] = {-1,-1,-1,-1};
int active_velocities[MAX_ACTIVE_NOTES] = {-1,-1,-1,-1};
int active_offsets[MAX_ACTIVE_NOTES] = {0,0,0,0};
uint64_t active_times[MAX_ACTIVE_NOTES] = {0,0,0,0};
File active_wav_files[MAX_ACTIVE_NOTES];
FLACReader* active_flac_files[MAX_ACTIVE_NOTES];
uint8_t* active_wav_buffers[MAX_ACTIVE_NOTES] = {0,0,0,0};
int active_wav_buffers_sizes[MAX_ACTIVE_NOTES] = {0,0,0,0};

I2S sound_out(OUTPUT, I2S_BCLK, I2S_DATA);
//#define WAV_BITS_PER_SAMPLE 24
//#define WAV_MAX_AMPLITUDE 16777216
const int WAV_BITS_PER_SAMPLE = 16;
const int WAV_NUM_CHANNELS = 2;
const int WAV_BYTES_PER_SAMPLE = WAV_BITS_PER_SAMPLE / 8 * WAV_NUM_CHANNELS;
const int WAV_MAX_AMPLITUDE = 65536;
const int WAV_SAMPLE_RATE = 44100;
const int SAMPLES_PER_MS = WAV_SAMPLE_RATE / 1000;
const int SAMPLES_PER_BUFFER = WAV_SAMPLE_RATE / 50;
const int US_PER_BUFFER = 1000000 / 50;

void initialize_audio_output()
{
	pinMode(I2S_BCLK, OUTPUT);
	pinMode(I2S_WS, OUTPUT);
	pinMode(I2S_DATA, OUTPUT);
	pinMode(I2S_XSMT, OUTPUT);
	pinMode(I2S_FMT, OUTPUT);
	pinMode(I2S_FLT, OUTPUT);  

	digitalWrite(I2S_XSMT, HIGH);  // Unmute
	digitalWrite(I2S_FMT, LOW);  // PCM format
	digitalWrite(I2S_FLT, LOW);  // Normal Latency

	sound_out.setBitsPerSample(WAV_BITS_PER_SAMPLE);
	if (!sound_out.begin(WAV_SAMPLE_RATE))
	{
		// FAILED TO INITIALIZE I2S!
		Serial1.println("FAILED TO INITIALIZE I2S!");
		//display_text("FAILED TO INITIALIZE I2S!");
	}
}

void play_sample_bytes(int32_t* output_samples, int num_channels, int num_samples)
{
	for (int sample_index=0; sample_index < SAMPLES_PER_BUFFER; sample_index++)
	{
		for (int channel_index=0; channel_index < WAV_NUM_CHANNELS; channel_index++)
		{
			int out_index = WAV_NUM_CHANNELS * sample_index + channel_index;
			sound_out.write(output_samples[out_index]);
		}
	}
}

