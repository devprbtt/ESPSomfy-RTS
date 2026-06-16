#include <ArduinoJson.h>
#include <LittleFS.h>

#include "DinplugBridge.h"
#include "ConfigSettings.h"
#include "Network.h"
#include "Somfy.h"

extern Network net;
extern SomfyShadeController somfy;

const char *DinplugBridge::kConfigPath = "/dinplug.cfg";
DinplugBridge dinplugBridge;

DinplugBridge::DinplugBridge() { this->gatewayHost[0] = '\0'; }

void DinplugBridge::begin() {
  this->loadConfig();
  if(this->autoConnect && this->gatewayHost[0] != '\0') this->ensureConnected(true);
}

void DinplugBridge::loop() {
  if(this->gatewayHost[0] == '\0') return;
  if(this->autoConnect) this->ensureConnected(false);
  if(!this->client.connected() && this->wasConnected) {
    this->wasConnected = false;
    Serial.println("dinplug: disconnected");
  }
  if(!this->client.connected()) return;
  const unsigned long now = millis();
  if((now - this->lastKeepAliveMs) > kKeepAliveIntervalMs) {
    if(!this->sendCommand("STA")) {
      this->ensureConnected(true);
      return;
    }
    this->lastKeepAliveMs = now;
  }
  if(this->lastRxMs > 0 && (now - this->lastRxMs) > kRxTimeoutMs) {
    Serial.println("dinplug: rx timeout, reconnecting");
    this->client.stop();
    this->wasConnected = false;
    this->ensureConnected(true);
    return;
  }
  while(this->client.available()) {
    const char ch = static_cast<char>(this->client.read());
    this->lastRxMs = millis();
    if(ch == '\r') continue;
    if(ch == '\n') {
      if(this->rxBuffer.length() > 0) this->processLine(this->rxBuffer);
      this->rxBuffer = "";
      continue;
    }
    this->rxBuffer += ch;
    if(this->rxBuffer.length() > 512) this->rxBuffer = "";
  }
}

void DinplugBridge::printHelp(Stream &out) {
  out.println("{\"event\":\"dinplug_help\",\"commands\":[\"dinplug status\",\"dinplug host <host>\",\"dinplug auto <on|off>\",\"dinplug connect\",\"dinplug disconnect\",\"dinplug map list\",\"dinplug map clear\",\"dinplug map del <index>\",\"dinplug map add <keypadId> <buttonId> <press|release|hold|double> <shade|group> <targetId> <command|target|cycle> [value]\"]}");
}

void DinplugBridge::printStatus(Stream &out) {
  out.printf("{\"event\":\"dinplug_status\",\"host\":\"%s\",\"autoConnect\":%s,\"connected\":%s,\"mappings\":%u,\"status\":\"%s\"}\r\n",
             this->gatewayHost,
             this->autoConnect ? "true" : "false",
             this->client.connected() ? "true" : "false",
             this->mappingCount,
             this->connectionLabel().c_str());
}

void DinplugBridge::printMappings(Stream &out) {
  out.printf("{\"event\":\"dinplug_mappings\",\"count\":%u}\r\n", this->mappingCount);
  for(uint8_t i = 0; i < this->mappingCount; i++) {
    const Mapping &m = this->mappings[i];
    if(m.commandType == CommandTarget) {
      out.printf("{\"event\":\"dinplug_mapping\",\"index\":%u,\"keypad\":%u,\"button\":%u,\"action\":\"%s\",\"targetType\":\"%s\",\"targetId\":%u,\"command\":\"target\",\"value\":%d}\r\n",
                 i, m.keypadId, m.buttonId, this->actionToString(m.action),
                 this->targetTypeToString(m.targetType), m.targetId, m.value);
    }
    else {
      out.printf("{\"event\":\"dinplug_mapping\",\"index\":%u,\"keypad\":%u,\"button\":%u,\"action\":\"%s\",\"targetType\":\"%s\",\"targetId\":%u,\"command\":\"%s\"}\r\n",
                 i, m.keypadId, m.buttonId, this->actionToString(m.action),
                 this->targetTypeToString(m.targetType), m.targetId, this->commandTypeToString(m.commandType, m.command));
    }
  }
}

bool DinplugBridge::setGatewayHost(const char *host, String &message) {
  if(host == nullptr) {
    message = "{\"event\":\"error\",\"msg\":\"Missing host\"}";
    return false;
  }
  const String trimmed = String(host);
  if(trimmed.length() == 0 || trimmed.length() >= sizeof(this->gatewayHost)) {
    message = "{\"event\":\"error\",\"msg\":\"Invalid host\"}";
    return false;
  }
  strlcpy(this->gatewayHost, trimmed.c_str(), sizeof(this->gatewayHost));
  this->saveConfig();
  message = "{\"event\":\"dinplug\",\"msg\":\"Gateway host saved\"}";
  return true;
}

bool DinplugBridge::setAutoConnect(bool enabled, String &message) {
  this->autoConnect = enabled;
  this->saveConfig();
  message = String("{\"event\":\"dinplug\",\"msg\":\"Auto-connect ") + (enabled ? "enabled" : "disabled") + "\"}";
  return true;
}

bool DinplugBridge::connectNow(String &message) {
  if(this->gatewayHost[0] == '\0') {
    message = "{\"event\":\"error\",\"msg\":\"Gateway host not set\"}";
    return false;
  }
  const bool connected = this->ensureConnected(true);
  message = connected ? "{\"event\":\"dinplug\",\"msg\":\"Connected\"}" : "{\"event\":\"error\",\"msg\":\"Connect failed\"}";
  return connected;
}

bool DinplugBridge::disconnect(String &message) {
  this->client.stop();
  this->wasConnected = false;
  this->rxBuffer = "";
  message = "{\"event\":\"dinplug\",\"msg\":\"Disconnected\"}";
  return true;
}

bool DinplugBridge::clearMappings(String &message) {
  this->mappingCount = 0;
  this->saveConfig();
  message = "{\"event\":\"dinplug\",\"msg\":\"Mappings cleared\"}";
  return true;
}

bool DinplugBridge::removeMapping(uint8_t index, String &message) {
  if(index >= this->mappingCount) {
    message = "{\"event\":\"error\",\"msg\":\"Mapping index out of range\"}";
    return false;
  }
  for(uint8_t i = index; i + 1 < this->mappingCount; i++) this->mappings[i] = this->mappings[i + 1];
  if(this->mappingCount > 0) this->mappingCount--;
  this->saveConfig();
  message = "{\"event\":\"dinplug\",\"msg\":\"Mapping removed\"}";
  return true;
}

bool DinplugBridge::addMapping(uint16_t keypadId, uint16_t buttonId, const char *actionName,
                               const char *targetTypeName, uint8_t targetId,
                               const char *commandName, int16_t value, String &message) {
  if(keypadId == 0 || buttonId == 0 || targetId == 0 || commandName == nullptr) {
    message = "{\"event\":\"error\",\"msg\":\"Invalid mapping parameters\"}";
    return false;
  }
  if(this->mappingCount >= kMaxMappings) {
    message = "{\"event\":\"error\",\"msg\":\"Mapping capacity reached\"}";
    return false;
  }
  uint8_t action = ActionPress;
  uint8_t targetType = TargetShade;
  if(!this->parseAction(actionName, action)) {
    message = "{\"event\":\"error\",\"msg\":\"Invalid action\"}";
    return false;
  }
  if(!this->parseTargetType(targetTypeName, targetType)) {
    message = "{\"event\":\"error\",\"msg\":\"Invalid target type\"}";
    return false;
  }
  Mapping &m = this->mappings[this->mappingCount];
  m.keypadId = keypadId;
  m.buttonId = buttonId;
  m.action = action;
  m.targetType = targetType;
  m.targetId = targetId;
  if(strcasecmp(commandName, "target") == 0) {
    if(targetType != TargetShade) {
      message = "{\"event\":\"error\",\"msg\":\"Target position is only supported for shades\"}";
      return false;
    }
    m.commandType = CommandTarget;
    m.value = constrain(value, 0, 100);
    strlcpy(m.command, "target", sizeof(m.command));
  }
  else if(strcasecmp(commandName, "cycle") == 0 || strcasecmp(commandName, "shade_toggle") == 0) {
    if(targetType != TargetShade) {
      message = "{\"event\":\"error\",\"msg\":\"Cycle is only supported for shades\"}";
      return false;
    }
    m.commandType = CommandCycle;
    m.value = 0;
    strlcpy(m.command, "cycle", sizeof(m.command));
  }
  else {
    m.commandType = CommandSomfy;
    strlcpy(m.command, commandName, sizeof(m.command));
    m.value = 0;
  }
  this->mappingCount++;
  this->saveConfig();
  message = "{\"event\":\"dinplug\",\"msg\":\"Mapping added\"}";
  return true;
}

void DinplugBridge::toJSON(JsonObject obj) {
  obj["gatewayHost"] = this->gatewayHost;
  obj["autoConnect"] = this->autoConnect;
  obj["connected"] = this->client.connected();
  obj["mappingCount"] = this->mappingCount;
  obj["status"] = this->connectionLabel();
}

void DinplugBridge::mappingsToJSON(JsonArray arr) {
  for(uint8_t i = 0; i < this->mappingCount; i++) {
    const Mapping &m = this->mappings[i];
    JsonObject obj = arr.createNestedObject();
    obj["index"] = i;
    obj["keypadId"] = m.keypadId;
    obj["buttonId"] = m.buttonId;
    obj["action"] = this->actionToString(m.action);
    obj["targetType"] = this->targetTypeToString(m.targetType);
    obj["targetId"] = m.targetId;
    obj["command"] = this->commandTypeToString(m.commandType, m.command);
    if(m.commandType == CommandTarget) obj["value"] = m.value;
  }
}

bool DinplugBridge::loadConfig() {
  if(!LittleFS.exists(kConfigPath)) return true;
  File file = LittleFS.open(kConfigPath, "r");
  if(!file) return false;
  DynamicJsonDocument doc(8192);
  const DeserializationError err = deserializeJson(doc, file);
  file.close();
  if(err) {
    Serial.printf("dinplug: failed to load config: %s\n", err.c_str());
    return false;
  }
  strlcpy(this->gatewayHost, doc["gateway_host"] | "", sizeof(this->gatewayHost));
  this->autoConnect = doc["auto_connect"] | false;
  this->mappingCount = 0;
  JsonArray arr = doc["mappings"].as<JsonArray>();
  for(JsonObject obj : arr) {
    if(this->mappingCount >= kMaxMappings) break;
    Mapping &m = this->mappings[this->mappingCount++];
    m.keypadId = obj["keypad_id"] | 0;
    m.buttonId = obj["button_id"] | 0;
    m.action = obj["action"] | static_cast<uint8_t>(ActionPress);
    m.targetType = obj["target_type"] | static_cast<uint8_t>(TargetShade);
    m.targetId = obj["target_id"] | 0;
    m.commandType = obj["command_type"] | static_cast<uint8_t>(CommandSomfy);
    m.value = obj["value"] | 0;
    strlcpy(m.command, obj["command"] | "My", sizeof(m.command));
  }
  return true;
}

bool DinplugBridge::saveConfig() {
  DynamicJsonDocument doc(8192);
  doc["gateway_host"] = this->gatewayHost;
  doc["auto_connect"] = this->autoConnect;
  JsonArray arr = doc.createNestedArray("mappings");
  for(uint8_t i = 0; i < this->mappingCount; i++) {
    const Mapping &m = this->mappings[i];
    JsonObject obj = arr.createNestedObject();
    obj["keypad_id"] = m.keypadId;
    obj["button_id"] = m.buttonId;
    obj["action"] = m.action;
    obj["target_type"] = m.targetType;
    obj["target_id"] = m.targetId;
    obj["command_type"] = m.commandType;
    obj["command"] = m.command;
    obj["value"] = m.value;
  }
  File file = LittleFS.open(kConfigPath, "w");
  if(!file) return false;
  const size_t written = serializeJson(doc, file);
  file.close();
  return written > 0;
}

bool DinplugBridge::ensureConnected(bool forceNow) {
  if(this->client.connected()) {
    this->wasConnected = true;
    return true;
  }
  if(!net.connected()) return false;
  const unsigned long now = millis();
  if(!forceNow && (now - this->lastAttemptMs) < kReconnectIntervalMs) return false;
  this->lastAttemptMs = now;
  this->client.stop();
  Serial.printf("dinplug: connecting to %s:%u\n", this->gatewayHost, kDinplugPort);
  if(this->client.connect(this->gatewayHost, kDinplugPort)) {
    this->client.setNoDelay(true);
    this->wasConnected = true;
    this->rxBuffer = "";
    this->lastKeepAliveMs = millis();
    this->lastRxMs = millis();
    Serial.println("dinplug: connected");
    this->sendCommand("REFRESH");
    return true;
  }
  Serial.println("dinplug: connect failed");
  return false;
}

bool DinplugBridge::sendCommand(const String &cmd) {
  if(!this->client.connected()) return false;
  const size_t written = this->client.print(cmd + "\r\n");
  if(written == 0) {
    this->client.stop();
    this->wasConnected = false;
    Serial.println("dinplug: tx failed, reconnecting");
    return false;
  }
  return true;
}

void DinplugBridge::processLine(const String &line) {
  String trimmed = line;
  trimmed.trim();
  if(trimmed.length() == 0) return;
  if(trimmed.startsWith("R:")) trimmed = trimmed.substring(2);
  if(!trimmed.startsWith("BTN ")) return;

  const int firstSpace = trimmed.indexOf(' ');
  const int secondSpace = trimmed.indexOf(' ', firstSpace + 1);
  const int thirdSpace = trimmed.indexOf(' ', secondSpace + 1);
  if(firstSpace < 0 || secondSpace < 0 || thirdSpace < 0) return;

  const String actionName = trimmed.substring(firstSpace + 1, secondSpace);
  const uint16_t keypadId = static_cast<uint16_t>(trimmed.substring(secondSpace + 1, thirdSpace).toInt());
  const uint16_t buttonId = static_cast<uint16_t>(trimmed.substring(thirdSpace + 1).toInt());
  uint8_t action = ActionPress;
  if(!this->parseAction(actionName.c_str(), action)) return;
  this->handleButtonEvent(keypadId, buttonId, static_cast<ActionType>(action));
}

void DinplugBridge::handleButtonEvent(uint16_t keypadId, uint16_t buttonId, ActionType action) {
  for(uint8_t i = 0; i < this->mappingCount; i++) {
    const Mapping &m = this->mappings[i];
    if(m.keypadId != keypadId || m.buttonId != buttonId || m.action != action) continue;
    String detail;
    if(this->applyMapping(m, detail)) {
      Serial.printf("dinplug: applied mapping idx=%u keypad=%u button=%u action=%s %s\n",
                    i, keypadId, buttonId, this->actionToString(action), detail.c_str());
    }
    else {
      Serial.printf("dinplug: mapping idx=%u failed: %s\n", i, detail.c_str());
    }
  }
}

bool DinplugBridge::applyMapping(const Mapping &mapping, String &detail) {
  if(mapping.targetType == TargetShade) {
    SomfyShade *shade = somfy.getShadeById(mapping.targetId);
    if(!shade) {
      detail = "shade not found";
      return false;
    }
    if(mapping.commandType == CommandCycle) {
      somfy_commands cycleCmd = somfy_commands::My;
      if(shade->direction != 0) {
        cycleCmd = somfy_commands::My;
      }
      else if(shade->currentPos >= 100.0f) {
        cycleCmd = somfy_commands::Up;
      }
      else if(shade->currentPos <= 0.0f) {
        cycleCmd = somfy_commands::Down;
      }
      else if(shade->lastMovement < 0) {
        cycleCmd = somfy_commands::Down;
      }
      else if(shade->lastMovement > 0) {
        cycleCmd = somfy_commands::Up;
      }
      else {
        cycleCmd = somfy_commands::Down;
      }
      shade->sendCommand(cycleCmd, shade->repeats);
      shade->emitState();
      detail = "cycle cmd " + translateSomfyCommand(cycleCmd);
      return true;
    }
    if(mapping.commandType == CommandTarget) {
      shade->moveToTarget(shade->transformPosition(constrain(mapping.value, 0, 100)));
      shade->emitState();
      detail = "target " + String(mapping.value);
      return true;
    }
    const somfy_commands cmd = translateSomfyCommand(String(mapping.command));
    shade->sendCommand(cmd, shade->repeats);
    shade->emitState();
    detail = "shade cmd " + String(mapping.command);
    return true;
  }

  SomfyGroup *group = somfy.getGroupById(mapping.targetId);
  if(!group) {
    detail = "group not found";
    return false;
  }
  if(mapping.commandType == CommandTarget) {
    detail = "groups do not support target positions";
    return false;
  }
  const somfy_commands cmd = translateSomfyCommand(String(mapping.command));
  group->sendCommand(cmd, group->repeats);
  group->emitState();
  detail = "group cmd " + String(mapping.command);
  return true;
}

const char *DinplugBridge::actionToString(uint8_t action) const {
  switch(action) {
    case ActionPress: return "press";
    case ActionRelease: return "release";
    case ActionHold: return "hold";
    case ActionDouble: return "double";
    default: return "press";
  }
}

const char *DinplugBridge::targetTypeToString(uint8_t targetType) const {
  switch(targetType) {
    case TargetShade: return "shade";
    case TargetGroup: return "group";
    default: return "shade";
  }
}

const char *DinplugBridge::commandTypeToString(uint8_t commandType, const char *command) const {
  if(commandType == CommandTarget) return "target";
  if(commandType == CommandCycle) return "cycle";
  return command;
}

bool DinplugBridge::parseAction(const char *name, uint8_t &action) const {
  if(name == nullptr) return false;
  if(strcasecmp(name, "press") == 0) { action = ActionPress; return true; }
  if(strcasecmp(name, "release") == 0) { action = ActionRelease; return true; }
  if(strcasecmp(name, "hold") == 0) { action = ActionHold; return true; }
  if(strcasecmp(name, "double") == 0) { action = ActionDouble; return true; }
  return false;
}

bool DinplugBridge::parseTargetType(const char *name, uint8_t &targetType) const {
  if(name == nullptr) return false;
  if(strcasecmp(name, "shade") == 0) { targetType = TargetShade; return true; }
  if(strcasecmp(name, "group") == 0) { targetType = TargetGroup; return true; }
  return false;
}

String DinplugBridge::connectionLabel() {
  if(this->gatewayHost[0] == '\0') return "gateway not set";
  if(this->client.connected()) return "connected";
  if(this->autoConnect) return "auto-connect enabled, disconnected";
  return "configured, disconnected";
}
