#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#define FIRMWARE_VERSION "1.1.0"

WebServer server(80);

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* VERSION_URL = "https://raw.githubusercontent.com/USERNAME/REPO/main/firmware/version.json";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7200;
const int daylightOffset_sec = 3600;

unsigned long lastUpdateCheck = 0;
const unsigned long CHECK_INTERVAL = 60000;
bool updateCheckDone = false;
bool startupCheckDone = false;

String latestAvailableVersion = "";
bool updateAvailable = false;

// HTML για το web interface
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>OTA Update</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; margin: 20px; background: #f0f0f0; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; }
    .info { background: #e3f2fd; padding: 15px; border-radius: 5px; margin: 10px 0; }
    .button { background: #2196F3; color: white; padding: 15px 30px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; }
    .button:hover { background: #0b7dda; }
    .button.danger { background: #f44336; }
    .button.danger:hover { background: #da190b; }
    .status { padding: 10px; margin: 10px 0; border-radius: 5px; }
    .success { background: #d4edda; color: #155724; }
    .warning { background: #fff3cd; color: #856404; }
    .error { background: #f8d7da; color: #721c24; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🔄 OTA Update System</h1>
    
    <div class="info">
      <h3>📱 Device Info</h3>
      <p><strong>Device:</strong> %DEVICE%</p>
      <p><strong>Current Version:</strong> %VERSION%</p>
      <p><strong>IP Address:</strong> %IP%</p>
      <p><strong>Free Heap:</strong> %HEAP% bytes</p>
    </div>
    
    <div class="info">
      <h3>🌐 Update Status</h3>
      <p><strong>Latest Version:</strong> %LATEST%</p>
      <p><strong>Status:</strong> %STATUS%</p>
    </div>
    
    <div style="text-align: center;">
      <button class="button" onclick="checkUpdate()">🔍 Check for Updates</button>
      <button class="button" onclick="performUpdate()">⬇️ Update Now</button>
      <button class="button danger" onclick="if(confirm('Restart device?')) restart()">🔄 Restart</button>
    </div>
    
    <div id="message"></div>
  </div>
  
  <script>
    function checkUpdate() {
      document.getElementById('message').innerHTML = '<div class="status">Checking for updates...</div>';
      fetch('/check').then(r => r.text()).then(data => {
        document.getElementById('message').innerHTML = '<div class="status success">' + data + '</div>';
        setTimeout(() => location.reload(), 2000);
      });
    }
    
    function performUpdate() {
      if(confirm('Start OTA update? Device will restart after update.')) {
        document.getElementById('message').innerHTML = '<div class="status warning">Updating... DO NOT power off!</div>';
        fetch('/update').then(r => r.text()).then(data => {
          document.getElementById('message').innerHTML = '<div class="status success">' + data + '</div>';
        }).catch(err => {
          document.getElementById('message').innerHTML = '<div class="status error">Update failed: ' + err + '</div>';
        });
      }
    }
    
    function restart() {
      fetch('/restart').then(() => {
        document.getElementById('message').innerHTML = '<div class="status warning">Restarting... Page will reload.</div>';
        setTimeout(() => location.reload(), 10000);
      });
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== GitHub OTA Update System (Advanced) ===");
  Serial.print("Firmware Version: ");
  Serial.println(FIRMWARE_VERSION);
  
  connectWiFi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Αναμονή για NTP sync
  Serial.print("Syncing time");
  int retries = 0;
  while (time(nullptr) < 100000 && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println();
  
  // Setup web server
  setupWebServer();
  
  // Check για update στο startup
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Checking for updates on startup...");
    checkForUpdates();
    startupCheckDone = true;
  }
}

void loop() {
  server.handleClient();
  
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  
  if (millis() - lastUpdateCheck > CHECK_INTERVAL) {
    lastUpdateCheck = millis();
    checkMidnightUpdate();
  }
  
  delay(10);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/check", HTTP_GET, handleCheck);
  server.on("/update", HTTP_GET, handleUpdate);
  server.on("/restart", HTTP_GET, handleRestart);
  
  server.begin();
  Serial.println("Web server started");
  Serial.print("Access at: http://");
  Serial.println(WiFi.localIP());
}

void handleRoot() {
  String html = String(index_html);
  
  #if defined(ESP8266)
    html.replace("%DEVICE%", "ESP8266");
    html.replace("%HEAP%", String(ESP.getFreeHeap()));
  #elif defined(ESP32)
    html.replace("%DEVICE%", "ESP32");
    html.replace("%HEAP%", String(ESP.getFreeHeap()));
  #endif
  
  html.replace("%VERSION%", FIRMWARE_VERSION);
  html.replace("%IP%", WiFi.localIP().toString());
  html.replace("%LATEST%", latestAvailableVersion.length() > 0 ? latestAvailableVersion : "Not checked yet");
  html.replace("%STATUS%", updateAvailable ? "⚠️ Update available!" : "✅ Up to date");
  
  server.send(200, "text/html", html);
}

void handleCheck() {
  checkForUpdatesWeb();
  String response = updateAvailable ? 
    "New version available: " + latestAvailableVersion : 
    "Firmware is up to date!";
  server.send(200, "text/plain", response);
}

void handleUpdate() {
  if (updateAvailable) {
    server.send(200, "text/plain", "Starting update... Device will restart when complete.");
    delay(1000);
    performOTAUpdateWeb();
  } else {
    server.send(200, "text/plain", "No update available. Check for updates first.");
  }
}

void handleRestart() {
  server.send(200, "text/plain", "Restarting...");
  delay(1000);
  ESP.restart();
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
}

void checkMidnightUpdate() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  if (timeinfo->tm_hour == 0 && timeinfo->tm_min == 0 && !updateCheckDone) {
    Serial.println("Midnight - checking for updates...");
    checkForUpdates();
    updateCheckDone = true;
  }
  
  if (timeinfo->tm_hour != 0 || timeinfo->tm_min != 0) {
    updateCheckDone = false;
  }
}

void checkForUpdates() {
  checkForUpdatesWeb();
  if (updateAvailable) {
    Serial.println("Auto-updating...");
    performOTAUpdateWeb();
  }
}

void checkForUpdatesWeb() {
  Serial.println("\n=== Checking for updates ===");
  
  WiFiClientSecure client;
  #if defined(ESP8266)
    client.setInsecure();
  #elif defined(ESP32)
    client.setInsecure(); // Simplified για το παράδειγμα
  #endif
  
  HTTPClient https;
  
  if (https.begin(client, VERSION_URL)) {
    int httpCode = https.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      DynamicJsonDocument doc(1024);
      
      if (!deserializeJson(doc, payload)) {
        #if defined(ESP8266)
          latestAvailableVersion = doc["esp8266"]["version"].as<String>();
        #elif defined(ESP32)
          latestAvailableVersion = doc["esp32"]["version"].as<String>();
        #endif
        
        Serial.print("Current: ");
        Serial.print(FIRMWARE_VERSION);
        Serial.print(" | Latest: ");
        Serial.println(latestAvailableVersion);
        
        updateAvailable = (latestAvailableVersion != FIRMWARE_VERSION);
      }
    }
    https.end();
  }
}

void performOTAUpdateWeb() {
  WiFiClientSecure client;
  #if defined(ESP8266)
    client.setInsecure();
  #elif defined(ESP32)
    client.setInsecure();
  #endif
  
  HTTPClient https;
  if (https.begin(client, VERSION_URL)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      DynamicJsonDocument doc(1024);
      
      if (!deserializeJson(doc, payload)) {
        String firmwareURL;
        
        #if defined(ESP8266)
          firmwareURL = doc["esp8266"]["url"].as<String>();
          ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
          t_httpUpdate_return ret = ESPhttpUpdate.update(client, firmwareURL);
        #elif defined(ESP32)
          firmwareURL = doc["esp32"]["url"].as<String>();
          httpUpdate.setLedPin(LED_BUILTIN, LOW);
          t_httpUpdate_return ret = httpUpdate.update(client, firmwareURL);
        #endif
        
        Serial.println("Update complete - restarting...");
      }
    }
    https.end();
  }
}