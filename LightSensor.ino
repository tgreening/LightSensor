/* Photocell simple testing sketch.

  Connect one end of the photocell to 5V, the other end to Analog 0.
  Then connect one end of a 10K resistor from Analog 0 to ground
  Connect LED from pin 11 through a resistor to ground
  For more information see http://learn.adafruit.com/photocells */
#include <DS1307RTC.h>   // https://github.com/PaulStoffregen/DS1307RTC
#include <Timezone.h>    // https://github.com/JChristensen/Timezone
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ArduinoOTA.h>
#include <Wire.h>
#include <ESP8266mDNS.h>

// US Eastern Time Zone (New York, Detroit)
TimeChangeRule myDST = {"EDT", Second, Sun, Mar, 2, -240};    //Daylight time = UTC - 4 hours
TimeChangeRule mySTD = {"EST", First, Sun, Nov, 2, -300};     //Standard time = UTC - 5 hours
Timezone myTZ(myDST, mySTD);

// If TimeChangeRules are already stored in EEPROM, comment out the three
// lines above and uncomment the line below.
//Timezone myTZ(100);       //assumes rules stored at EEPROM address 100

TimeChangeRule *tcr;        //pointer to the time change rule, use to get TZ abbrev
time_t local, utc;

const unsigned long READ_MILLIS = 60000UL;
const unsigned int POST_COUNT = 15;
const char* thingSpeakserver = "api.thingspeak.com";
const char* host = "LightSensor";

int photocellPin = A0;     // the cell and 10K pulldown are connected to a0
int photocellReading;     // the analog reading from the sensor divider
int lowLightCount = 0;
int switchStatus = 0;
char apiKey[16];

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
  WiFiManager wifiManager;
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
    html += "<br>Last Reading: ";
    html += lastReadingTime;
    html += "<br>Last Post Time: ";
    html += postCount;
    html += "<br>Switch Status: ";
    html += switchStatus;
    html += "<br>Low Light Count: ";
    html += lowLightCount;
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
          strcpy(apiKey, json["ThingSpeakWriteKey"]);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  WiFiManagerParameter custom_thingspeak_api_key("key", "API key", apiKey, 40);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_thingspeak_api_key);

  if (!wifiManager.autoConnect("LightSwitchAP", "brook13s")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  WiFi.hostname(host);
  if (!MDNS.begin(host)) {
    Serial.println("Error setting up MDNS responder!");
  }

  //read updated parameters
  strcpy(apiKey, custom_thingspeak_api_key.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["ThingSpeakWriteKey"] = apiKey;

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
  ArduinoOTA.setHostname("LightSwitch");
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
  setSyncProvider(RTC.get);   // the function to get the time from the RTC
  if (timeStatus() != timeSet)
    Serial.println("Unable to sync with the RTC");
  else
    Serial.println("RTC has set the system time");
  ArduinoOTA.begin();
  httpServer.begin();
  Serial.println("Ready");
}

void loop(void) {
  if ((long) (millis() - lastReadingTime) >= 0) {
    local = now();
    lastReadingTime += READ_MILLIS;
    photocellReading = analogRead(photocellPin);
    Serial.print("Analog reading = ");
    Serial.println(photocellReading);     // the raw analog reading
    postCount++;
    if (hour(local) > 17 && hour(local) < 23 && photocellReading < 200  && switchStatus == 0) {
      lowLightCount++;
      if ( lowLightCount > 2 ) {
        switchStatus = 1;
      }
    }
    if (hour(local) > 22 && minute(local) > 15 ) {
      switchStatus = 0;
      lowLightCount = 0;
    }
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
  String data = String(apiKey).substring(0, 16) + "&field1=";
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
    client.print("X-THINGSPEAKAPIKEY: " + String(apiKey).substring(0, 16) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(data.length());
    client.print("\n\n");
    client.print(data);
  }
}

time_t time_when_compiled()
{
  char const *datestr = __DATE__;
  char const *timestr = __TIME__;

  char s_month[5];
  int year;
  tmElements_t t;
  static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

  sscanf(datestr, "%s %hhd %d", s_month, &t.Day, &year);
  sscanf(timestr, "%2hhd %*c %2hhd %*c %2hhd", &t.Hour, &t.Minute, &t.Second);

  // Find where is s_month in month_names. Deduce month value.
  t.Month = (strstr(month_names, s_month) - month_names) / 3 + 1;

  // year can be given as '2010' or '10'. It is converted to years since 1970
  if (year > 99) t.Year = year - 1970;
  else t.Year = year + 30;

  return makeTime(t);
}

