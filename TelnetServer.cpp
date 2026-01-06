#include <Arduino.h>
#include <ctype.h>
#include <esp_task_wdt.h>
#include "TelnetServer.h"

extern SomfyShadeController somfy;

TelnetServer::TelnetServer() : server(23) {
  this->resetInput();
}
void TelnetServer::begin() {
  this->server.begin();
  this->server.setNoDelay(true);
  Serial.println("Telnet server listening on port 23...");
}
void TelnetServer::resetInput() {
  memset(this->inputBuffer, 0x00, sizeof(this->inputBuffer));
  this->inputLength = 0;
}
void TelnetServer::sendPrompt() {
  if(this->client && this->client.connected()) this->client.print("> ");
}
bool TelnetServer::parseId(const char *token, uint8_t *outId) {
  if(!token || !outId) return false;
  long val = strtol(token, nullptr, 10);
  if(val < 0 || val > 255) return false;
  *outId = static_cast<uint8_t>(val);
  return true;
}
void TelnetServer::printShadeState(SomfyShade *shade) {
  if(!this->client || !this->client.connected() || !shade) return;
  const int8_t pos = shade->transformPosition(shade->currentPos);
  const int8_t target = shade->transformPosition(shade->target);
  this->client.printf("#%u %-20s pos:%3d%% tgt:%3d%% dir:%2d addr:%lu flags:0x%02X\r\n",
    shade->getShadeId(),
    shade->name,
    pos,
    target,
    shade->direction,
    static_cast<unsigned long>(shade->getRemoteAddress()),
    shade->flags);
  if(shade->tiltType != tilt_types::none) {
    const int8_t tiltPos = shade->transformPosition(shade->currentTiltPos);
    const int8_t tiltTarget = shade->transformPosition(shade->tiltTarget);
    this->client.printf("    tilt pos:%3d%% tgt:%3d%% dir:%2d\r\n", tiltPos, tiltTarget, shade->tiltDirection);
  }
}
void TelnetServer::printAllShades() {
  if(!this->client || !this->client.connected()) return;
  uint8_t count = 0;
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &somfy.shades[i];
    if(shade && shade->getShadeId() != 255) {
      this->printShadeState(shade);
      count++;
    }
  }
  if(count == 0) this->client.println("No shades configured.");
}
void TelnetServer::printHelp() {
  if(!this->client || !this->client.connected()) return;
  this->client.println("Available commands:");
  this->client.println("  list                         - show all shades and their state");
  this->client.println("  shade <id>                   - show details for a shade");
  this->client.println("  target <id> <0-100>          - move shade to percentage target");
  this->client.println("  cmd <id> <cmd> [repeat] [step]- send Somfy command (up/down/my/stop/prog/favorite/stepup/stepdown/toggle)");
  this->client.println("  exit                         - close the telnet session");
}
void TelnetServer::handleLine(char *line) {
  if(!line || !this->client || !this->client.connected()) return;
  while(*line && isspace(static_cast<unsigned char>(*line))) line++;
  if(*line == '\0') return;
  char *cmd = strtok(line, " ");
  if(!cmd) return;
  for(char *p = cmd; *p; ++p) *p = tolower(*p);
  if(strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    this->printHelp();
    return;
  }
  else if(strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
    this->printAllShades();
    return;
  }
  else if(strcmp(cmd, "shade") == 0 || strcmp(cmd, "get") == 0 || strcmp(cmd, "show") == 0) {
    char *idTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !this->parseId(idTok, &shadeId)) {
      this->client.println("Usage: shade <shadeId>");
      return;
    }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      this->client.println("Shade not found.");
      return;
    }
    this->printShadeState(shade);
    return;
  }
  else if(strcmp(cmd, "target") == 0 || strcmp(cmd, "set") == 0 || strcmp(cmd, "goto") == 0) {
    char *idTok = strtok(nullptr, " ");
    char *targetTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !targetTok || !this->parseId(idTok, &shadeId)) {
      this->client.println("Usage: target <shadeId> <0-100>");
      return;
    }
    int target = atoi(targetTok);
    target = constrain(target, 0, 100);
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      this->client.println("Shade not found.");
      return;
    }
    shade->moveToTarget(shade->transformPosition(target));
    this->client.printf("Shade %u moving to %d%%\r\n", shadeId, target);
    this->printShadeState(shade);
    return;
  }
  else if(strcmp(cmd, "cmd") == 0 || strcmp(cmd, "send") == 0 || strcmp(cmd, "control") == 0) {
    char *idTok = strtok(nullptr, " ");
    char *commandTok = strtok(nullptr, " ");
    char *repeatTok = strtok(nullptr, " ");
    char *stepTok = strtok(nullptr, " ");
    uint8_t shadeId = 255;
    if(!idTok || !commandTok || !this->parseId(idTok, &shadeId)) {
      this->client.println("Usage: cmd <shadeId> <command> [repeat] [stepSize]");
      return;
    }
    SomfyShade *shade = somfy.getShadeById(shadeId);
    if(!shade) {
      this->client.println("Shade not found.");
      return;
    }
    somfy_commands cmdVal = translateSomfyCommand(String(commandTok));
    uint8_t repeat = repeatTok ? static_cast<uint8_t>(atoi(repeatTok)) : shade->repeats;
    uint8_t stepSize = stepTok ? static_cast<uint8_t>(atoi(stepTok)) : 0;
    shade->sendCommand(cmdVal, repeat > 0 ? repeat : shade->repeats, stepSize);
    this->client.printf("Sent %s to shade %u (repeat %u", commandTok, shadeId, repeat > 0 ? repeat : shade->repeats);
    if(stepSize > 0) this->client.printf(", step %u", stepSize);
    this->client.println(")");
    this->printShadeState(shade);
    return;
  }
  else if(strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "bye") == 0) {
    this->client.println("Closing session.");
    this->client.stop();
    this->resetInput();
    return;
  }
  this->client.println("Unknown command. Type 'help' for options.");
}
void TelnetServer::loop() {
  esp_task_wdt_reset();
  if(!this->client || !this->client.connected()) {
    if(this->client) this->client.stop();
    WiFiClient newClient = this->server.available();
    if(newClient) {
      this->client = newClient;
      this->resetInput();
      this->lastActivity = millis();
      this->client.println();
      this->client.println("Welcome to the ESPSomfy RTS telnet console.");
      this->client.println("Type 'help' to list commands.");
      this->printAllShades();
      this->sendPrompt();
    }
    return;
  }
  while(this->client.connected() && this->client.available()) {
    char c = this->client.read();
    if(c == '\r') continue;
    if(c == '\n') {
      this->inputBuffer[this->inputLength] = '\0';
      this->handleLine(this->inputBuffer);
      this->resetInput();
      if(this->client && this->client.connected()) this->sendPrompt();
    }
    else if(c == 0x08 || c == 0x7F) {
      if(this->inputLength > 0) this->inputLength--;
    }
    else if(this->inputLength < sizeof(this->inputBuffer) - 1 && isprint(static_cast<unsigned char>(c))) {
      this->inputBuffer[this->inputLength++] = c;
    }
    this->lastActivity = millis();
  }
  // Push live updates while connected.
  if(this->client.connected()) this->emitLiveUpdates();
  if(this->client.connected() && millis() - this->lastActivity > 600000UL) {
    this->client.println("Closing inactive session.");
    this->client.stop();
    this->resetInput();
  }
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
  if(!this->client || !this->client.connected()) return;
  static uint32_t lastEmit = 0;
  // Throttle to ~10 updates/sec to avoid flooding idle loops.
  if(millis() - lastEmit < 100) return;
  lastEmit = millis();
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
      this->client.print("UPDATE ");
      this->printShadeState(shade);
      this->snapshotShade(snap, shade);
    }
  }
  // Check for removals
  for(uint8_t id = 0; id < SOMFY_MAX_SHADES; id++) {
    ShadeSnapshot &snap = this->snapshots[id];
    if(snap.present && !seen[id]) {
      this->client.printf("REMOVED #%u %s\r\n", snap.shadeId, snap.name);
      snap.present = false;
    }
  }
}
