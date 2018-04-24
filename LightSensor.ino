/* Photocell simple testing sketch.

  Connect one end of the photocell to 5V, the other end to Analog 0.
  Then connect one end of a 10K resistor from Analog 0 to ground
  Connect LED from pin 11 through a resistor to ground
  For more information see http://learn.adafruit.com/photocells */
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ArduinoOTA.h>
#include <Wire.h>
#include <ESP8266mDNS.h>
#include <TimeLib.h>

const unsigned long READ_MILLIS = 60000UL;
const unsigned int POST_COUNT = 15;
const char* thingSpeakserver = "api.thingspeak.com";
const char* host = "lightsensor";
const char* ntpServerName = "time.nist.gov";
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
const String timeZone = "America/Detroit";
const int RELAY_PIN = D5;
const int SYNC_INTERVAL = 3600; //every hour sync the time

byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets
int photocellPin = A0;     // the cell and 10K pulldown are connected to a0
int photocellReading;     // the analog reading from the sensor divider
int lowLightCount = 0;
int switchStatus = 0;
char thingSpeakAPIKey[16];
char timeZoneAPIKey[16];
bool adjustmentMade = false;
unsigned long start;
ESP8266WebServer httpServer(80);

//flag for saving data
bool shouldSaveConfig = false;
long lastReadingTime = 0;
int postCount = 0;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}
void setup(void) {
  // We'll send debugging information via the Serial monitor
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  //read configuration from FS json
  Serial.println("mounting FS...");
  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body><b>Light Switch</b><br>";
    html += "Current Reading: ";
    html += photocellReading;
    html += "<br>Last Post Count: ";
    html += postCount;
    html += "<br>Switch Status: ";
    html += switchStatus;
    html += "<br>Low Light Count: ";
    html += lowLightCount;
    html += "<br>Time: ";
    html += String(month()) + "/" + String(day()) + "/" + String(year()) + " " + String(hour()) + ":" + String(minute());
    html += "<br>API Key: ";
    html += String(thingSpeakAPIKey);
    html += "<br>Uptime: ";
    html += uptimeString();
    html += "<br></body></html>";
    Serial.println("Done serving up HTML...");
    httpServer.send(200, "text/html", html);
  });

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          strlcpy(thingSpeakAPIKey, json["ThingSpeakWriteKey"] | "12345", sizeof(thingSpeakAPIKey));
          strlcpy(timeZoneAPIKey, json["TimezoneAPIKey"] | "ABCD", sizeof(timeZoneAPIKey));
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  WiFiManagerParameter custom_thingspeak_api_key("thingspeakapikey", "Thingspeak API key", thingSpeakAPIKey, 40);
  wifiManager.addParameter(&custom_thingspeak_api_key);

  WiFiManagerParameter custom_timezone_api_key("timezoneapikey", "TimezoneDB API key", timeZoneAPIKey, 40);
  wifiManager.addParameter(&custom_timezone_api_key);

  WiFi.hostname(String(host));
  wifiManager.setConfigPortalTimeout(120);
  if (!wifiManager.startConfigPortal(host)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  if (!MDNS.begin(host)) {
    Serial.println("Error setting up MDNS responder!");
  }

  //read updated parameters
  strcpy(thingSpeakAPIKey, custom_thingspeak_api_key.getValue());
  strcpy(timeZoneAPIKey, custom_timezone_api_key.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["ThingSpeakWriteKey"] = thingSpeakAPIKey;
    json["TimezoneAPIKey"] = timeZoneAPIKey;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }
  Serial.println("local ip");
  Serial.println(WiFi.localIP());
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  Wire.begin(D2, D1);
  setSyncProvider(setTime);
  setSyncInterval(SYNC_INTERVAL);
  setTime();
  if (timeStatus() != timeSet)
    Serial.println("Unable to sync with the RTC");
  else
    Serial.println("RTC has set the system time");
  start = now();
  Serial.print("Hour: ");
  Serial.println(hour(now()));
  ArduinoOTA.begin();
  httpServer.begin();
  Serial.println("Ready");
}

void loop(void) {
  if ((long) (millis() - lastReadingTime) >= 0) {
    lastReadingTime += READ_MILLIS;
    photocellReading = analogRead(photocellPin);
    Serial.print("Analog reading = ");
    Serial.println(photocellReading);     // the raw analog reading
    postCount++;
    if (hour() > 17 && hour() < 23 && photocellReading < 350  && switchStatus == 0) {
      lowLightCount++;
      if ( lowLightCount > 2 ) {
        switchStatus = 1;
        digitalWrite(RELAY_PIN, HIGH);
      }
    }
  }
  if (hour() >= 22 && minute() > 5 ) {
    switchStatus = 0;
    digitalWrite(RELAY_PIN, LOW);
    lowLightCount = 0;
  }

  if (postCount >= 30) {
    Serial.println("Updating checks...");
    postReading(photocellReading, switchStatus);
    postCount = 0;
  }
  ArduinoOTA.handle();
  httpServer.handleClient();
}

void postReading(int reading, int switchStatus) {
  String data = String(thingSpeakAPIKey).substring(0, 16) + "&field1=";
  data += reading;
  data += "&field2=";
  data += switchStatus;
  data += "\r\n\r\n";
  postToThingSpeak(data);
}

void postToThingSpeak(String data) {
  WiFiClient client;
  if (client.connect(thingSpeakserver, 80)) { // "184.106.153.149" or api.thingspeak.com

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + String(thingSpeakAPIKey).substring(0, 16) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(data.length());
    client.print("\n\n");
    client.print(data);
  }
}

String uptimeString() {
  long Day = 0;
  int Hour = 0;
  int Minute = 0;
  int Second = 0;
  long secsUp = now() - start;
  Second = secsUp % 60;
  Minute = (secsUp / 60) % 60;
  Hour = (secsUp / (60 * 60)) % 24;
  Day = secsUp / (60 * 60 * 24); //First portion takes care of a rollover [around 50 days]
  char buff[32];
  sprintf(buff, "%3d Days %2d:%02d:%02d", Day, Hour, Minute, Second);
  String retVal = String(buff);
  Serial.print("Uptime String: ");
  Serial.println(retVal);
  return retVal;
}


time_t setTime() {
  WiFiClient client;
  if (!client.connect("api.timezonedb.com", 80)) {
    Serial.println("connection failed");
    return 0;
  }
  //  String URL = "GET /v2/get-time-zone?key=" + timezoneKey + AY0H14VE80X2&format=json&by=zone&zone=" + timeZone
  client.print("GET /v2/get-time-zone?key=" + String(timeZoneAPIKey) + "&format=json&by=zone&zone=");
  client.print(timeZone);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: api.timezonedb.com\r\n");
  client.print("Connection: close\r\n");
  client.println();
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println(">>> Client Timeout !");
      client.stop();
      return 0;
    }
  }

  // Check HTTP status
  char status[32] = {0};
  client.readBytesUntil('\r', status, sizeof(status));
  if (strcmp(status, "HTTP/1.1 200 OK") != 0) {
    Serial.print(F("Unexpected response: "));
    Serial.println(status);
    return 0;
  }

  // Skip HTTP headers
  char endOfHeaders[] = "\r\n\r\n";
  if (!client.find(endOfHeaders)) {
    Serial.println(F("Invalid response"));
    return 0;
  }

  //skip extra characters
  if (client.available()) {
    client.readStringUntil('\r');
  }

  // Allocate JsonBuffer
  // Use arduinojson.org/assistant to compute the capacity.
  DynamicJsonBuffer jsonBuffer;
  // Parse JSON object
  JsonObject& root = jsonBuffer.parseObject(client);
  if (!root.success()) {
    Serial.println(F("Parsing failed!"));
    return 0;
  }
  return root["timestamp"].as<long>();

}
