/*
   Main .ino file
   Integrating SSD1306 display for single-core (Core 1) mining stats including temperature.
*/

#include <ArduinoJson.h>
#if defined(ESP8266)
    #include <ESP8266WiFi.h>
    #include <ESP8266mDNS.h>
    #include <ESP8266HTTPClient.h>
    #include <ESP8266WebServer.h>
#else
    #include <ESPmDNS.h>
    #include <WiFi.h>
    #include <HTTPClient.h>
    #include <WebServer.h>
#endif
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiClient.h>
#include <Ticker.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MiningJob.h"
#include "Settings.h"

#ifdef USE_LAN
  #include <ETH.h>
#endif

#if defined(WEB_DASHBOARD)
  #include "Dashboard.h"
#endif

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#if defined(ESP8266)
    #define CORE 1
    typedef ESP8266WebServer WebServer;
#elif defined(CONFIG_FREERTOS_UNICORE)
    #define CORE 1
#else
    #define CORE 2
#endif

#if defined(WEB_DASHBOARD)
    WebServer server(80);
#endif

namespace {
    MiningConfig *configuration = new MiningConfig(
        DUCO_USER,
        RIG_IDENTIFIER,
        MINER_KEY
    );

    float hashrateCore1 = 0.0;
    unsigned int sharesCore1 = 0;
    unsigned int difficultyCore1 = 0;
    float temperatureCore1 = 0.0;

    #ifdef USE_LAN
      static bool eth_connected = false;
    #endif

    void UpdateHostPort(String input) {
        DynamicJsonDocument doc(256);
        deserializeJson(doc, input);
        const char *name = doc["name"];
        configuration->host = doc["ip"].as<String>().c_str();
        configuration->port = doc["port"].as<int>();
        node_id = String(name);

        #if defined(SERIAL_PRINTING)
          Serial.println("Poolpicker selected the best mining node: " + node_id);
        #endif
    }

    String httpGetString(String URL) {
        String payload = "";
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;

        if (http.begin(client, URL)) {
          int httpCode = http.GET();

          if (httpCode == HTTP_CODE_OK)
            payload = http.getString();
          else
            #if defined(SERIAL_PRINTING)
               Serial.printf("Error fetching node from poolpicker: %s\n", http.errorToString(httpCode).c_str());
            #endif

          http.end();
        }
        return payload;
    }

    void SelectNode() {
        String input = "";
        int waitTime = 1;

        while (input == "") {
            #if defined(SERIAL_PRINTING)
              Serial.println("Fetching mining node from the poolpicker in " + String(waitTime) + "s");
            #endif
            input = httpGetString("https://server.duinocoin.com/getPool");
            
            delay(waitTime * 1000);
            waitTime *= 2;
            if (waitTime > 32)
                  waitTime = 32;
        }

        UpdateHostPort(input);
    }

    #ifdef USE_LAN
    void WiFiEvent(WiFiEvent_t event)
    {
      switch (event) {
        case ARDUINO_EVENT_ETH_START:
          #if defined(SERIAL_PRINTING)
            Serial.println("ETH Started");
          #endif
          ETH.setHostname("esp32-ethernet");
          break;
        case ARDUINO_EVENT_ETH_CONNECTED:
          #if defined(SERIAL_PRINTING)
            Serial.println("ETH Connected");
          #endif
          break;
        case ARDUINO_EVENT_ETH_GOT_IP:
          #if defined(SERIAL_PRINTING)
            Serial.println("ETH Got IP");
          #endif
          eth_connected = true;
          break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
          #if defined(SERIAL_PRINTING)
            Serial.println("ETH Disconnected");
          #endif
          eth_connected = false;
          break;
        case ARDUINO_EVENT_ETH_STOP:
          #if defined(SERIAL_PRINTING)
            Serial.println("ETH Stopped");
          #endif
          eth_connected = false;
          break;
        default:
          break;
      }
    }
    #endif

    void SetupWifi() {
      
      #ifdef USE_LAN
        #if defined(SERIAL_PRINTING)
          Serial.println("Connecting to Ethernet...");
        #endif
        WiFi.onEvent(WiFiEvent);  // Will call WiFiEvent() from another thread.
        ETH.begin();
        

        while (!eth_connected) {
            delay(500);
            #if defined(SERIAL_PRINTING)
                Serial.print(".");
            #endif
        }

        #if defined(SERIAL_PRINTING)
          Serial.println("\n\nSuccessfully connected to Ethernet");
          Serial.println("Local IP address: " + ETH.localIP().toString());
          Serial.println("Rig name: " + String(RIG_IDENTIFIER));
          Serial.println();
        #endif

      #else
        #if defined(SERIAL_PRINTING)
          Serial.println("Connecting to: " + String(SSID));
        #endif
        WiFi.mode(WIFI_STA); // Setup ESP in client mode
        #if defined(ESP8266)
            WiFi.setSleepMode(WIFI_NONE_SLEEP);
        #else
            WiFi.setSleep(false);
        #endif
        WiFi.begin(SSID, PASSWORD);

        int wait_passes = 0;
        while (WiFi.waitForConnectResult() != WL_CONNECTED) {
            delay(500);
            #if defined(SERIAL_PRINTING)
              Serial.print(".");
            #endif
            if (++wait_passes >= 10) {
                WiFi.begin(SSID, PASSWORD);
                wait_passes = 0;
            }
        }

        #if defined(SERIAL_PRINTING)
          Serial.println("\n\nSuccessfully connected to WiFi");
          Serial.println("Local IP address: " + WiFi.localIP().toString());
          Serial.println("Rig name: " + String(RIG_IDENTIFIER));
          Serial.println();
        #endif

      #endif

        SelectNode();
    }

    void SetupOTA() {
        // Prepare OTA handler
        ArduinoOTA.onStart([]()
                           { 
                             #if defined(SERIAL_PRINTING)
                               Serial.println("Start"); 
                             #endif
                           });
        ArduinoOTA.onEnd([]()
                         { 
                            #if defined(SERIAL_PRINTING)
                              Serial.println("\nEnd"); 
                            #endif
                         });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                              { 
                                 #if defined(SERIAL_PRINTING)
                                   Serial.printf("Progress: %u%%\r", (progress / (total / 100))); 
                                 #endif
                              });
        ArduinoOTA.onError([](ota_error_t error)
                           {
                                Serial.printf("Error[%u]: ", error);
                                #if defined(SERIAL_PRINTING)
                                  if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
                                  else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
                                  else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
                                  else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
                                  else if (error == OTA_END_ERROR) Serial.println("End Failed");
                                #endif
                          });

        ArduinoOTA.setHostname(RIG_IDENTIFIER); // Give port a name
        ArduinoOTA.begin();
    }

    void VerifyWifi() {
      #ifdef USE_LAN
        while ((!eth_connected) || (ETH.localIP() == IPAddress(0, 0, 0, 0))) {
          #if defined(SERIAL_PRINTING)
            Serial.println("Ethernet connection lost. Reconnect..." );
          #endif
          SetupWifi();
        }
      #else
        while (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0))
            WiFi.reconnect();
      #endif
    }

    void handleSystemEvents(void) {
        VerifyWifi();
        ArduinoOTA.handle();
        yield();
    }

    void displayStats() {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(F("Core 1: "));
      display.print(hashrateCore1, 2); // 2 decimal places
      display.println(F(" kH/s"));

      display.setCursor(0, 10);
      display.print(F("Diff: "));
      display.print(difficultyCore1);

      display.setCursor(0, 20);
      display.print(F("Shares: "));
      display.println(sharesCore1);

      // display.setCursor(0, 30);
      // display.print(F("Temp: "));
      // display.print(temperatureCore1);
      // display.println(F(" C"));

      display.setCursor(0, 40);
      display.print(F("WiFi: "));
      display.println(WiFi.isConnected() ? "Connected" : "Disconnected");

      display.display();
    }
}

MiningJob *job[1];

void task1_func(void *pvParameters) {
    while (true) {
        job[0]->mine();
        hashrateCore1 = hashrate / 1000.0; // Update core 1 hashrate
        difficultyCore1 = job[0]->getDifficulty(); // Update core 1 difficulty
        sharesCore1 = job[0]->getShares(); // Update core 1 shares
        temperatureCore1 = job[0]->getTemperature(); // Update core 1 temperature
        displayStats();
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to allow system events
    }
}

void setup() {
    delay(500);

    #if defined(SERIAL_PRINTING)
      Serial.begin(115200);
      Serial.println("\n\nDuino-Coin " + String(configuration->MINER_VER));
    #endif
    pinMode(LED_BUILTIN, OUTPUT);

    assert(CORE == 1 || CORE == 2);
    WALLET_ID = String(random(0, 2811));
    job[0] = new MiningJob(1, configuration);

    SetupWifi();
    SetupOTA();

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      Serial.println(F("SSD1306 allocation failed"));
      for (;;);
    }

    Serial.println(F("SSD1306 allocation successful"));

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("MMMMMMMMINE!!!"));
    display.display();
    delay(2000); // Delay to show the initialization message

    xTaskCreatePinnedToCore(
        task1_func,    // Function to implement the task
        "Task1",       // Name of the task
        4096,          // Stack size in words
        NULL,          // Task input parameter
        1,             // Priority of the task
        NULL,          // Task handle
        1);            // Core where the task should run
}

void loop() {
    // Empty loop since mining and display are handled in tasks
}

