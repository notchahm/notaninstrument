//#include <TM1637Display.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

// 7 Seg display object
//TM1637Display display(DISPLAY_CLK, DISPLAY_DIO);
// OLED display object
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
#define DISPLAY_CLK 26
#define DISPLAY_DIO 27

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);


void initialize_midi_display()
{
	pinMode(DISPLAY_CLK, OUTPUT);
	pinMode(DISPLAY_DIO, OUTPUT);
	if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
	{
		Serial.println(F("SSD1306 allocation failed"));
	}
}

void display_text(const char* text) 
{
	display.clearDisplay();

	display.setTextSize(2);             // Normal 1:1 pixel scale
	display.setTextColor(SSD1306_WHITE);        // Draw white text
	display.setCursor(0,0);             // Start at top-left corner
	display.println(F(text));
	display.display();
}

void display_note(int channel, int key, int velocity)
{
  /*
  uint8_t* p_segment_buffer = segment_letters_note[note_index];
  p_segment_buffer[2] = display.encodeDigit(octave);
  display.setSegments(p_segment_buffer);
  uint8_t brightness = velocity / 18;
  if (brightness < 0)
  {
    brightness = 0;
  }
  display.setBrightness(brightness);
  */

	char channel_buffer[64];
	char key_buffer[64];
	sprintf(channel_buffer, "Channel: %d", channel);
	sprintf(key_buffer, "%s%d", midi_note_names[note_index], octave);

	display.clearDisplay();
	display.setTextColor(SSD1306_WHITE);
	display.setTextSize(2);
	display.setCursor(0,0);             // Start at top-left corner
	display.println(F(channel));
	display.setTextSize(5);
	display.setCursor(0,16);             // Next Line
	display.println(F(key));
	display.fillRect(display.width()-8, display.height()-velocity/2, display.width()-1, display.height()-1, SSD1306_WHITE);
	display.display();
}

