#ifndef MIDI_DRIVER_H
#define MIDI_DRIVER_H

#define LED_PIN 25

#define SD_MISO 8
#define SD_MOSI 11
#define SD_CS   9
#define SD_SCK  10

const char* midi_message_types[] = 
{
  "Note Off",
  "Note On",
  "Key Pressure",
  "Controller Change",
  "Program Change",
  "Channel Pressure",
  "Pitch Bend"
};

const char* midi_note_names[] = 
{
  "C",
  "C#",
  "D",
  "D#",
  "E",
  "F",
  "F#",
  "G",
  "G#",
  "A",
  "A#",
  "B"
};

const double midi_note_frequencies[] = 
{
  8.175799,
  8.664957,
  9.177024,
  9.722718,
  10.300864,
  10.913383,
  11.562325,
  12.249857,
  12.978271,
  13.75,
  14.567617,
  15.433853
};

/*
const uint8_t segment_letters[] = {
	SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G,   // A
	SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,           // b
	SEG_A | SEG_D | SEG_E | SEG_F,                   // C
	SEG_B | SEG_C | SEG_D | SEG_E | SEG_G,           // d
	SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,           // E
	SEG_A | SEG_E | SEG_F | SEG_G,                   // F
	SEG_A | SEG_C | SEG_D | SEG_E | SEG_F,           // G
	SEG_A | SEG_B | SEG_F | SEG_G,                   // #
};

uint8_t segment_letters_note[][4] = {
	{segment_letters[2], 0, 0, 0},                   // C
	{segment_letters[2], segment_letters[7], 0, 0},  // C#
	{segment_letters[3], 0, 0, 0},                   // C
	{segment_letters[3], segment_letters[7], 0, 0},  // C#
	{segment_letters[4], 0, 0, 0},                   // E
	{segment_letters[5], 0, 0, 0},                   // F
	{segment_letters[5], segment_letters[7], 0, 0},  // F#
	{segment_letters[6], 0, 0, 0},                   // G
	{segment_letters[6], segment_letters[7], 0, 0},  // G#
	{segment_letters[0], 0, 0, 0},                   // A
	{segment_letters[0], segment_letters[7], 0, 0},  // A#
	{segment_letters[1], 0, 0, 0},                   // A
};

uint8_t data[] = { 0xff, 0xff, 0xff, 0xff };
uint8_t blank[] = { 0x00, 0x00, 0x00, 0x00 };

*/

#endif

