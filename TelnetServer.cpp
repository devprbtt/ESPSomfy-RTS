#include <Arduino.h>
#include <ctype.h>
#include <esp_task_wdt.h>
#include "TelnetServer.h"

extern SomfyShadeController somfy;

TelnetServer::TelnetServer() : server(23) {
  for(auto &c : this->clients) this->resetInput(c);
}
void TelnetServer::begin() {
  this->server.begin();
  this->server.setNoDelay(true);
  Serial.println("Telnet server listening on port 23...");
}
void TelnetServer::resetInput(TelnetClient &c) {
  memset(c.inputBuffer, 0x00, sizeof(c.inputBuffer));
  c.inputLength = 0;
}
bool TelnetServer::parseId(const char *token, uint8_t *outId) {
  if(!token || !outId) return false;
  long val = strtol(token, nullptr, 10);
  if(val < 0 || val > 255) return false;
  *outId = static_cast<uint8_t>(val);
  return true;
}
void TelnetServer::printShadeJson(TelnetClient &c, SomfyShade *shade, const char *evt) {
  if(!c.client || !c.client.connected() || !shade) return;
  const int8_t pos = shade->transformPosition(shade->currentPos);
  const int8_t target = shade->transformPosition(shade->target);
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
    "{\"event\":\"%s\",\"id\":%u,\"name\":\"%s\",\"pos\":%d,\"target\":%d,\"dir\":%d,\"addr\":%lu,\"flags\":%u",
    evt, shade->getShadeId(), shade->name, pos, target, shade->direction,
    static_cast<unsigned long>(shade->getRemoteAddress()), shade->flags);
  if(shade->tiltType != tilt_types::none) {
    const int8_t tiltPos = shade->transformPosition(shade->currentTiltPos);
    const int8_t tiltTarget = shade->transformPosition(shade->tiltTarget);
    n += snprintf(&buf[n], sizeof(buf) - n, ",\"tiltPos\":%d,\"tiltTarget\":%d,\"tiltDir\":%d",
      tiltPos, tiltTarget, shade->tiltDirection);
  }
  n += snprintf(&buf[n], sizeof(buf) - n, "}");
  if(n < (int)sizeof(buf)) {
    buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = '\0';
  }
  c.client.write((const uint8_t *)buf, strlen(buf));
}
void TelnetServer::printAllShades(TelnetClient &c) {
  if(!c.client || !c.client.connected()) return;
  uint8_t count = 0;
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &somfy.shades[i];
    if(shade && shade->getShadeId() != 255) {
      this->printShadeJson(c, shade);
      count++;
    }
  }
  if(count == 0) c.client.println("{\"event\":\"info\",\"msg\":\"No shades configured\"}");
}
void TelnetServer::printHelp(TelnetClient &c) {
  if(!c.client || !c.client.connected()) return;
  this->sendJson(c, "{\"event\":\"help\",\"commands\":[\"list\",\"shade <id>\",\"target <id> <0-100>\",\"cmd <id> <cmd> [repeat] [step]\",\"exit\"]}");
}
void TelnetServer::handleLine(TelnetClient &tc, char *line) {
  if(!line || !tc.client || !tc.client.connected()) return;
  while(*line && isspace(static_cast<unsigned char>(*line))) line++;
  if(*line == '\0') return;
  char *cmd = strtok(line, " ");
  if(!cmd) return;
  for(char *p = cmd; *p; ++p) *p = tolower(*p);
  if(strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    this->printHelp(tc);
    return;
  }
  else if(strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
    this->printAllShades(tc);
    return;
  }
  else if(strcmp(cmd, "shade") == 0 || strcmp(cmd, "get") == 0 || strcmp(cmd, "show") == 0) {
    char *idTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !this->parseId(idTok, &shadeId)) {
      this->sendJson(tc, "{\"event\":\"error\",\"msg\":\"Usage: shade <shadeId>\"}");
      return;
    }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      this->sendJson(tc, "{\"event\":\"error\",\"msg\":\"Shade not found\"}");
      return;
    }
    this->printShadeJson(tc, shade);
    return;
  }
  else if(strcmp(cmd, "target") == 0 || strcmp(cmd, "set") == 0 || strcmp(cmd, "goto") == 0) {
    char *idTok = strtok(nullptr, " ");
    char *targetTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !targetTok || !this->parseId(idTok, &shadeId)) {
      this->sendJson(tc, "{\"event\":\"error\",\"msg\":\"Usage: target <shadeId> <0-100>\"}");
      return;
    }
    int target = atoi(targetTok);
    target = constrain(target, 0, 100);
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      this->sendJson(tc, "{\"event\":\"error\",\"msg\":\"Shade not found\"}");
      return;
    }
    shade->moveToTarget(shade->transformPosition(target));
    this->sendJsonf(tc, "{\"event\":\"command\",\"id\":%u,\"target\":%d}", shadeId, target);
    this->printShadeJson(tc, shade, "update");
    return;
  }
  else if(strcmp(cmd, "cmd") == 0 || strcmp(cmd, "send") == 0 || strcmp(cmd, "control") == 0) {
    char *idTok = strtok(nullptr, " ");
    char *commandTok = strtok(nullptr, " ");
    char *repeatTok = strtok(nullptr, " ");
    char *stepTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !commandTok || !this->parseId(idTok, &shadeId)) {
      tc.client.println("{\"event\":\"error\",\"msg\":\"Usage: cmd <shadeId> <command> [repeat] [stepSize]\"}");
      return;
    }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      tc.client.println("{\"event\":\"error\",\"msg\":\"Shade not found\"}");
      return;
    }
    somfy_commands cmdVal = translateSomfyCommand(String(commandTok));
    uint8_t repeat = repeatTok ? static_cast<uint8_t>(atoi(repeatTok)) : shade->repeats;
    uint8_t stepSize = stepTok ? static_cast<uint8_t>(atoi(stepTok)) : 0;
    shade->sendCommand(cmdVal, repeat > 0 ? repeat : shade->repeats, stepSize);
    this->sendJsonf(tc, "{\"event\":\"command\",\"id\":%u,\"cmd\":\"%s\",\"repeat\":%u,\"step\":%u}",
      shadeId, commandTok, repeat > 0 ? repeat : shade->repeats, stepSize);
    this->printShadeJson(tc, shade, "update");
    return;
  }
  else if(strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "bye") == 0) {
    this->sendJson(tc, "{\"event\":\"bye\"}");
    tc.client.stop();
    this->resetInput(tc);
    return;
  }
  this->sendJson(tc, "{\"event\":\"error\",\"msg\":\"Unknown command\"}");
}
void TelnetServer::loop() {
  esp_task_wdt_reset();
  // Accept new connections
  WiFiClient incoming = this->server.available();
  if(incoming) {
    for(auto &c : this->clients) {
      if(!c.client || !c.client.connected()) {
        c.client = incoming;
        c.client.setNoDelay(true);
        this->resetInput(c);
        c.lastActivity = millis();
        this->sendJson(c, "{\"event\":\"welcome\",\"msg\":\"ESPSomfy RTS telnet\"}");
        this->printAllShades(c);
        break;
      }
    }
  }
  // Handle input and timeouts
  for(auto &c : this->clients) {
    if(!c.client || !c.client.connected()) continue;
    while(c.client.connected() && c.client.available()) {
      char ch = c.client.read();
      if(ch == '\r') continue;
      if(ch == '\n') {
        c.inputBuffer[c.inputLength] = '\0';
        this->handleLine(c, c.inputBuffer);
        this->resetInput(c);
      }
      else if(ch == 0x08 || ch == 0x7F) {
        if(c.inputLength > 0) c.inputLength--;
      }
      else if(c.inputLength < sizeof(c.inputBuffer) - 1 && isprint(static_cast<unsigned char>(ch))) {
        c.inputBuffer[c.inputLength++] = ch;
      }
      c.lastActivity = millis();
    }
    if(c.client.connected() && millis() - c.lastActivity > 600000UL) {
      c.client.println("{\"event\":\"bye\",\"reason\":\"timeout\"}");
      c.client.stop();
      this->resetInput(c);
    }
  }
  this->emitLiveUpdates();
}

void TelnetServer::sendJson(TelnetClient &c, const char *json) {
  if(!c.client || !c.client.connected() || !json) return;
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "%s\r\n", json);
  if(n > 0) c.client.write((const uint8_t *)buf, (size_t)n);
}
void TelnetServer::sendJsonf(TelnetClient &c, const char *fmt, ...) {
  if(!c.client || !c.client.connected() || !fmt) return;
  char payload[240];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(payload, sizeof(payload), fmt, args);
  va_end(args);
  if(n <= 0) return;
  char buf[256];
  n = snprintf(buf, sizeof(buf), "%s\r\n", payload);
  if(n > 0) c.client.write((const uint8_t *)buf, (size_t)n);
}

void TelnetServer::snapshotShade(ShadeSnapshot &snap, SomfyShade *shade) {
  snap.present = shade != nullptr;
  if(!shade) return;
  snap.shadeId = shade->getShadeId();
  snap.remoteAddress = shade->getRemoteAddress();
  snap.position = shade->transformPosition(shade->currentPos);
  snap.target = shade->transformPosition(shade->target);
  snap.direction = shade->direction;
  snap.flags = shade->flags;
  strlcpy(snap.name, shade->name, sizeof(snap.name));
  snap.tiltType = shade->tiltType;
  if(shade->tiltType != tilt_types::none) {
    snap.tiltPosition = shade->transformPosition(shade->currentTiltPos);
    snap.tiltTarget = shade->transformPosition(shade->tiltTarget);
    snap.tiltDirection = shade->tiltDirection;
  }
  else {
    snap.tiltPosition = snap.tiltTarget = -1;
    snap.tiltDirection = 0;
  }
}
bool TelnetServer::hasShadeChanged(ShadeSnapshot &snap, SomfyShade *shade) {
  if(!shade) return snap.present;
  const int8_t pos = shade->transformPosition(shade->currentPos);
  const int8_t tgt = shade->transformPosition(shade->target);
  const tilt_types ttype = shade->tiltType;
  const int8_t tiltPos = ttype != tilt_types::none ? shade->transformPosition(shade->currentTiltPos) : -1;
  const int8_t tiltTgt = ttype != tilt_types::none ? shade->transformPosition(shade->tiltTarget) : -1;
  if(!snap.present ||
     snap.shadeId != shade->getShadeId() ||
     snap.remoteAddress != shade->getRemoteAddress() ||
     snap.position != pos ||
     snap.target != tgt ||
     snap.direction != shade->direction ||
     snap.flags != shade->flags ||
     snap.tiltType != ttype ||
     snap.tiltDirection != shade->tiltDirection ||
     snap.tiltPosition != tiltPos ||
     snap.tiltTarget != tiltTgt ||
     strncmp(snap.name, shade->name, sizeof(snap.name)) != 0) {
       return true;
  }
  return false;
}
void TelnetServer::emitLiveUpdates() {
  bool anyClient = false;
  for(auto &c : this->clients) if(c.client && c.client.connected()) { anyClient = true; break; }
  if(!anyClient) return;
  // Throttle to ~10 updates/sec to avoid flooding idle loops.
  if(millis() - this->lastEmit < 100) return;
  this->lastEmit = millis();
  bool seen[SOMFY_MAX_SHADES] = {false};
  // Check for additions/changes
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &somfy.shades[i];
    if(!shade || shade->getShadeId() == 255) continue;
    uint8_t sid = shade->getShadeId();
    if(sid >= SOMFY_MAX_SHADES) continue;
    seen[sid] = true;
    ShadeSnapshot &snap = this->snapshots[sid];
    if(this->hasShadeChanged(snap, shade)) {
      for(auto &c : this->clients) if(c.client && c.client.connected()) this->printShadeJson(c, shade, "update");
      this->snapshotShade(snap, shade);
    }
  }
  // Check for removals
  for(uint8_t id = 0; id < SOMFY_MAX_SHADES; id++) {
    ShadeSnapshot &snap = this->snapshots[id];
    if(snap.present && !seen[id]) {
      for(auto &c : this->clients) if(c.client && c.client.connected())
        c.client.printf("{\"event\":\"removed\",\"id\":%u,\"name\":\"%s\"}\r\n", snap.shadeId, snap.name);
      snap.present = false;
    }
  }
}
