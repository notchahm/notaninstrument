#include "sdio.h"
#include <FS.h>

void initialize_filesystem()
{
#ifdef ENABLE_SPI
  // Ensure the SPI pinout the SD card is connected to is configured properly
  SPI1.setRX(SD_MISO);
  SPI1.setTX(SD_MOSI);
  SPI1.setSCK(SD_SCK);
#endif
  if (!SD.begin(SD_CS, SPI1)) 
  {
    Serial1.println("SD initialization failed!");
    display_text("SD initialization failed!");
    //display.setSegments(data);
  }
  else
  {
    Serial1.println("SD initialization success!");
    display_text("SD initialization success!");
    //File root;
    //root = SD.open("/");
    //printDirectory(root, 0);
    //int buffer_size;
    //const char* filename = "GrandPiano/A5v10.wav";
    //read_wav_file(filename, &buffer_size);
  }

}

int read_wav_file(const char* filename, int* buffer_size)
{
    uint8_t* wav_data_buffer = NULL;
    /*
    File wav_file = SD.open(filename);
    if (wav_file)
    {
      Serial1.println(filename);

      // read from the file until there's nothing else in it:
      if (wav_file.available()) 
      {
        uint8_t wav_header_buffer[45];
        wav_file.read(wav_header_buffer, 44);
        wav_header_buffer[44] = '\0';
        uint16_t* p_num_channels = (uint16_t*)(wav_header_buffer + 22);
        uint32_t* p_sample_rate = (uint32_t*)(wav_header_buffer + 24);
        uint16_t* p_bits_per_sample = (uint16_t*)(wav_header_buffer + 34);
        uint32_t* p_data_size = (uint32_t*)(wav_header_buffer + 34);
        Serial1.println((char*)wav_header_buffer);
        Serial1.printf("num channels: %d, sample_rate: %d, bits_per_sample: %d, data_size: %d\r\n", *p_num_channels, *p_sample_rate, *p_bits_per_sample, *p_data_size);
        //wav_data_buffer = new uint8_t[*p_data_size];
        //wav_file.read(wav_data_buffer, *p_data_size);
        //wav_file.close();
        *buffer_size = *p_data_size;


        //Serial1.write(test_file.read());
      }
      //return wav_data_buffer;
      return wav_file;
    } 
    else 
    {
      // if the file didn't open, print an error:
      Serial1.printf("error opening %s\r\n", filename);
      return wav_file;
    }
    */
    return 0;
}

void printDirectory(File dir, int numTabs) 
{
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      // no more files
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      Serial1.print("  ");
    }
    Serial1.print(entry.name());
    if (entry.isDirectory()) {
      Serial1.println("/");
      printDirectory(entry, numTabs + 1);
    } else {
      // files have sizes, directories do not
      Serial1.print("    ");
      Serial1.print(entry.size(), DEC);
      Serial1.print("\n");
      /*
      time_t cr = entry.getCreationTime();
      time_t lw = entry.getLastWrite();
      struct tm* tmstruct = localtime(&cr);
      Serial1.printf("\tCREATION: %d-%02d-%02d %02d:%02d:%02d", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
      tmstruct = localtime(&lw);
      Serial1.printf("\tLAST WRITE: %d-%02d-%02d %02d:%02d:%02d\n", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
      */
    }
    entry.close();
  }
}

