#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <LittleFS.h>
#include <math.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#define FW_NAME "CC1101 RF Monitor"
#define FW_VERSION "0.1.0"

static const uint8_t CC1101_GDO0_TX_PIN = 13;
static const uint8_t CC1101_CSN_PIN = 5;
static const uint8_t CC1101_SCK_PIN = 18;
static const uint8_t CC1101_MOSI_PIN = 23;
static const uint8_t CC1101_MISO_PIN = 19;
static const uint8_t CC1101_GDO2_RX_PIN = 12;

static const uint16_t DNS_PORT = 53;
static const uint16_t HTTP_PORT = 80;
static const uint16_t WS_PORT = 81;
static const uint16_t TELNET_PORT = 23;

static const uint16_t MAX_PULSES = 256;
static const uint8_t FRAME_QUEUE_SIZE = 6;
static const uint8_t MAX_LEARNED_COMMANDS = 12;
static const uint8_t MAX_LOG_ENTRIES = 30;
static const uint8_t MAX_TELNET_CLIENTS = 3;
static const uint32_t CAPTURE_GAP_US = 6000;
static const uint16_t MIN_PULSE_US = 80;
static const uint8_t MIN_FRAME_PULSES = 8;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

DNSServer dnsServer;
WebServer server(HTTP_PORT);
WebSocketsServer wsServer(WS_PORT);
WiFiServer telnetServer(TELNET_PORT);
Preferences prefs;

struct RadioSettings {
  char wifiSsid[33] = "";
  char wifiPassword[65] = "";
  char hostname[33] = "cc1101-rf-monitor";
  char apSsid[33] = "";
  float frequency = 433.92f;
  float rxBandwidth = 270.83f;
  float dataRate = 4.80f;
  float deviation = 47.60f;
  uint8_t modulation = 2;
  int8_t txPower = 10;
  bool manchester = false;
  uint8_t txRepeat = 4;
  uint32_t txGapUs = 6000;
} settings;

struct PulseFrame {
  uint32_t id = 0;
  uint32_t capturedAt = 0;
  float frequency = 0.0f;
  int rssi = -100;
  bool startLevel = false;
  uint16_t pulseCount = 0;
  uint16_t pulses[MAX_PULSES] = {};
};

struct LearnedCommand {
  bool used = false;
  uint32_t id = 0;
  char name[33] = "";
  float frequency = 0.0f;
  float rxBandwidth = 0.0f;
  float dataRate = 0.0f;
  float deviation = 0.0f;
  uint8_t modulation = 2;
  bool manchester = false;
  bool startLevel = false;
  uint8_t repeat = 4;
  uint32_t gapUs = 6000;
  uint16_t pulseCount = 0;
  uint16_t pulses[MAX_PULSES] = {};
};

struct LogEntry {
  char text[320] = "";
};

struct CaptureState {
  bool active = false;
  bool signalLevel = false;
  bool startLevel = false;
  uint16_t pulseCount = 0;
  uint16_t pulses[MAX_PULSES] = {};
  uint32_t firstEdgeUs = 0;
  uint32_t lastEdgeUs = 0;
};

struct TelnetClientState {
  WiFiClient client;
  char input[192] = "";
  size_t inputLength = 0;
  uint32_t lastActivity = 0;
};

portMUX_TYPE captureMux = portMUX_INITIALIZER_UNLOCKED;
volatile CaptureState isrCapture;
volatile PulseFrame frameQueue[FRAME_QUEUE_SIZE];
volatile uint8_t frameQueueHead = 0;
volatile uint8_t frameQueueTail = 0;
volatile uint8_t frameQueueCount = 0;
volatile uint32_t frameSequence = 0;

LearnedCommand learned[MAX_LEARNED_COMMANDS];
LogEntry logs[MAX_LOG_ENTRIES];
uint8_t logCount = 0;
uint8_t logHead = 0;
TelnetClientState telnetClients[MAX_TELNET_CLIENTS];

PulseFrame lastReceivedFrame;
bool hasLastReceivedFrame = false;
bool radioReady = false;
bool apStarted = false;
bool staConnected = false;
uint32_t wifiConnectStart = 0;
String apSsid;

static void applyRadioSettings();
static void ensureWifi();
static void startAccessPoint();
static void setupWeb();
static void setupSockets();
static void setupTelnet();
static void handleSockets();
static void handleTelnet();
static void flushCapturedFrameIfIdle();
static bool dequeueFrame(PulseFrame &frame);
static void handleReceivedFrame(PulseFrame &frame);
static void sendRawPulses(const PulseFrame &frame, uint8_t repeatOverride = 0, uint32_t gapOverride = 0);
static void saveSettings();
static void loadSettings();
static void saveLearnedCommands();
static void loadLearnedCommands();
static void broadcastState(uint8_t clientId = 255);
static void addLogf(const char *fmt, ...);
static void sendTextToTelnet(const char *line);
static bool parsePulsesFromJson(JsonVariant src, uint16_t *pulses, uint16_t &count);
static bool parseSendFrame(JsonVariant src, PulseFrame &frame, uint8_t &repeat, uint32_t &gapUs);
static void syncClocklessDelay(uint32_t usec);
template <typename TDoc> static void writeJsonState(TDoc &doc);

static uint32_t chipIdShort() {
  const uint64_t mac = ESP.getEfuseMac();
  return static_cast<uint32_t>((mac >> 24) & 0xFFFFFF);
}

static void IRAM_ATTR pushFrameFromISR() {
  if (isrCapture.pulseCount < MIN_FRAME_PULSES) {
    isrCapture.active = false;
    isrCapture.pulseCount = 0;
    isrCapture.firstEdgeUs = 0;
    isrCapture.lastEdgeUs = 0;
    return;
  }
  PulseFrame &slot = const_cast<PulseFrame &>(frameQueue[frameQueueHead]);
  slot.id = ++frameSequence;
  slot.capturedAt = millis();
  slot.frequency = settings.frequency;
  slot.rssi = -100;
  slot.startLevel = isrCapture.startLevel;
  slot.pulseCount = isrCapture.pulseCount;
  memcpy(slot.pulses, (const void *)isrCapture.pulses, sizeof(uint16_t) * isrCapture.pulseCount);
  if (frameQueueCount == FRAME_QUEUE_SIZE) {
    frameQueueTail = (frameQueueTail + 1) % FRAME_QUEUE_SIZE;
    frameQueueCount--;
  }
  frameQueueHead = (frameQueueHead + 1) % FRAME_QUEUE_SIZE;
  frameQueueCount++;
  isrCapture.active = false;
  isrCapture.pulseCount = 0;
  isrCapture.firstEdgeUs = 0;
  isrCapture.lastEdgeUs = 0;
}

void IRAM_ATTR onRadioEdge() {
  const uint32_t now = micros();
  const bool levelNow = digitalRead(CC1101_GDO2_RX_PIN);

  portENTER_CRITICAL_ISR(&captureMux);
  if (!isrCapture.active) {
    isrCapture.active = true;
    isrCapture.startLevel = levelNow;
    isrCapture.signalLevel = levelNow;
    isrCapture.pulseCount = 0;
    isrCapture.firstEdgeUs = now;
    isrCapture.lastEdgeUs = now;
    portEXIT_CRITICAL_ISR(&captureMux);
    return;
  }

  const uint32_t duration = now - isrCapture.lastEdgeUs;
  if (duration >= CAPTURE_GAP_US) {
    pushFrameFromISR();
    isrCapture.active = true;
    isrCapture.startLevel = levelNow;
    isrCapture.signalLevel = levelNow;
    isrCapture.firstEdgeUs = now;
    isrCapture.lastEdgeUs = now;
    portEXIT_CRITICAL_ISR(&captureMux);
    return;
  }

  if (duration >= MIN_PULSE_US && isrCapture.pulseCount < MAX_PULSES) {
    isrCapture.pulses[isrCapture.pulseCount++] = static_cast<uint16_t>(duration);
  } else if (isrCapture.pulseCount >= MAX_PULSES) {
    pushFrameFromISR();
    portEXIT_CRITICAL_ISR(&captureMux);
    return;
  }

  isrCapture.signalLevel = levelNow;
  isrCapture.lastEdgeUs = now;
  portEXIT_CRITICAL_ISR(&captureMux);
}

static void syncClocklessDelay(uint32_t usec) {
  while (usec > 16000) {
    delayMicroseconds(16000);
    usec -= 16000;
  }
  if (usec > 0) delayMicroseconds(usec);
}

static void fastWriteTx(bool level) {
  const uint32_t mask = 1UL << CC1101_GDO0_TX_PIN;
  if (level) REG_WRITE(GPIO_OUT_W1TS_REG, mask);
  else REG_WRITE(GPIO_OUT_W1TC_REG, mask);
}

static void loadSettings() {
  prefs.begin("rfmon", true);
  prefs.getString("wifi_ssid", settings.wifiSsid, sizeof(settings.wifiSsid));
  prefs.getString("wifi_pass", settings.wifiPassword, sizeof(settings.wifiPassword));
  prefs.getString("hostname", settings.hostname, sizeof(settings.hostname));
  prefs.getString("ap_ssid", settings.apSsid, sizeof(settings.apSsid));
  settings.frequency = prefs.getFloat("freq", settings.frequency);
  settings.rxBandwidth = prefs.getFloat("rxbw", settings.rxBandwidth);
  settings.dataRate = prefs.getFloat("drate", settings.dataRate);
  settings.deviation = prefs.getFloat("dev", settings.deviation);
  settings.modulation = prefs.getUChar("mod", settings.modulation);
  settings.txPower = static_cast<int8_t>(prefs.getChar("txpwr", settings.txPower));
  settings.manchester = prefs.getBool("manch", settings.manchester);
  settings.txRepeat = prefs.getUChar("txrep", settings.txRepeat);
  settings.txGapUs = prefs.getUInt("txgap", settings.txGapUs);
  prefs.end();

  if (settings.hostname[0] == '\0') strlcpy(settings.hostname, "cc1101-rf-monitor", sizeof(settings.hostname));
  if (settings.apSsid[0] == '\0') {
    snprintf(settings.apSsid, sizeof(settings.apSsid), "RF-Monitor-%06lX", static_cast<unsigned long>(chipIdShort()));
  }
}

static void saveSettings() {
  prefs.begin("rfmon", false);
  prefs.putString("wifi_ssid", settings.wifiSsid);
  prefs.putString("wifi_pass", settings.wifiPassword);
  prefs.putString("hostname", settings.hostname);
  prefs.putString("ap_ssid", settings.apSsid);
  prefs.putFloat("freq", settings.frequency);
  prefs.putFloat("rxbw", settings.rxBandwidth);
  prefs.putFloat("drate", settings.dataRate);
  prefs.putFloat("dev", settings.deviation);
  prefs.putUChar("mod", settings.modulation);
  prefs.putChar("txpwr", settings.txPower);
  prefs.putBool("manch", settings.manchester);
  prefs.putUChar("txrep", settings.txRepeat);
  prefs.putUInt("txgap", settings.txGapUs);
  prefs.end();
}

static void addLogf(const char *fmt, ...) {
  char buffer[320];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  strlcpy(logs[logHead].text, buffer, sizeof(logs[logHead].text));
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES) logCount++;

  StaticJsonDocument<384> doc;
  doc["type"] = "log";
  doc["message"] = buffer;
  char json[384];
  serializeJson(doc, json, sizeof(json));
  wsServer.broadcastTXT(json);
  sendTextToTelnet(buffer);
  Serial.println(buffer);
}

static void saveLearnedCommands() {
  DynamicJsonDocument doc(32768);
  JsonArray arr = doc.createNestedArray("commands");
  for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) {
    if (!learned[i].used) continue;
    JsonObject obj = arr.createNestedObject();
    obj["id"] = learned[i].id;
    obj["name"] = learned[i].name;
    obj["frequency"] = learned[i].frequency;
    obj["rxBandwidth"] = learned[i].rxBandwidth;
    obj["dataRate"] = learned[i].dataRate;
    obj["deviation"] = learned[i].deviation;
    obj["modulation"] = learned[i].modulation;
    obj["manchester"] = learned[i].manchester;
    obj["startLevel"] = learned[i].startLevel;
    obj["repeat"] = learned[i].repeat;
    obj["gapUs"] = learned[i].gapUs;
    JsonArray pulses = obj.createNestedArray("pulses");
    for (uint16_t j = 0; j < learned[i].pulseCount; j++) pulses.add(learned[i].pulses[j]);
  }

  File file = LittleFS.open("/learned.json", "w");
  if (!file) {
    addLogf("Failed to save learned commands");
    return;
  }
  serializeJson(doc, file);
  file.close();
}

static void loadLearnedCommands() {
  for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) learned[i] = LearnedCommand();
  if (!LittleFS.exists("/learned.json")) return;

  File file = LittleFS.open("/learned.json", "r");
  if (!file) return;

  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, file)) {
    file.close();
    addLogf("Failed to parse /learned.json");
    return;
  }
  file.close();

  JsonArray arr = doc["commands"].as<JsonArray>();
  uint8_t slot = 0;
  for (JsonObject obj : arr) {
    if (slot >= MAX_LEARNED_COMMANDS) break;
    learned[slot].used = true;
    learned[slot].id = obj["id"] | static_cast<uint32_t>(millis() + slot + 1);
    strlcpy(learned[slot].name, obj["name"] | "command", sizeof(learned[slot].name));
    learned[slot].frequency = obj["frequency"] | settings.frequency;
    learned[slot].rxBandwidth = obj["rxBandwidth"] | settings.rxBandwidth;
    learned[slot].dataRate = obj["dataRate"] | settings.dataRate;
    learned[slot].deviation = obj["deviation"] | settings.deviation;
    learned[slot].modulation = obj["modulation"] | settings.modulation;
    learned[slot].manchester = obj["manchester"] | settings.manchester;
    learned[slot].startLevel = obj["startLevel"] | false;
    learned[slot].repeat = obj["repeat"] | settings.txRepeat;
    learned[slot].gapUs = obj["gapUs"] | settings.txGapUs;
    JsonArray pulses = obj["pulses"].as<JsonArray>();
    for (JsonVariant v : pulses) {
      if (learned[slot].pulseCount >= MAX_PULSES) break;
      learned[slot].pulses[learned[slot].pulseCount++] = v.as<uint16_t>();
    }
    slot++;
  }
}

static void startAccessPoint() {
  if (apStarted) return;
  apSsid = settings.apSsid;
  WiFi.softAP(apSsid.c_str());
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  apStarted = true;
  addLogf("AP started: %s on %s", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void ensureWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(settings.hostname);
  startAccessPoint();

  if (settings.wifiSsid[0] == '\0') {
    addLogf("No STA credentials saved, staying in AP mode");
    return;
  }

  WiFi.begin(settings.wifiSsid, settings.wifiPassword);
  wifiConnectStart = millis();
  addLogf("Connecting to WiFi SSID: %s", settings.wifiSsid);
}

static void applyRadioSettings() {
  detachInterrupt(digitalPinToInterrupt(CC1101_GDO2_RX_PIN));
  radioReady = false;

  ELECHOUSE_cc1101.setGDO(CC1101_GDO0_TX_PIN, CC1101_GDO2_RX_PIN);
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CSN_PIN);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setMHZ(settings.frequency);
  ELECHOUSE_cc1101.setRxBW(settings.rxBandwidth);
  ELECHOUSE_cc1101.setDRate(settings.dataRate);
  ELECHOUSE_cc1101.setDeviation(settings.deviation);
  ELECHOUSE_cc1101.setPA(settings.txPower);
  ELECHOUSE_cc1101.setModulation(settings.modulation);
  ELECHOUSE_cc1101.setManchester(settings.manchester ? 1 : 0);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setCRC_AF(0);
  ELECHOUSE_cc1101.setSyncMode(0);
  ELECHOUSE_cc1101.setAdrChk(0);
  ELECHOUSE_cc1101.setDcFilterOff(0);

  if (!ELECHOUSE_cc1101.getCC1101()) {
    addLogf("CC1101 init failed");
    return;
  }

  pinMode(CC1101_GDO2_RX_PIN, INPUT);
  pinMode(CC1101_GDO0_TX_PIN, OUTPUT);
  fastWriteTx(false);
  ELECHOUSE_cc1101.SetRx();
  attachInterrupt(digitalPinToInterrupt(CC1101_GDO2_RX_PIN), onRadioEdge, CHANGE);
  radioReady = true;

  addLogf("Radio ready: freq=%.2fMHz bw=%.2fkHz rate=%.2fkBd mod=%u manchester=%s",
    settings.frequency,
    settings.rxBandwidth,
    settings.dataRate,
    settings.modulation,
    settings.manchester ? "on" : "off");
}

static void flushCapturedFrameIfIdle() {
  portENTER_CRITICAL(&captureMux);
  if (isrCapture.active && isrCapture.pulseCount >= MIN_FRAME_PULSES) {
    const uint32_t now = micros();
    if ((now - isrCapture.lastEdgeUs) >= CAPTURE_GAP_US) {
      pushFrameFromISR();
    }
  }
  portEXIT_CRITICAL(&captureMux);
}

static bool dequeueFrame(PulseFrame &frame) {
  bool hasFrame = false;
  portENTER_CRITICAL(&captureMux);
  if (frameQueueCount > 0) {
    memcpy(&frame, (const void *)&frameQueue[frameQueueTail], sizeof(PulseFrame));
    frameQueueTail = (frameQueueTail + 1) % FRAME_QUEUE_SIZE;
    frameQueueCount--;
    hasFrame = true;
  }
  portEXIT_CRITICAL(&captureMux);
  return hasFrame;
}

static void handleReceivedFrame(PulseFrame &frame) {
  frame.rssi = ELECHOUSE_cc1101.getRssi();
  lastReceivedFrame = frame;
  hasLastReceivedFrame = true;

  DynamicJsonDocument doc(4096);
  doc["type"] = "rx";
  doc["id"] = frame.id;
  doc["timestamp"] = frame.capturedAt;
  doc["frequency"] = frame.frequency;
  doc["rssi"] = frame.rssi;
  doc["startLevel"] = frame.startLevel;
  doc["pulseCount"] = frame.pulseCount;
  JsonArray pulses = doc.createNestedArray("pulses");
  for (uint16_t i = 0; i < frame.pulseCount; i++) pulses.add(frame.pulses[i]);

  char payload[4096];
  serializeJson(doc, payload, sizeof(payload));
  wsServer.broadcastTXT(payload);

  addLogf("RX #%lu freq=%.2f RSSI=%d start=%s pulses=%u",
    static_cast<unsigned long>(frame.id),
    frame.frequency,
    frame.rssi,
    frame.startLevel ? "HIGH" : "LOW",
    frame.pulseCount);
}

static void sendRawPulses(const PulseFrame &frame, uint8_t repeatOverride, uint32_t gapOverride) {
  if (!radioReady || frame.pulseCount == 0) return;

  const uint8_t repeats = repeatOverride > 0 ? repeatOverride : settings.txRepeat;
  const uint32_t gapUs = gapOverride > 0 ? gapOverride : settings.txGapUs;

  detachInterrupt(digitalPinToInterrupt(CC1101_GDO2_RX_PIN));
  ELECHOUSE_cc1101.SetTx();
  pinMode(CC1101_GDO0_TX_PIN, OUTPUT);

  for (uint8_t r = 0; r < repeats; r++) {
    bool level = frame.startLevel;
    fastWriteTx(level);
    for (uint16_t i = 0; i < frame.pulseCount; i++) {
      syncClocklessDelay(frame.pulses[i]);
      level = !level;
      fastWriteTx(level);
    }
    fastWriteTx(false);
    syncClocklessDelay(gapUs);
  }

  ELECHOUSE_cc1101.setSidle();
  fastWriteTx(false);
  ELECHOUSE_cc1101.SetRx();
  attachInterrupt(digitalPinToInterrupt(CC1101_GDO2_RX_PIN), onRadioEdge, CHANGE);

  DynamicJsonDocument doc(4096);
  doc["type"] = "tx";
  doc["frequency"] = frame.frequency;
  doc["startLevel"] = frame.startLevel;
  doc["pulseCount"] = frame.pulseCount;
  doc["repeat"] = repeats;
  doc["gapUs"] = gapUs;
  JsonArray pulses = doc.createNestedArray("pulses");
  for (uint16_t i = 0; i < frame.pulseCount; i++) pulses.add(frame.pulses[i]);
  char payload[4096];
  serializeJson(doc, payload, sizeof(payload));
  wsServer.broadcastTXT(payload);

  addLogf("TX freq=%.2f repeat=%u gap=%lu start=%s pulses=%u",
    frame.frequency,
    repeats,
    static_cast<unsigned long>(gapUs),
    frame.startLevel ? "HIGH" : "LOW",
    frame.pulseCount);
}

static LearnedCommand *findLearnedById(uint32_t id) {
  for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) {
    if (learned[i].used && learned[i].id == id) return &learned[i];
  }
  return nullptr;
}

template <typename TDoc> static void writeJsonState(TDoc &doc) {
  doc["name"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["radioReady"] = radioReady;
  doc["apStarted"] = apStarted;
  doc["apSsid"] = apSsid;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["staConnected"] = WiFi.status() == WL_CONNECTED;
  doc["staIp"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["staSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  doc["staRssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -100;

  JsonObject radio = doc.createNestedObject("radio");
  radio["frequency"] = settings.frequency;
  radio["rxBandwidth"] = settings.rxBandwidth;
  radio["dataRate"] = settings.dataRate;
  radio["deviation"] = settings.deviation;
  radio["modulation"] = settings.modulation;
  radio["txPower"] = settings.txPower;
  radio["manchester"] = settings.manchester;
  radio["txRepeat"] = settings.txRepeat;
  radio["txGapUs"] = settings.txGapUs;

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = settings.wifiSsid;
  wifi["hostname"] = settings.hostname;
  wifi["apSsid"] = settings.apSsid;

  JsonArray commandArray = doc.createNestedArray("learned");
  for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) {
    if (!learned[i].used) continue;
    JsonObject obj = commandArray.createNestedObject();
    obj["id"] = learned[i].id;
    obj["name"] = learned[i].name;
    obj["frequency"] = learned[i].frequency;
    obj["repeat"] = learned[i].repeat;
    obj["gapUs"] = learned[i].gapUs;
    obj["startLevel"] = learned[i].startLevel;
    obj["pulseCount"] = learned[i].pulseCount;
  }

  JsonArray logArray = doc.createNestedArray("logs");
  const uint8_t count = logCount;
  const uint8_t start = (logHead + MAX_LOG_ENTRIES - count) % MAX_LOG_ENTRIES;
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t idx = (start + i) % MAX_LOG_ENTRIES;
    logArray.add(logs[idx].text);
  }

  JsonObject last = doc.createNestedObject("lastFrame");
  if (hasLastReceivedFrame) {
    last["available"] = true;
    last["id"] = lastReceivedFrame.id;
    last["frequency"] = lastReceivedFrame.frequency;
    last["rssi"] = lastReceivedFrame.rssi;
    last["startLevel"] = lastReceivedFrame.startLevel;
    last["pulseCount"] = lastReceivedFrame.pulseCount;
    JsonArray pulses = last.createNestedArray("pulses");
    for (uint16_t i = 0; i < lastReceivedFrame.pulseCount; i++) pulses.add(lastReceivedFrame.pulses[i]);
  } else {
    last["available"] = false;
  }
}

static void broadcastState(uint8_t clientId) {
  StaticJsonDocument<8192> doc;
  doc["type"] = "state";
  writeJsonState(doc);
  char payload[8192];
  serializeJson(doc, payload, sizeof(payload));
  if (clientId == 255) wsServer.broadcastTXT(payload);
  else wsServer.sendTXT(clientId, payload);
}

static bool parsePulsesFromJson(JsonVariant src, uint16_t *pulses, uint16_t &count) {
  count = 0;
  if (!src.is<JsonArray>()) return false;
  JsonArray arr = src.as<JsonArray>();
  for (JsonVariant value : arr) {
    if (count >= MAX_PULSES) break;
    const int pulse = value.as<int>();
    if (pulse < static_cast<int>(MIN_PULSE_US)) continue;
    pulses[count++] = static_cast<uint16_t>(pulse);
  }
  return count > 0;
}

static bool parseSendFrame(JsonVariant src, PulseFrame &frame, uint8_t &repeat, uint32_t &gapUs) {
  frame = PulseFrame();
  frame.frequency = src["frequency"] | settings.frequency;
  frame.startLevel = src["startLevel"] | false;
  repeat = src["repeat"] | settings.txRepeat;
  gapUs = src["gapUs"] | settings.txGapUs;
  return parsePulsesFromJson(src["pulses"], frame.pulses, frame.pulseCount);
}

static void handleApiState() {
  StaticJsonDocument<8192> doc;
  writeJsonState(doc);
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

static void handleApiSettings() {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  JsonObject wifi = doc["wifi"];
  if (!wifi.isNull()) {
    if (wifi.containsKey("ssid")) strlcpy(settings.wifiSsid, wifi["ssid"] | "", sizeof(settings.wifiSsid));
    if (wifi.containsKey("password")) strlcpy(settings.wifiPassword, wifi["password"] | "", sizeof(settings.wifiPassword));
    if (wifi.containsKey("hostname")) strlcpy(settings.hostname, wifi["hostname"] | settings.hostname, sizeof(settings.hostname));
    if (wifi.containsKey("apSsid")) strlcpy(settings.apSsid, wifi["apSsid"] | settings.apSsid, sizeof(settings.apSsid));
  }

  JsonObject radio = doc["radio"];
  if (!radio.isNull()) {
    settings.frequency = radio["frequency"] | settings.frequency;
    settings.rxBandwidth = radio["rxBandwidth"] | settings.rxBandwidth;
    settings.dataRate = radio["dataRate"] | settings.dataRate;
    settings.deviation = radio["deviation"] | settings.deviation;
    settings.modulation = radio["modulation"] | settings.modulation;
    settings.txPower = radio["txPower"] | settings.txPower;
    settings.manchester = radio["manchester"] | settings.manchester;
    settings.txRepeat = radio["txRepeat"] | settings.txRepeat;
    settings.txGapUs = radio["txGapUs"] | settings.txGapUs;
  }

  saveSettings();
  WiFi.setHostname(settings.hostname);
  apSsid = settings.apSsid;
  WiFi.softAPdisconnect(true);
  apStarted = false;
  startAccessPoint();
  if (strlen(settings.wifiSsid) > 0) {
    WiFi.disconnect();
    WiFi.begin(settings.wifiSsid, settings.wifiPassword);
    wifiConnectStart = millis();
  }
  applyRadioSettings();

  server.send(200, "application/json", "{\"ok\":true}");
  broadcastState();
}

static void handleApiSend() {
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  PulseFrame frame;
  uint8_t repeat = settings.txRepeat;
  uint32_t gapUs = settings.txGapUs;
  if (!parseSendFrame(doc.as<JsonVariant>(), frame, repeat, gapUs)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing or invalid pulses\"}");
    return;
  }

  if (fabs(frame.frequency - settings.frequency) > 0.0001f) {
    settings.frequency = frame.frequency;
    saveSettings();
    applyRadioSettings();
  }

  sendRawPulses(frame, repeat, gapUs);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiLearnSave() {
  if (!hasLastReceivedFrame) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"No received frame available\"}");
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  const char *name = doc["name"] | "";
  if (name[0] == '\0') {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Name is required\"}");
    return;
  }

  int freeIndex = -1;
  for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) {
    if (!learned[i].used) {
      freeIndex = i;
      break;
    }
  }
  if (freeIndex < 0) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"Learned storage is full\"}");
    return;
  }

  LearnedCommand &cmd = learned[freeIndex];
  cmd = LearnedCommand();
  cmd.used = true;
  cmd.id = millis() + chipIdShort() + freeIndex + 1;
  strlcpy(cmd.name, name, sizeof(cmd.name));
  cmd.frequency = settings.frequency;
  cmd.rxBandwidth = settings.rxBandwidth;
  cmd.dataRate = settings.dataRate;
  cmd.deviation = settings.deviation;
  cmd.modulation = settings.modulation;
  cmd.manchester = settings.manchester;
  cmd.startLevel = lastReceivedFrame.startLevel;
  cmd.repeat = doc["repeat"] | settings.txRepeat;
  cmd.gapUs = doc["gapUs"] | settings.txGapUs;
  cmd.pulseCount = lastReceivedFrame.pulseCount;
  memcpy(cmd.pulses, lastReceivedFrame.pulses, sizeof(uint16_t) * cmd.pulseCount);

  saveLearnedCommands();
  addLogf("Learned command saved: %s (%lu)", cmd.name, static_cast<unsigned long>(cmd.id));
  server.send(200, "application/json", "{\"ok\":true}");
  broadcastState();
}

static void handleApiLearnSend() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing id\"}");
    return;
  }
  const uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  LearnedCommand *cmd = findLearnedById(id);
  if (!cmd) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Command not found\"}");
    return;
  }

  settings.frequency = cmd->frequency;
  saveSettings();
  applyRadioSettings();

  PulseFrame frame;
  frame.frequency = cmd->frequency;
  frame.startLevel = cmd->startLevel;
  frame.pulseCount = cmd->pulseCount;
  memcpy(frame.pulses, cmd->pulses, sizeof(uint16_t) * frame.pulseCount);
  sendRawPulses(frame, cmd->repeat, cmd->gapUs);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiLearnDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing id\"}");
    return;
  }
  const uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 10);
  LearnedCommand *cmd = findLearnedById(id);
  if (!cmd) {
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"Command not found\"}");
    return;
  }
  addLogf("Deleted learned command: %s (%lu)", cmd->name, static_cast<unsigned long>(cmd->id));
  *cmd = LearnedCommand();
  saveLearnedCommands();
  server.send(200, "application/json", "{\"ok\":true}");
  broadcastState();
}

static void serveIndex() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain", "Missing /index.html in LittleFS");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

static void handleCaptivePortal() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

static void setupWeb() {
  server.on("/", HTTP_GET, serveIndex);
  server.on("/index.html", HTTP_GET, serveIndex);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/settings", HTTP_POST, handleApiSettings);
  server.on("/api/send", HTTP_POST, handleApiSend);
  server.on("/api/learn/save", HTTP_POST, handleApiLearnSave);
  server.on("/api/learn/send", HTTP_POST, handleApiLearnSend);
  server.on("/api/learn/delete", HTTP_POST, handleApiLearnDelete);

  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);

  server.onNotFound([]() {
    if (WiFi.getMode() & WIFI_AP) {
      handleCaptivePortal();
      return;
    }
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

static void handleSocketMessage(uint8_t clientId, uint8_t *payload, size_t length) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload, length)) return;
  const char *action = doc["action"] | "";
  if (strcmp(action, "state") == 0) {
    broadcastState(clientId);
  } else if (strcmp(action, "send") == 0) {
    PulseFrame frame;
    uint8_t repeat = settings.txRepeat;
    uint32_t gapUs = settings.txGapUs;
    if (parseSendFrame(doc.as<JsonVariant>(), frame, repeat, gapUs)) {
      if (fabs(frame.frequency - settings.frequency) > 0.0001f) {
        settings.frequency = frame.frequency;
        saveSettings();
        applyRadioSettings();
      }
      sendRawPulses(frame, repeat, gapUs);
    }
  }
}

static void setupSockets() {
  wsServer.begin();
  wsServer.onEvent([](uint8_t clientId, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_CONNECTED) {
      addLogf("WebSocket client %u connected", clientId);
      broadcastState(clientId);
    } else if (type == WStype_TEXT) {
      handleSocketMessage(clientId, payload, length);
    } else if (type == WStype_DISCONNECTED) {
      addLogf("WebSocket client %u disconnected", clientId);
    }
  });
}

static void setupTelnet() {
  telnetServer.begin();
  telnetServer.setNoDelay(true);
}

static void sendTextToTelnet(const char *line) {
  for (auto &clientState : telnetClients) {
    if (clientState.client && clientState.client.connected()) {
      clientState.client.println(line);
    }
  }
}

static void handleTelnetCommand(TelnetClientState &clientState, char *line) {
  while (*line == ' ') line++;
  if (*line == '\0') return;

  char *command = strtok(line, " ");
  if (!command) return;

  if (strcmp(command, "help") == 0) {
    clientState.client.println("help");
    clientState.client.println("status");
    clientState.client.println("radio");
    clientState.client.println("set freq <mhz>");
    clientState.client.println("learn list");
    clientState.client.println("learn send <id>");
    clientState.client.println("learn delete <id>");
    clientState.client.println("send <startLevel 0|1> <repeat> <gapUs> <comma-separated-pulses>");
    return;
  }

  if (strcmp(command, "status") == 0) {
    clientState.client.printf("AP: %s (%s)\r\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
    clientState.client.printf("STA: %s %s\r\n", WiFi.status() == WL_CONNECTED ? "connected" : "down",
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
    clientState.client.printf("Radio: %s freq=%.2f bw=%.2f rate=%.2f mod=%u\r\n",
      radioReady ? "ready" : "down", settings.frequency, settings.rxBandwidth, settings.dataRate, settings.modulation);
    return;
  }

  if (strcmp(command, "radio") == 0) {
    clientState.client.printf("freq=%.2f bw=%.2f rate=%.2f deviation=%.2f mod=%u manchester=%s txPower=%d repeat=%u gap=%lu\r\n",
      settings.frequency, settings.rxBandwidth, settings.dataRate, settings.deviation, settings.modulation,
      settings.manchester ? "on" : "off", settings.txPower, settings.txRepeat, static_cast<unsigned long>(settings.txGapUs));
    return;
  }

  if (strcmp(command, "set") == 0) {
    char *field = strtok(nullptr, " ");
    char *value = strtok(nullptr, " ");
    if (field && value && strcmp(field, "freq") == 0) {
      settings.frequency = atof(value);
      saveSettings();
      applyRadioSettings();
      clientState.client.printf("Frequency set to %.2f\r\n", settings.frequency);
      broadcastState();
      return;
    }
  }

  if (strcmp(command, "learn") == 0) {
    char *sub = strtok(nullptr, " ");
    if (sub && strcmp(sub, "list") == 0) {
      for (uint8_t i = 0; i < MAX_LEARNED_COMMANDS; i++) {
        if (learned[i].used) {
          clientState.client.printf("%lu %s freq=%.2f pulses=%u\r\n",
            static_cast<unsigned long>(learned[i].id), learned[i].name, learned[i].frequency, learned[i].pulseCount);
        }
      }
      return;
    }
    if (sub && strcmp(sub, "send") == 0) {
      char *idToken = strtok(nullptr, " ");
      if (!idToken) {
        clientState.client.println("Missing id");
        return;
      }
      LearnedCommand *cmd = findLearnedById(strtoul(idToken, nullptr, 10));
      if (!cmd) {
        clientState.client.println("Command not found");
        return;
      }
      PulseFrame frame;
      frame.frequency = cmd->frequency;
      frame.startLevel = cmd->startLevel;
      frame.pulseCount = cmd->pulseCount;
      memcpy(frame.pulses, cmd->pulses, sizeof(uint16_t) * frame.pulseCount);
      sendRawPulses(frame, cmd->repeat, cmd->gapUs);
      return;
    }
    if (sub && strcmp(sub, "delete") == 0) {
      char *idToken = strtok(nullptr, " ");
      if (!idToken) {
        clientState.client.println("Missing id");
        return;
      }
      LearnedCommand *cmd = findLearnedById(strtoul(idToken, nullptr, 10));
      if (!cmd) {
        clientState.client.println("Command not found");
        return;
      }
      *cmd = LearnedCommand();
      saveLearnedCommands();
      broadcastState();
      clientState.client.println("Deleted");
      return;
    }
  }

  if (strcmp(command, "send") == 0) {
    char *startLevelTok = strtok(nullptr, " ");
    char *repeatTok = strtok(nullptr, " ");
    char *gapTok = strtok(nullptr, " ");
    char *pulseTok = strtok(nullptr, "");
    if (!startLevelTok || !repeatTok || !gapTok || !pulseTok) {
      clientState.client.println("Usage: send <startLevel 0|1> <repeat> <gapUs> <comma-separated-pulses>");
      return;
    }
    PulseFrame frame;
    frame.frequency = settings.frequency;
    frame.startLevel = atoi(startLevelTok) != 0;
    char *token = strtok(pulseTok, ",");
    while (token && frame.pulseCount < MAX_PULSES) {
      const int pulse = atoi(token);
      if (pulse >= static_cast<int>(MIN_PULSE_US)) frame.pulses[frame.pulseCount++] = static_cast<uint16_t>(pulse);
      token = strtok(nullptr, ",");
    }
    if (frame.pulseCount == 0) {
      clientState.client.println("No valid pulses");
      return;
    }
    sendRawPulses(frame, static_cast<uint8_t>(atoi(repeatTok)), static_cast<uint32_t>(strtoul(gapTok, nullptr, 10)));
    return;
  }

  clientState.client.println("Unknown command");
}

static void handleTelnet() {
  WiFiClient incoming = telnetServer.available();
  if (incoming) {
    for (auto &clientState : telnetClients) {
      if (!clientState.client || !clientState.client.connected()) {
        clientState.client = incoming;
        clientState.inputLength = 0;
        clientState.lastActivity = millis();
        clientState.client.println("CC1101 RF Monitor");
        clientState.client.println("Type help for commands");
        break;
      }
    }
  }

  for (auto &clientState : telnetClients) {
    if (!clientState.client || !clientState.client.connected()) continue;
    while (clientState.client.available()) {
      const char ch = clientState.client.read();
      if (ch == '\r') continue;
      if (ch == '\n') {
        clientState.input[clientState.inputLength] = '\0';
        handleTelnetCommand(clientState, clientState.input);
        clientState.inputLength = 0;
      } else if (isprint(static_cast<unsigned char>(ch)) && clientState.inputLength < sizeof(clientState.input) - 1) {
        clientState.input[clientState.inputLength++] = ch;
      }
      clientState.lastActivity = millis();
    }
    if (millis() - clientState.lastActivity > 600000UL) {
      clientState.client.println("Timeout");
      clientState.client.stop();
      clientState.inputLength = 0;
    }
  }
}

static void handleSockets() {
  wsServer.loop();
}

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println(FW_NAME);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  loadSettings();
  loadLearnedCommands();
  ensureWifi();
  setupWeb();
  setupSockets();
  setupTelnet();
  applyRadioSettings();
  broadcastState();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleSockets();
  handleTelnet();
  flushCapturedFrameIfIdle();

  PulseFrame frame;
  while (dequeueFrame(frame)) {
    handleReceivedFrame(frame);
  }

  if (!staConnected && WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    addLogf("STA connected: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    broadcastState();
  } else if (staConnected && WiFi.status() != WL_CONNECTED) {
    staConnected = false;
    addLogf("STA disconnected");
    broadcastState();
  }

  if (settings.wifiSsid[0] != '\0' && WiFi.status() != WL_CONNECTED && wifiConnectStart > 0 &&
      millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnectStart = 0;
    addLogf("STA connect timeout, AP remains available as fallback");
  }
}
