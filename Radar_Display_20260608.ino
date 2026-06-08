#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

/*
  ESP32-S3 SuperMini + GC9A01 240x240 Flugzeug-Radar

  Libraries:
  - GFX Library for Arduino
  - ArduinoJson
  - WiFiManager by tzapu

  Verkabelung:
  GC9A01  -> ESP32-S3 SuperMini
  BLK     -> GPIO13
  CS      -> GPIO10
  DC      -> GPIO9
  RES     -> GPIO8
  SDA     -> GPIO11
  SCL     -> GPIO12
  VCC     -> 3V3
  GND     -> GND

  Erststart WLAN:
  SSID: ESP32-Radar-Setup
  PASS: radar1234
*/

#define TFT_BL    13
#define TFT_CS    10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_MOSI  11
#define TFT_SCLK  12

Arduino_DataBus *bus = new Arduino_ESP32SPI(
  TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, HSPI
);

Arduino_GFX *realDisplay = new Arduino_GC9A01(bus, TFT_RST, 0, true);

Arduino_Canvas *gfx = new Arduino_Canvas(
  240,
  240,
  realDisplay
);

#define BLACK      0x0000
#define GREEN      0x07E0
#define DARKGREEN  0x0320
#define WHITE      0xFFFF
#define RED        0xF800
#define YELLOW     0xFFE0
#define CYAN       0x07FF

const int CENTER_X = 120;
const int CENTER_Y = 120;
const int RADAR_R  = 112;

float centerLat = 49.4521;
float centerLon = 11.0767;
float radarRadiusKm = 50.0;

unsigned long updateIntervalMs = 15000;
unsigned long lastUpdate = 0;
unsigned long lastAnimDraw = 0;

const unsigned long ANIM_INTERVAL_MS = 250;

String openskyClientId = "";
String openskyClientSecret = "";
String openskyToken = "";
unsigned long openskyTokenUntil = 0;

WebServer server(80);
Preferences prefs;

struct Plane {
  char callsign[12];
  float lat;
  float lon;
  float altitude;
  float velocity;
  float track;
};

Plane planes[100];
int planeCount = 0;

// ------------------------------------------------------------

float degToRad(float deg) {
  return deg * PI / 180.0;
}

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  const float earthRadius = 6371.0;

  float dLat = degToRad(lat2 - lat1);
  float dLon = degToRad(lon2 - lon1);

  float a =
    sin(dLat / 2) * sin(dLat / 2) +
    cos(degToRad(lat1)) *
    cos(degToRad(lat2)) *
    sin(dLon / 2) *
    sin(dLon / 2);

  float c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return earthRadius * c;
}

float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float y = sin(degToRad(lon2 - lon1)) * cos(degToRad(lat2));

  float x =
    cos(degToRad(lat1)) * sin(degToRad(lat2)) -
    sin(degToRad(lat1)) * cos(degToRad(lat2)) *
    cos(degToRad(lon2 - lon1));

  float brng = atan2(y, x) * 180.0 / PI;
  return fmod(brng + 360.0, 360.0);
}

// ------------------------------------------------------------

void showMessage(const char* txt) {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(GREEN);
  gfx->setTextSize(2);
  gfx->setCursor(25, 110);
  gfx->print(txt);

  gfx->flush();
}

void showIpScreen() {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(GREEN);
  gfx->setTextSize(1);

  gfx->setCursor(35, 75);
  gfx->print("ONLINE");

  gfx->setCursor(35, 100);
  gfx->print("IP:");

  gfx->setCursor(35, 118);
  gfx->print(WiFi.localIP());

  gfx->setCursor(35, 145);
  gfx->print("Webseite oeffnen");

  gfx->flush();
}

// ------------------------------------------------------------

void loadSettings() {
  prefs.begin("radar", false);

  centerLat = prefs.getFloat("lat", 49.4521);
  centerLon = prefs.getFloat("lon", 11.0767);
  radarRadiusKm = prefs.getFloat("radius", 50.0);

  int intervalSec = prefs.getInt("interval", 15);
  if (intervalSec < 5) intervalSec = 5;
  if (intervalSec > 300) intervalSec = 300;
  updateIntervalMs = intervalSec * 1000UL;

  openskyClientId = prefs.getString("os_id", "");
  openskyClientSecret = prefs.getString("os_secret", "");
}

void saveSettings() {
  prefs.putFloat("lat", centerLat);
  prefs.putFloat("lon", centerLon);
  prefs.putFloat("radius", radarRadiusKm);
  prefs.putInt("interval", updateIntervalMs / 1000);

  prefs.putString("os_id", openskyClientId);
  prefs.putString("os_secret", openskyClientSecret);
}

// ------------------------------------------------------------

bool hasOpenSkyAuth() {
  return openskyClientId.length() > 0 && openskyClientSecret.length() > 0;
}

bool getOpenSkyToken() {
  if (!hasOpenSkyAuth()) return false;

  if (openskyToken.length() > 0 && millis() < openskyTokenUntil) {
    return true;
  }

  HTTPClient http;
  http.begin("https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=client_credentials";
  body += "&client_id=" + openskyClientId;
  body += "&client_secret=" + openskyClientSecret;

  int code = http.POST(body);

  if (code != HTTP_CODE_OK) {
    Serial.print("OpenSky Token Fehler: ");
    Serial.println(code);
    Serial.println(http.getString());
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(12000);
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.print("Token JSON Fehler: ");
    Serial.println(err.c_str());
    return false;
  }

  openskyToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"] | 1800;

  openskyTokenUntil = millis() + ((expiresIn - 60) * 1000UL);

  Serial.println("OpenSky Token OK");
  return true;
}

// ------------------------------------------------------------

String buildOpenSkyUrl() {
  float latDelta = radarRadiusKm / 111.0;
  float lonDelta = radarRadiusKm / (111.0 * cos(degToRad(centerLat)));

  float lamin = centerLat - latDelta;
  float lamax = centerLat + latDelta;
  float lomin = centerLon - lonDelta;
  float lomax = centerLon + lonDelta;

  String url = "https://opensky-network.org/api/states/all?";
  url += "lamin=" + String(lamin, 6);
  url += "&lomin=" + String(lomin, 6);
  url += "&lamax=" + String(lamax, 6);
  url += "&lomax=" + String(lomax, 6);

  return url;
}

// ------------------------------------------------------------

void drawRadarBase() {
  gfx->fillScreen(BLACK);

  gfx->drawCircle(CENTER_X, CENTER_Y, RADAR_R, GREEN);
  gfx->drawCircle(CENTER_X, CENTER_Y, RADAR_R * 2 / 3, DARKGREEN);
  gfx->drawCircle(CENTER_X, CENTER_Y, RADAR_R / 3, DARKGREEN);

  gfx->drawLine(CENTER_X - RADAR_R, CENTER_Y, CENTER_X + RADAR_R, CENTER_Y, DARKGREEN);
  gfx->drawLine(CENTER_X, CENTER_Y - RADAR_R, CENTER_X, CENTER_Y + RADAR_R, DARKGREEN);

  gfx->drawLine(CENTER_X - 79, CENTER_Y - 79, CENTER_X + 79, CENTER_Y + 79, DARKGREEN);
  gfx->drawLine(CENTER_X - 79, CENTER_Y + 79, CENTER_X + 79, CENTER_Y - 79, DARKGREEN);

  gfx->fillCircle(CENTER_X, CENTER_Y, 4, RED);

}

void drawStatus() {
  gfx->setTextColor(WHITE);
  gfx->setTextSize(1);

  gfx->setCursor(113, 230);
  gfx->print(updateIntervalMs / 1000);
  gfx->print("s");
}

void drawPlaneAnimated(const Plane& p, float progress) {
  float distance = distanceKm(centerLat, centerLon, p.lat, p.lon);
  if (distance > radarRadiusKm) return;

  float bearing = bearingDeg(centerLat, centerLon, p.lat, p.lon);
  float posAngle = degToRad(bearing - 90.0);

  float r = (distance / radarRadiusKm) * RADAR_R;

  float x = CENTER_X + cos(posAngle) * r;
  float y = CENTER_Y + sin(posAngle) * r;

  uint16_t color = CYAN;
  if (p.altitude > 9000) color = YELLOW;

  float drawX = x;
  float drawY = y;

  if (p.track >= 0 && p.velocity > 1) {
    float secondsUntilReload = updateIntervalMs / 1000.0;
    float predictionDistanceKm = (p.velocity * secondsUntilReload) / 1000.0;

    float lineLength = (predictionDistanceKm / radarRadiusKm) * RADAR_R;

    if (lineLength < 8) lineLength = 8;
    if (lineLength > 45) lineLength = 45;

    float trackAngle = degToRad(p.track - 90.0);

    float x2 = x + cos(trackAngle) * lineLength;
    float y2 = y + sin(trackAngle) * lineLength;

    gfx->drawLine(x, y, x2, y2, DARKGREEN);

    drawX = x + (x2 - x) * progress;
    drawY = y + (y2 - y) * progress;
  } else {
    gfx->drawLine(x, y, x + 6, y, DARKGREEN);
  }

  gfx->fillCircle((int)drawX, (int)drawY, 3, color);

  if (strlen(p.callsign) > 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(WHITE);

    int textX = drawX + 6;
    int textY = drawY - 3;

    gfx->setCursor(textX, textY);
    gfx->print(p.callsign);
  }
}

void drawRadarAnimated() {
  drawRadarBase();

  float progress =
    (float)(millis() - lastUpdate) /
    (float)updateIntervalMs;

  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;

  for (int i = 0; i < planeCount; i++) {
    drawPlaneAnimated(planes[i], progress);
  }

  drawStatus();

  gfx->flush();
}

// ------------------------------------------------------------

void fetchPlanes() {
  planeCount = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WIFI nicht verbunden");
    return;
  }

  HTTPClient http;
  String url = buildOpenSkyUrl();

  Serial.println();
  Serial.println("OpenSky URL:");
  Serial.println(url);

  http.begin(url);

  if (hasOpenSkyAuth()) {
    if (getOpenSkyToken()) {
      http.addHeader("Authorization", "Bearer " + openskyToken);
      Serial.println("OpenSky mit Token");
    } else {
      Serial.println("Token fehlgeschlagen, nutze anonym");
    }
  } else {
    Serial.println("OpenSky anonym");
  }

  int code = http.GET();

  Serial.print("HTTP Code: ");
  Serial.println(code);

  if (code != HTTP_CODE_OK) {
    Serial.println(http.getString());
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.print("Payload length: ");
  Serial.println(payload.length());

  DynamicJsonDocument doc(70000);
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.print("JSON Fehler: ");
    Serial.println(err.c_str());
    return;
  }

  JsonArray states = doc["states"];

  if (states.isNull()) {
    Serial.println("states ist NULL");
    return;
  }

  Serial.print("States total: ");
  Serial.println(states.size());

  for (JsonArray state : states) {
    if (planeCount >= 100) break;

    if (state[5].isNull() || state[6].isNull()) continue;

    float lon = state[5];
    float lat = state[6];

    float dist = distanceKm(centerLat, centerLon, lat, lon);
    if (dist > radarRadiusKm) continue;

    Plane& p = planes[planeCount];

    const char* cs = state[1] | "UNKNOWN";
    strncpy(p.callsign, cs, sizeof(p.callsign));
    p.callsign[sizeof(p.callsign) - 1] = 0;

    p.lat = lat;
    p.lon = lon;
    p.altitude = state[7] | 0;
    p.velocity = state[9].isNull() ? 0 : state[9].as<float>();
    p.track = state[10].isNull() ? -1 : state[10].as<float>();

    planeCount++;
  }

  Serial.print("Planes im Radius: ");
  Serial.println(planeCount);
}

// ------------------------------------------------------------

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Radar</title>
<style>
body {
  font-family: Arial, sans-serif;
  background: #050505;
  color: #00ff66;
  padding: 18px;
}
.card {
  max-width: 430px;
  margin: auto;
  border: 1px solid #00ff66;
  border-radius: 14px;
  padding: 18px;
}
h2 { margin-top: 0; }
label { display: block; margin-top: 12px; }
input, button {
  width: 100%;
  font-size: 18px;
  padding: 10px;
  margin-top: 6px;
  box-sizing: border-box;
}
input {
  background: #111;
  color: #00ff66;
  border: 1px solid #00ff66;
  border-radius: 8px;
}
button {
  background: #00ff66;
  color: #000;
  border: none;
  border-radius: 8px;
  font-weight: bold;
  margin-top: 18px;
}
.info {
  color: #aaa;
  font-size: 14px;
  line-height: 1.4;
}
a { color: #00ff66; }
</style>
</head>
<body>
<div class="card">
<h2>Radar Einstellungen</h2>

<form action="/save" method="GET">
<label>Latitude</label>
<input name="lat" value="%LAT%" step="0.000001" type="number">

<label>Longitude</label>
<input name="lon" value="%LON%" step="0.000001" type="number">

<label>Radius in km</label>
<input name="radius" value="%RADIUS%" step="1" min="5" max="250" type="number">

<label>API Intervall in Sekunden</label>
<input name="interval" value="%INTERVAL%" step="1" min="5" max="300" type="number">

<label>OpenSky Client ID</label>
<input name="os_id" value="%OS_ID%" type="text">

<label>OpenSky Client Secret</label>
<input name="os_secret" value="%OS_SECRET%" type="password">

<button type="submit">Speichern</button>
</form>

<p class="info">
Aktuelle Flugzeuge: %PLANES%<br>
IP: %IP%<br>
OpenSky Auth: %AUTH%
</p>

<p class="info">
<a href="/refresh">Radar sofort aktualisieren</a><br>
<a href="/resetwifi">WLAN Zugangsdaten loeschen</a>
</p>
</div>
</body>
</html>
)rawliteral";

  html.replace("%LAT%", String(centerLat, 6));
  html.replace("%LON%", String(centerLon, 6));
  html.replace("%RADIUS%", String(radarRadiusKm, 1));
  html.replace("%INTERVAL%", String(updateIntervalMs / 1000));
  html.replace("%PLANES%", String(planeCount));
  html.replace("%IP%", WiFi.localIP().toString());
  html.replace("%OS_ID%", openskyClientId);
  html.replace("%OS_SECRET%", openskyClientSecret);
  html.replace("%AUTH%", hasOpenSkyAuth() ? "aktiv" : "anonym");

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("lat")) centerLat = server.arg("lat").toFloat();
  if (server.hasArg("lon")) centerLon = server.arg("lon").toFloat();

  if (server.hasArg("radius")) {
    radarRadiusKm = server.arg("radius").toFloat();
    if (radarRadiusKm < 5) radarRadiusKm = 5;
    if (radarRadiusKm > 250) radarRadiusKm = 250;
  }

  if (server.hasArg("interval")) {
    int sec = server.arg("interval").toInt();
    if (sec < 5) sec = 5;
    if (sec > 300) sec = 300;
    updateIntervalMs = sec * 1000UL;
  }

  if (server.hasArg("os_id")) {
    openskyClientId = server.arg("os_id");
  }

  if (server.hasArg("os_secret")) {
    openskyClientSecret = server.arg("os_secret");
  }

  openskyToken = "";
  openskyTokenUntil = 0;

  saveSettings();

  fetchPlanes();
  lastUpdate = millis();
  drawRadarAnimated();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRefresh() {
  fetchPlanes();
  lastUpdate = millis();
  drawRadarAnimated();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleResetWifi() {
  server.send(200, "text/html",
              "<html><body><h3>WLAN wird geloescht. ESP startet neu.</h3></body></html>");

  delay(1000);

  WiFiManager wm;
  wm.resetSettings();

  ESP.restart();
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/refresh", handleRefresh);
  server.on("/resetwifi", handleResetWifi);
  server.begin();
}

// ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  realDisplay->begin();
  gfx->begin();

  loadSettings();

  showMessage("WIFI SETUP");

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  bool connected = wm.autoConnect(
    "ESP32-Radar-Setup",
    "radar1234"
  );

  if (!connected) {
    showMessage("NO WIFI");
    delay(3000);
    ESP.restart();
  }

  showIpScreen();

  setupWebServer();

  delay(3000);

  fetchPlanes();
  lastUpdate = millis();
  drawRadarAnimated();
}

// ------------------------------------------------------------

void loop() {
  server.handleClient();

  if (millis() - lastUpdate >= updateIntervalMs) {
    fetchPlanes();
    lastUpdate = millis();
    drawRadarAnimated();
  }

  if (millis() - lastAnimDraw >= ANIM_INTERVAL_MS) {
    drawRadarAnimated();
    lastAnimDraw = millis();
  }
}
