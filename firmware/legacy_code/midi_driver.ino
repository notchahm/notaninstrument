#include "midi_host_interface.h"
#include "midi_display.h"
#include "audio_output.h"
#include "audio_file.h"
#include "midi_driver.h"

// the setup function runs once when you press reset or power the board
void setup()
{
  initialize_midi_display();
  initialize_midi_host();
  initialize_audio_output();
  initialize_filesystem();
  pinMode(LED_PIN, OUTPUT);

  Serial1.println("Notchahm's Notaninstrument");
  display_text("Notchahm's\nNotaninstrument");
}


#if 0
uint64_t prev_time = 0;
uint64_t stream_end_time = 0;


bool play_note_from_flac(int active_index, uint8_t note_velocity, uint8_t note_index, uint8_t note_octave, int32_t* output_samples, int num_channels, int num_samples, int initial_file_offset=0)
{
  //Serial1.printf("PlayNoteFromWav(%d): key[%s:%d], velocity[%d], offset%d\r\n", active_index, midi_note_names[note_index], note_octave, note_velocity, initial_file_offset);
  
  FLACReader* p_reader = active_flac_files[active_index];
  if (p_reader)
  {
    int samples_read = p_reader->mix_samples(num_samples, num_channels, output_samples);
  }
  return false;
}

bool play_note_from_wav(int active_index, uint8_t note_velocity, uint8_t note_index, uint8_t note_octave, int32_t* output_samples, int num_channels, int num_samples, int initial_file_offset=0)
{
  //Serial1.printf("PlayNoteFromWav(%d): key[%s:%d], velocity[%d], offset%d\r\n", active_index, midi_note_names[note_index], note_octave, note_velocity, initial_file_offset);
  
  File file_handle = active_wav_files[active_index];
  uint8_t wav_buffer[num_samples * WAV_BYTES_PER_SAMPLE];
  int bytes_read = file_handle.read(wav_buffer, SAMPLES_PER_BUFFER * WAV_BYTES_PER_SAMPLE);
  int samples_read = bytes_read / WAV_BYTES_PER_SAMPLE;
  int16_t* p_sample_buffer = (int16_t*)wav_buffer;
  
  /*
  uint8_t* wav_buffer = active_wav_buffers[active_index];
  int active_buffer_size = active_wav_buffers_sizes[active_index];
  int samples_in_buffer = active_buffer_size / WAV_BYTES_PER_SAMPLE;
  int samples_read = num_samples;
  if (samples_in_buffer - (initial_file_offset/WAV_NUM_CHANNELS) < num_samples)
  {
      samples_read = samples_in_buffer - (initial_file_offset/WAV_NUM_CHANNELS);
  }
  int16_t* p_sample_buffer = (int16_t*)wav_buffer;
  p_sample_buffer += initial_file_offset;
  */

  //Serial1.printf("PlayNoteFromWav(%d): %d bytes, %d samples\r\n", active_index, bytes_read, samples_read);
  char display_buffer[64];
  sprintf(display_buffer, "PlayNoteFromWav(%d): %d bytes, %d samples\r\n", active_index, bytes_read, samples_read);
  display_text(display_buffer);

  bool clipping_flag = false;
  for (int sample_index=0; sample_index < samples_read; sample_index++)
  {
    for (int channel_index=0; channel_index < num_channels; channel_index++)
    {
      int sample_buffer_offset = num_channels * sample_index + channel_index;
      int output_buffer_offset = WAV_NUM_CHANNELS * sample_index + channel_index;
      int output_sample = output_samples[output_buffer_offset] + (int16_t)p_sample_buffer[sample_buffer_offset];
      if (output_sample >= WAV_MAX_AMPLITUDE/2)
      {
        // Clipping detected -- TODO: try some smoothing /lowpass filter
        output_sample = WAV_MAX_AMPLITUDE/2 - 1;
        clipping_flag = true;
      }
      if (output_sample < -WAV_MAX_AMPLITUDE/2)
      {
        output_sample = -WAV_MAX_AMPLITUDE/2;
        clipping_flag = true;
      }
      output_samples[output_buffer_offset] = output_sample;

    }
  }
  return clipping_flag;
}

bool synth_note(int active_index, uint8_t note_velocity, uint8_t note_index, uint8_t note_octave, int32_t* output_samples, int num_channels, int buffer_size, int initial_phase_offset=0)
{
  double amplitude = (WAV_MAX_AMPLITUDE * 0.4) * (double)note_velocity / 127.0;
  double decay_amount = 1.0;
  double decay_factor = 0.99999;
  //double decay_factor = 1.0;
  double frequency = midi_note_frequencies[note_index];
  for (int current_octave = 0; current_octave < note_octave; current_octave++)
  {
    frequency *= 2;
  }
  double sin_phase = (TWO_PI*frequency)/WAV_SAMPLE_RATE;
  //Serial1.printf("SynthNote(%d): frequency[%lf], amplitude[%lf], phase[%lf]\r\n", active_index, frequency, amplitude, sin_phase);
  bool clipping_flag = false;
  for (int sample_index=0; sample_index < buffer_size; sample_index++)
  {
    int phase_offset = sample_index + initial_phase_offset;
    double sample = (amplitude * decay_amount * sin(phase_offset * sin_phase));
    for (int channel_index=0; channel_index < WAV_NUM_CHANNELS; channel_index++)
    {
      int output_buffer_offset = WAV_NUM_CHANNELS * sample_index + channel_index;
      output_samples[output_buffer_offset] += (int32_t)sample;
    }
    if (sample_index % SAMPLES_PER_MS == 0)
    {
      decay_amount *= decay_factor;
    }
  }
  active_velocities[active_index] *= decay_amount;
  return clipping_flag;
}

void play_active_notes()
{
  // If we write to I2S directly from each midi note on event, it isn't very responsive and lacks support for polyphonic chords
  // The solution to this is to keep track of the state of each active note and combine waveforms for all active notes
  uint64_t current_time = time_us_64();
  if (current_time - prev_time < US_PER_BUFFER/4)
  {
    // don't process more than once per 10 ms
    return;
  }
  else if (current_time < stream_end_time)
  {
    // don't process again while streaming
    return;
  }

  int32_t output_samples[WAV_NUM_CHANNELS*SAMPLES_PER_BUFFER];
  memset(output_samples,0, sizeof(output_samples));
  bool write_flag = false;
  for (int active_index=0; active_index<MAX_ACTIVE_NOTES; active_index++)
  {
    if (active_notes[active_index] >= 0)
    {
      int key = active_notes[active_index];
      uint8_t note_velocity = (uint8_t)active_velocities[active_index];
      uint8_t note_index = key % 12;
      uint8_t note_octave = key / 12;
      uint64_t start_time = active_times[active_index];
      int start_offset = active_offsets[active_index];
      int offset_in_ms = start_offset / SAMPLES_PER_MS / WAV_NUM_CHANNELS;
      int elapsed_time_in_ms = (current_time - start_time) / 1000;
      if (start_offset == 0 || elapsed_time_in_ms > offset_in_ms - 10)
      {
        Serial1.printf("MIX(%d): key[%s:%d], velocity[%d], offset[%d(%d)] elapsed[%d] \r\n", active_index, midi_note_names[note_index], note_octave, note_velocity, start_offset, offset_in_ms, elapsed_time_in_ms);
        //char display_buffer[64];
        //sprintf(display_buffer, "MIX(%d): key[%s:%d], velocity[%d], offset[%d(%d)] elapsed[%d] \r\n", active_index, midi_note_names[note_index], note_octave, note_velocity, start_offset, offset_in_ms, elapsed_time_in_ms);
        //display_text(display_buffer);
        //if (start_offset < WAV_DATA_LENGTH)
        {

          //int end_offset = start_offset + SAMPLES_PER_BUFFER; // stream 20 ms each channel
          int end_offset = start_offset + (SAMPLES_PER_BUFFER*WAV_NUM_CHANNELS); // stream 20 ms each channel
          //digitalWrite(LED_PIN, HIGH);  // turn the LED on (HIGH is the voltage level)
          /*
          if (end_offset > WAV_DATA_LENGTH)
          {
            end_offset = WAV_DATA_LENGTH; // don't stream past end of wav file
          }
          */
          //bool clipping_flag = play_note_from_wav(active_index, note_velocity, note_index, note_octave, left_samples, right_samples, SAMPLES_PER_BUFFER, start_offset);
          bool clipping_flag = synth_note(active_index, note_velocity, note_index, note_octave, output_samples, SAMPLES_PER_BUFFER, start_offset/WAV_NUM_CHANNELS);
          //decay velocity
          active_offsets[active_index] = end_offset;
          //active_times[active_index] = current_time;
          write_flag = true;
        }
      }
      else
      {
        //Serial1.printf("SKIP(%d): key[%s:%d], velocity[%d], offset[%d] elapsed(%d/%d) \r\n", active_index, midi_note_names[note_index], note_octave, note_velocity, start_offset, offset_in_ms, elapsed_time_in_ms);
        //digitalWrite(LED_PIN, LOW);  // turn the LED off
      }
    }
  }
  if (write_flag == true)
  {
    //sound_out.write(samples, SAMPLES_PER_BUFFER);

    for (int sample_index=0; sample_index < SAMPLES_PER_BUFFER; sample_index++)
    {
      for (int channel_index=0; channel_index < WAV_NUM_CHANNELS; channel_index++)
      {
        int out_index = WAV_NUM_CHANNELS * sample_index + channel_index;
        sound_out.write(output_samples[out_index]);  //left channel
      }
    }
    if (stream_end_time == 0)
    {
      Serial1.printf("Stream Begin (%d)", current_time);
      stream_end_time = current_time;
    }
    else
    {
      stream_end_time += US_PER_BUFFER;
    }
  }
  else
  {
    if (stream_end_time != 0)
    {
      Serial1.printf("Stream End (%d)", current_time);
      stream_end_time = 0;
    }
  }
  prev_time = current_time;
}
#endif

void start_note(int channel, int key, int velocity)
{
  uint8_t note_index = key % 12;
  uint8_t octave = key / 12;
  //char display_buffer[64];
  //sprintf(display_buffer, "Channel:%d\nKey:%s%d\nVel:%d", channel, midi_note_names[note_index], octave, velocity);
  //display_text(display_buffer);
  display_note(channel_buffer, key_buffer, velocity);
  /*
  int active_index;
  int empty_index = -1;
  // search existing active notes for key, also keep track of first empty slot
  for (active_index=0; active_index<MAX_ACTIVE_NOTES; active_index++)
  {
    if (active_notes[active_index] == key)
    {
      break;
    }
    else if (empty_index < 0 && active_notes[active_index] < 0)
    {
      empty_index = active_index;
    }
  }
  if (active_index < MAX_ACTIVE_NOTES)
  {
    // note is already active, do nothing
  }
  else if (empty_index < 0)
  {
    // note is not active and thera are no empty slots, do nothing
  }
  else
  {
    active_index = empty_index;
    active_notes[active_index] = key;
    active_velocities[active_index] = velocity;
    active_offsets[active_index] = 0;
    if (true)
    {
      uint8_t note_velocity = (uint8_t)velocity;
      uint8_t note_index = key % 12;
      uint8_t note_octave = key / 12;

      uint8_t soundfont_velocity = note_velocity/8; //soundfont only has 16 velocity layers
      const char* note_name = midi_note_names[note_index];
      
      char flac_filename[64] = "GrandPiano/";
      sprintf(flac_filename, "GrandPiano/%s%dv%d.flac",note_name, note_octave, soundfont_velocity);
      int buffer_size = 0;

      //active_wav_files[active_index] = read_wav_file(wav_filename, &buffer_size);
      //active_wav_buffers[active_index] = read_wav_file(wav_filename, &buffer_size);
      active_wav_buffers_sizes[active_index] = buffer_size;
    }
    active_times[active_index] = time_us_64();
  }
  */
}

void stop_note(int channel, int key, int velocity)
{
  //display.setSegments(blank);
  //display.clearDisplay();
  //display.display();
  #if 0
  for (int active_index=0; active_index<MAX_ACTIVE_NOTES; active_index++)
  {
    if (active_notes[active_index] == key)
    {
      active_notes[active_index] = -1;
      active_velocities[active_index] = -1;
      /*
      if (active_wav_files[active_index])
      {
        active_wav_files[active_index].close();
      }
      if (active_wav_buffers[active_index] != NULL)
      {
        delete [] active_wav_buffers[active_index];
        active_wav_buffers[active_index] = NULL;
      }
      */
    }
  }
  #endif
}

void loop()
{
  handle_midi_events();
  //play_active_notes();
}

