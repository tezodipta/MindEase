/**
 * @brief Voice assistant. Client side ESP32.
 * @author Yurii Mykhailov (modified)
 * @copyright GPLv3
 */

// Libraries
#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

// INMP441 Ports
#define I2S_WS 5   // LRC
#define I2S_SD 19  // DOUT
#define I2S_SCK 18 // BCLK

// MAX98357A Ports
#define I2S_DOUT 22 // DIN
#define I2S_BCLK 15 // BCLK
#define I2S_LRC 21  // LRC

// Wake-up Button
#define Button_Pin GPIO_NUM_4

// LED Ports
#define isWifiConnectedPin 25
#define isAudioRecording 32

unsigned long lastButtonPressTime = 0;
const unsigned long debounceTime = 500; // 500ms debounce
volatile bool workflowInProgress = false;

// MAX98357A I2S Setup
#define MAX_I2S_NUM I2S_NUM_1
#define MAX_I2S_SAMPLE_BITS (16)
#define MAX_I2S_READ_LEN (256)
#define MAX_I2S_SAMPLE_RATE (16000) // Changed from 12000 to match recording rate
// INMP441 I2S Setup
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE (16000)
#define I2S_SAMPLE_BITS (16)
#define I2S_READ_LEN (16 * 1024)
#define RECORD_TIME (5) // Seconds
#define I2S_CHANNEL_NUM (1)
#define FLASH_RECORD_SIZE (I2S_CHANNEL_NUM * I2S_SAMPLE_RATE * I2S_SAMPLE_BITS / 8 * RECORD_TIME)

File file;
const char audioRecordfile[] = "/recording.wav";
const char audioResponsefile[] = "/voicedby.wav";
const int headerSize = 44;

bool isWIFIConnected = false;
volatile bool buttonPressed = false;

// Node Js server Addresses
const char *serverUploadUrl = "http://192.168.1.34:3000/uploadAudio";
const char *serverBroadcastUrl = "http://192.168.1.34:3000/broadcastAudio";
const char *broadcastPermitionUrl = "http://192.168.1.34:3000/checkVariable";

// Function prototypes
void SPIFFSInit();
void listSPIFFS(void);
void i2sInitINMP441();
void i2sInitMax98357A();
void wavHeader(byte *header, int wavSize);
void I2SAudioRecord_dataScale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len);
void printSpaceInfo();
bool connectToWifi();
void buttonInterrupt();
void handleVoiceAssistantWorkflow();
void recordAudio();
void uploadFile();
void waitForResponseAndPlay();

void setup()
{
  Serial.begin(115200);
  delay(500);

  // Set up LEDs
  pinMode(isWifiConnectedPin, OUTPUT);
  digitalWrite(isWifiConnectedPin, LOW);
  pinMode(isAudioRecording, OUTPUT);
  digitalWrite(isAudioRecording, LOW);

  // Set up button with interrupt
  pinMode(Button_Pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(Button_Pin), buttonInterrupt, FALLING);

  // Initialize SPIFFS
  SPIFFSInit();

  // Initialize I2S interfaces
  i2sInitINMP441();
  i2sInitMax98357A();

  // Connect to WiFi
  isWIFIConnected = connectToWifi();

  Serial.println("Setup complete. Press button to start voice assistant.");
}

void loop()
{
  if (buttonPressed && !workflowInProgress) {
    buttonPressed = false;
    workflowInProgress = true;
    handleVoiceAssistantWorkflow();
    workflowInProgress = false;
    // Add a delay after completing workflow to prevent immediate restart
    delay(1000);
  }
  delay(100);
}

void IRAM_ATTR buttonInterrupt() {
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPressTime > debounceTime && !workflowInProgress) {
    buttonPressed = true;
    lastButtonPressTime = currentTime;
  }
}

bool connectToWifi()
{
  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(isWifiConnectedPin, HIGH);
    Serial.println("\nConnected to WiFi!");
    return true;
  }
  else
  {
    digitalWrite(isWifiConnectedPin, LOW);
    Serial.println("\nFailed to connect to WiFi!");
    return false;
  }
}

void SPIFFSInit()
{
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS initialization failed!");
    while (1)
      yield();
  }
  Serial.println("SPIFFS initialized");

  // List files
  listSPIFFS();
}

void handleVoiceAssistantWorkflow()
{
  // Check WiFi and reconnect if needed
  if (WiFi.status() != WL_CONNECTED)
  {
    if (!connectToWifi())
    {
      Serial.println("Cannot proceed without WiFi connection");
      return;
    }
    isWIFIConnected = true;
  }

  // Clean up any existing files
  if (SPIFFS.exists(audioRecordfile))
  {
    SPIFFS.remove(audioRecordfile);
  }
  if (SPIFFS.exists(audioResponsefile))
  {
    SPIFFS.remove(audioResponsefile);
  }

  // Prepare recording file
  file = SPIFFS.open(audioRecordfile, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }

  // Write WAV header
  byte header[headerSize];
  wavHeader(header, FLASH_RECORD_SIZE);
  file.write(header, headerSize);

  // Record audio
  recordAudio();

  // Upload to server
  uploadFile();

  // Clean up recording file to save space
  if (SPIFFS.exists(audioRecordfile))
  {
    SPIFFS.remove(audioRecordfile);
  }

  // Wait for response and play it
  waitForResponseAndPlay();

  // Clean up response file
  if (SPIFFS.exists(audioResponsefile))
  {
    SPIFFS.remove(audioResponsefile);
  }

  Serial.println("Workflow completed. Ready for next button press.");
}

void recordAudio()
{
  digitalWrite(isAudioRecording, HIGH);
  Serial.println(" *** Get Ready to Speak *** ");

  // Initialize buffer for flushing
  int i2s_read_len = I2S_READ_LEN;
  size_t bytes_read;
  char *flush_buff = (char *)calloc(i2s_read_len, sizeof(char));

  // Flush the I2S buffer multiple times to clear any previous audio
  for (int i = 0; i < 5; i++)
  {
    i2s_read(I2S_PORT, (void *)flush_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
  }
  free(flush_buff);

  // Add a short delay before starting
  delay(500);

  Serial.println(" *** Recording Start *** ");

  int flash_wr_size = 0;
  char *i2s_read_buff = (char *)calloc(i2s_read_len, sizeof(char));
  uint8_t *flash_write_buff = (uint8_t *)calloc(i2s_read_len, sizeof(char));

  digitalWrite(isAudioRecording, HIGH);

  while (flash_wr_size < FLASH_RECORD_SIZE)
  {
    i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
    I2SAudioRecord_dataScale(flash_write_buff, (uint8_t *)i2s_read_buff, i2s_read_len);
    file.write((const byte *)flash_write_buff, i2s_read_len);
    flash_wr_size += i2s_read_len;
    Serial.printf("Sound recording %u%%\n", flash_wr_size * 100 / FLASH_RECORD_SIZE);
  }

  file.close();
  digitalWrite(isAudioRecording, LOW);

  free(i2s_read_buff);
  free(flash_write_buff);

  Serial.println("Recording completed");
  listSPIFFS();
}

void uploadFile()
{
  file = SPIFFS.open(audioRecordfile, FILE_READ);
  if (!file)
  {
    Serial.println("FILE IS NOT AVAILABLE!");
    return;
  }

  Serial.println("===> Upload FILE to Node.js Server");

  HTTPClient client;
  client.begin(serverUploadUrl);
  client.addHeader("Content-Type", "audio/wav");
  int httpResponseCode = client.sendRequest("POST", &file, file.size());
  Serial.print("httpResponseCode : ");
  Serial.println(httpResponseCode);

  if (httpResponseCode == 200)
  {
    String response = client.getString();
    Serial.println("==================== Transcription ====================");
    Serial.println(response);
    Serial.println("====================      End      ====================");
  }
  else
  {
    Serial.println("Server is not available");
  }

  file.close();
  client.end();
}

void waitForResponseAndPlay()
{
  HTTPClient http;
  int maxAttempts = 30;
  bool responseReady = false;

  Serial.println("Waiting for server processing...");

  // Poll server for response readiness
  for (int i = 0; i < maxAttempts; i++)
  {
    http.begin(broadcastPermitionUrl);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0)
    {
      String payload = http.getString();
      if (payload.indexOf("\"ready\":true") > -1)
      {
        responseReady = true;
        break;
      }
    }

    http.end();
    delay(500);
  }

  if (!responseReady)
  {
    Serial.println("Server response timeout");
    return;
  }

  // Get and play audio
  Serial.println("Playing response...");
  http.begin(serverBroadcastUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    // Clear the I2S buffer before starting playback
    i2s_zero_dma_buffer(MAX_I2S_NUM);

    WiFiClient *stream = http.getStreamPtr();
    // Skip WAV header (first 44 bytes) which might be causing issues
    uint8_t header_buffer[44];
    int header_bytes_read = 0;
    while (header_bytes_read < 44) {
      int len = stream->read(header_buffer + header_bytes_read, 44 - header_bytes_read);
      if (len > 0) {
        header_bytes_read += len;
      }
    }
    uint8_t buffer[1024]; // Smaller buffer for more frequent writes
    size_t total_bytes_written = 0;

    // Keep track of available data and handle it in chunks
    while (stream->connected())
    {
      size_t available_bytes = stream->available();
      if (available_bytes == 0)
      {
        // Small delay to allow data to arrive
        delay(10);
        continue;
      }

      // Read a chunk of data
      int len = stream->read(buffer, min(available_bytes, sizeof(buffer)));
      if (len > 0)
      {
        size_t bytes_written = 0;
        i2s_write(MAX_I2S_NUM, buffer, len, &bytes_written, 100);
        total_bytes_written += bytes_written;

        // Small delay to allow I2S to process
        delay(5);
      }
    }

    // Make sure all data is played before finishing
    delay(500);
    Serial.printf("Audio playback completed (bytes: %d)\n", total_bytes_written);
  }
  else
  {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void i2sInitINMP441()
{
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = i2s_bits_per_sample_t(I2S_SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = 0,
      .dma_buf_count = 64,
      .dma_buf_len = 1024,
      .use_apll = 1};

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  const i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = -1,
      .data_in_num = I2S_SD};

  i2s_set_pin(I2S_PORT, &pin_config);
}

void i2sInitMax98357A()
{
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = MAX_I2S_SAMPLE_RATE, // 16000 Hz
      .bits_per_sample = i2s_bits_per_sample_t(MAX_I2S_SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Try changing to ONLY_RIGHT if still too fast
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0, //ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4, //8,
      .dma_buf_len = 1024, //MAX_I2S_READ_LEN,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0};

  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_BCLK,
      .ws_io_num = I2S_LRC,
      .data_out_num = I2S_DOUT,
      .data_in_num = -1};//I2S_PIN_NO_CHANGE

  i2s_driver_install(MAX_I2S_NUM, &i2s_config, 0, NULL);
  i2s_set_pin(MAX_I2S_NUM, &pin_config);
  i2s_zero_dma_buffer(MAX_I2S_NUM);
}

void I2SAudioRecord_dataScale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len)
{
  uint32_t j = 0;
  uint32_t dac_value = 0;
  for (int i = 0; i < len; i += 2)
  {
    dac_value = ((((uint16_t)(s_buff[i + 1] & 0xf) << 8) | ((s_buff[i + 0]))));
    d_buff[j++] = 0;
    // Increase amplification factor from 256/2048 to improve volume
    d_buff[j++] = dac_value * 512 / 2048; // Double the amplification
  }
}

void wavHeader(byte *header, int wavSize)
{
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSize = wavSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 0x10;
  header[17] = 0x00;
  header[18] = 0x00;
  header[19] = 0x00;
  header[20] = 0x01;
  header[21] = 0x00;
  header[22] = 0x01;
  header[23] = 0x00;
  header[24] = 0x80;  // 16000 & 0xFF
  header[25] = 0x3E;  // (16000 >> 8) & 0xFF
  header[26] = 0x00;
  header[27] = 0x00;
  header[28] = 0x00; // 32000 & 0xFF
  header[29] = 0x7D; // (32000 >> 8) & 0xFF
  header[30] = 0x00; // (32000 >> 16) & 0xFF
  header[31] = 0x00;
  header[32] = 0x02;
  header[33] = 0x00;
  header[34] = 0x10;
  header[35] = 0x00;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(wavSize & 0xFF);
  header[41] = (byte)((wavSize >> 8) & 0xFF);
  header[42] = (byte)((wavSize >> 16) & 0xFF);
  header[43] = (byte)((wavSize >> 24) & 0xFF);
}

void listSPIFFS(void)
{
  // DEBUG
  printSpaceInfo();
  Serial.println(F("\r\nListing SPIFFS files:"));
  static const char line[] PROGMEM = "=================================================";

  Serial.println(FPSTR(line));
  Serial.println(F("  File name                              Size"));
  Serial.println(FPSTR(line));

  fs::File root = SPIFFS.open("/");
  if (!root)
  {
    Serial.println(F("Failed to open directory"));
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(F("Not a directory"));
    return;
  }

  fs::File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.print("DIR : ");
      String fileName = file.name();
      Serial.print(fileName);
    }
    else
    {
      String fileName = file.name();
      Serial.print("  " + fileName);
      // File path can be 31 characters maximum in SPIFFS
      int spaces = 33 - fileName.length(); // Tabulate nicely
      if (spaces < 1)
        spaces = 1;
      while (spaces--)
        Serial.print(" ");
      String fileSize = (String)file.size();
      spaces = 10 - fileSize.length(); // Tabulate nicely
      if (spaces < 1)
        spaces = 1;
      while (spaces--)
        Serial.print(" ");
      Serial.println(fileSize + " bytes");
    }

    file = root.openNextFile();
  }

  Serial.println(FPSTR(line));
  Serial.println();
  delay(1000);
}

void printSpaceInfo()
{
  size_t totalBytes = SPIFFS.totalBytes();
  size_t usedBytes = SPIFFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;

  Serial.print("Total space: ");
  Serial.println(totalBytes);
  Serial.print("Used space: ");
  Serial.println(usedBytes);
  Serial.print("Free space: ");
  Serial.println(freeBytes);
}