// Libraries
#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
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
#define LED 2 

// AP Mode Configuration
#define AP_SSID "MideEase_Config"
#define AP_PASSWORD "12345678"
#define AP_CONFIG_TIMEOUT 300000 // 5 minutes timeout for configuration

// DNS and webserver for captive portal
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer webServer(80);

// Configuration variables
String configSSID = "";
String configPassword = "";
String configServerIP = "";
bool configComplete = false;

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
unsigned long startMicros;

bool isWIFIConnected = false;
volatile bool buttonPressed = false;

// Dynamic server URLs that will be updated based on configuration
String serverUploadUrl;
String serverBroadcastUrl;
String broadcastPermitionUrl;

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
void startConfigPortal();
void handleRoot();
void handleSave();
void handleNotFound();
void updateServerUrls();
bool tryHardcodedWifi();

void setup()
{
  Serial.begin(115200);
  delay(500);

  // Set up LEDs
  pinMode(isWifiConnectedPin, OUTPUT);
  digitalWrite(isWifiConnectedPin, LOW);
  pinMode(isAudioRecording, OUTPUT);
  digitalWrite(isAudioRecording, LOW);
  pinMode(LED,OUTPUT);
  digitalWrite(LED, LOW);


  // Set up button with interrupt
  pinMode(Button_Pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(Button_Pin), buttonInterrupt, FALLING);

  // Initialize SPIFFS
  SPIFFSInit();

  // Initialize I2S interfaces
  i2sInitINMP441();
  i2sInitMax98357A();

  // First try to connect using hardcoded credentials
  isWIFIConnected = tryHardcodedWifi();

  // If hardcoded connection fails, start configuration portal
  if (!isWIFIConnected)
  {
    startConfigPortal();

    // After configuration, connect to the configured WiFi
    if (configComplete)
    {
      isWIFIConnected = connectToWifi();
    }
  }
  else
  {
    // If hardcoded connection succeeds, set as configured
    configComplete = true;
    configSSID = WIFI_SSID;
    configPassword = WIFI_PASSWORD;
    // No need to set configServerIP as we'll use the static URL
  }

  // Update server URLs with the static URL
  updateServerUrls();

  Serial.println("Setup complete. Press button to start voice assistant.");
}

void updateServerUrls()
{
  // Use static URL instead of IP address
  String baseUrl = "http://hornet-upright-ewe.ngrok-free.app";
  serverUploadUrl = baseUrl + "/uploadAudio";
  serverBroadcastUrl = baseUrl + "/broadcastAudio";
  broadcastPermitionUrl = baseUrl + "/checkVariable";

  Serial.println("Server URLs updated:");
  Serial.println("Upload URL: " + serverUploadUrl);
  Serial.println("Broadcast URL: " + serverBroadcastUrl);
  Serial.println("Permission URL: " + broadcastPermitionUrl);
}

void startConfigPortal()
{
  Serial.println("Starting configuration portal...");

  // Set up AP mode
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", apIP);

  // Setup web server routes
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  Serial.println("Configuration portal started!");
  Serial.println("Connect to WiFi network: " + String(AP_SSID));
  Serial.println("Password: " + String(AP_PASSWORD));
  Serial.println("Then navigate to http://192.168.4.1 to configure");

  // Blink LED to indicate config mode
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(isWifiConnectedPin, HIGH);
    delay(100);
    digitalWrite(isWifiConnectedPin, LOW);
    delay(100);
  }

  // Wait for configuration or timeout
  unsigned long startTime = millis();
  while (!configComplete && (millis() - startTime < AP_CONFIG_TIMEOUT))
  {
    dnsServer.processNextRequest();
    webServer.handleClient();
    delay(10);
  }

  // If timed out without configuration, use defaults if available
  if (!configComplete)
  {
    Serial.println("Configuration portal timed out. Using default settings if available.");

// Check if config.h has WiFi credentials defined
#ifdef WIFI_SSID
    configSSID = WIFI_SSID;
    configPassword = WIFI_PASSWORD;
    configServerIP = "192.168.81.41"; // Default server IP
    configComplete = true;
#else
    Serial.println("No default settings available. Device may not function properly.");
#endif
  }

  // Stop AP mode properly
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);

  // Switch mode and prepare for station connection
  WiFi.mode(WIFI_OFF);
  delay(500);
  WiFi.mode(WIFI_STA);
  delay(100);
}

void handleRoot()
{
  String html = "<!DOCTYPE html><html><head><title>MideEase Configuration</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; }"
                ".container { max-width: 500px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
                "h1 { color: #333; text-align: center; margin-bottom: 20px; }"
                "label { display: block; margin-bottom: 5px; font-weight: bold; }"
                "input[type='text'], input[type='password'] { width: 100%; padding: 10px; margin-bottom: 20px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; }"
                "button { background-color: #4CAF50; color: white; border: none; padding: 10px 20px; text-align: center; font-size: 16px; border-radius: 5px; cursor: pointer; width: 100%; }"
                "button:hover { background-color: #45a049; }"
                ".hint { font-size: 12px; color: #666; margin-top: -15px; margin-bottom: 15px; }"
                "</style></head>"
                "<body><div class='container'>"
                "<h1>MideEase Configuration</h1>"
                "<form action='/save' method='post'>"
                "<label for='ssid'>WiFi Network Name:</label>"
                "<input type='text' id='ssid' name='ssid' placeholder='Enter your WiFi SSID' required>"
                "<label for='password'>WiFi Password:</label>"
                "<input type='password' id='password' name='password' placeholder='Enter your WiFi password' required>"
                "<div class='hint'>Password must be at least 8 characters</div>"
                "<button type='submit'>Save and Connect</button>"
                "</form></div></body></html>";
  webServer.send(200, "text/html", html);
}

void handleSave()
{
  if (webServer.hasArg("ssid") && webServer.hasArg("password"))
  {
    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");

    // Simple validation
    if (ssid.length() == 0 || password.length() < 8)
    {
      String errorHtml = "<!DOCTYPE html><html><head><title>Configuration Error</title>"
                         "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                         "<style>body {font-family: Arial; text-align: center;} "
                         ".container {max-width: 400px; margin: 0 auto; padding: 20px; border: 1px solid #ccc;}"
                         "h1 {color: #f44336;}</style></head>"
                         "<body><div class='container'>"
                         "<h1>Configuration Error</h1>"
                         "<p>Please ensure all fields are filled correctly:</p>"
                         "<ul style='text-align: left;'>";

      if (ssid.length() == 0)
        errorHtml += "<li>SSID cannot be empty</li>";
      if (password.length() < 8)
        errorHtml += "<li>Password must be at least 8 characters</li>";

      errorHtml += "</ul><p><a href='/'>Back to Configuration</a></p></div></body></html>";

      webServer.send(400, "text/html", errorHtml);
      return;
    }

    // Save the configuration
    configSSID = ssid;
    configPassword = password;
    // Server URL is now static, no need to collect it

    String html = "<!DOCTYPE html><html><head><title>Configuration Saved</title>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<style>"
                  "body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; text-align: center; }"
                  ".container { max-width: 500px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
                  "h1 { color: #4CAF50; }"
                  "p { margin-bottom: 20px; }"
                  ".loader { border: 5px solid #f3f3f3; border-top: 5px solid #4CAF50; border-radius: 50%; width: 50px; height: 50px; animation: spin 2s linear infinite; margin: 20px auto; }"
                  "@keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }"
                  "</style></head>"
                  "<body><div class='container'>"
                  "<h1>Configuration Saved!</h1>"
                  "<p>The device will now attempt to connect to your WiFi network.</p>"
                  "<div class='loader'></div>"
                  "<p>You can close this page.</p>"
                  "</div></body></html>";
    webServer.send(200, "text/html", html);

    Serial.println("Configuration received:");
    Serial.println("SSID: " + configSSID);

    configComplete = true;
  }
  else
  {
    webServer.send(400, "text/plain", "Missing required parameters");
  }
}

void handleNotFound()
{
  // Check if the request is for a recognized file type
  String uri = webServer.uri();
  if (uri.endsWith(".css") || uri.endsWith(".js") || uri.endsWith(".ico") || uri.endsWith(".png"))
  {
    // For web resources, return a 404
    webServer.send(404, "text/plain", "File not found");
  }
  else
  {
    // For all other requests, redirect to the main configuration page
    // This creates a proper captive portal experience
    String redirectUrl = "http://192.168.4.1/";
    webServer.sendHeader("Location", redirectUrl, true);
    webServer.send(302, "text/plain", "");
  }
}

void loop()
{
  // If WiFi is disconnected, try to reconnect
  if (isWIFIConnected && WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi connection lost. Reconnecting...");
    isWIFIConnected = connectToWifi();
    if (!isWIFIConnected)
    {
      digitalWrite(isWifiConnectedPin, LOW);
    }
  }

  if (buttonPressed && !workflowInProgress)
  {
    buttonPressed = false;
    workflowInProgress = true;
    handleVoiceAssistantWorkflow();
    workflowInProgress = false;
    // Add a delay after completing workflow to prevent immediate restart
    delay(1000);
  }
  delay(100);
}

void IRAM_ATTR buttonInterrupt()
{
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPressTime > debounceTime && !workflowInProgress)
  {
    buttonPressed = true;
    lastButtonPressTime = currentTime;
  }
}

bool connectToWifi()
{
  // Ensure WiFi is in station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.print("Connecting to WiFi: ");

  // Use the configured WiFi credentials
  if (configComplete)
  {
    Serial.println(configSSID);
    WiFi.begin(configSSID.c_str(), configPassword.c_str());
  }
  else
  {
    // Fallback to config.h credentials if configuration wasn't completed
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // Wait longer for connection - some networks take time
  int attempts = 0;
  const int maxAttempts = 30; // Increased from 20 to 30

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(isWifiConnectedPin, HIGH);
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  else
  {
    digitalWrite(isWifiConnectedPin, LOW);
    Serial.println("\nFailed to connect to WiFi!");
    Serial.println("SSID: " + (configComplete ? configSSID : String(WIFI_SSID)));
    Serial.println("Status code: " + String(WiFi.status()));
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
  unsigned long totalStart = millis();

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
  unsigned long t1 = millis();
  digitalWrite(LED, HIGH);
  recordAudio();
  Serial.printf("Time taken for recording: %lu ms\n", millis() - t1);

  // Upload to server
  
  t1 = millis();
  uploadFile();
  Serial.printf("Time taken for upload: %lu ms\n", millis() - t1);

  // Clean up recording file to save space
  if (SPIFFS.exists(audioRecordfile))
  {
    SPIFFS.remove(audioRecordfile);
  }

  // Wait for response and play it
  t1 = millis();
  waitForResponseAndPlay();
  Serial.printf("Time taken for wait + playback: %lu ms\n", millis() - t1);
  Serial.printf("Total workflow time: %lu ms\n", millis() - totalStart);

  // Clean up response file
  if (SPIFFS.exists(audioResponsefile))
  {
    SPIFFS.remove(audioResponsefile);
  }
  digitalWrite(LED, LOW);
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
  startMicros = micros(); // Start time
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
  Serial.printf("Free heap before upload: %u bytes\n", ESP.getFreeHeap());


  HTTPClient client;
  client.begin(serverUploadUrl);
  client.addHeader("Content-Type", "audio/wav");
  client.setTimeout(10000); // <-- wait up to 60 seconds
  // client.setReuse(true);              // optional: reuse the TCP connection
  delay(500);  // Give ESP some breathing time
  yield();     // Yield to WiFi task scheduler
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
  else {
    Serial.printf("Upload failed, error code: %d\n", httpResponseCode);
    if (httpResponseCode == -11) {
      Serial.println("Likely memory or timeout issue. Try increasing timeout or reducing file size.");
    }
  }

  file.close();
  client.end();
}

void waitForResponseAndPlay() {
  HTTPClient http;
  int maxAttempts = 30;
  bool responseReady = false;

  Serial.println("Waiting for server processing...");

  for (int i = 0; i < maxAttempts; i++) {
    http.begin(broadcastPermitionUrl);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      String payload = http.getString();
      if (payload.indexOf("\"ready\":true") > -1) {
        responseReady = true;
        break;
      }
    }
    http.end();
    delay(500);
  }

  if (!responseReady) {
    Serial.println("Server response timeout");
    return;
  }

  Serial.println("Playing response...");
  http.begin(serverBroadcastUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    i2s_zero_dma_buffer(MAX_I2S_NUM);

    WiFiClient *stream = http.getStreamPtr();
    size_t contentLength = http.getSize();  // Actual content size

    // Skip WAV header
    uint8_t header_buffer[44];
    int header_bytes_read = 0;
    while (header_bytes_read < 44) {
      int len = stream->read(header_buffer + header_bytes_read, 44 - header_bytes_read);
      if (len > 0) header_bytes_read += len;
    }

    uint8_t buffer[4096];
    size_t total_bytes_written = 0;

    while (total_bytes_written < contentLength - 44) {
      int len = stream->read(buffer, sizeof(buffer));
      if (len > 0) {
        size_t written;
        i2s_write(MAX_I2S_NUM, buffer, len, &written, portMAX_DELAY);
        total_bytes_written += written;
      } else {
        delay(10); // small wait for more data
      }
    }

    delay(500); // Let audio complete
    i2s_stop(MAX_I2S_NUM);
    i2s_zero_dma_buffer(MAX_I2S_NUM);
    i2s_start(MAX_I2S_NUM);
    Serial.printf("Audio playback completed (bytes: %d)\n", total_bytes_written);
  } else {
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  // Important: manually close the stream to prevent hanging
  http.getStreamPtr()->stop();
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

void i2sInitMax98357A() {
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = MAX_I2S_SAMPLE_RATE,
      .bits_per_sample = i2s_bits_per_sample_t(MAX_I2S_SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 4,
      .dma_buf_len = 1024,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_BCLK,
      .ws_io_num = I2S_LRC,
      .data_out_num = I2S_DOUT,
      .data_in_num = -1
  };

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
  header[24] = 0x80; // 16000 & 0xFF
  header[25] = 0x3E; // (16000 >> 8) & 0xFF
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
bool tryHardcodedWifi()
{
  // Try to connect with hardcoded WiFi credentials from config.h
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.print("Connecting to hardcoded WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Try to connect 3 times
  int attempts = 0;
  const int maxAttempts = 3;

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts)
  {
    delay(1000);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(isWifiConnectedPin, HIGH);
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  else
  {
    Serial.println("\nFailed to connect using hardcoded credentials.");
    return false;
  }
}