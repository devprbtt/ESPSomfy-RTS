#include <ctype.h>
#include "Telnet.h"
#include "ConfigSettings.h"

extern SomfyShadeController somfy;
extern Network net;

static const uint32_t TELNET_IDLE_TIMEOUT = 300000; // 5 minutes

TelnetControl::TelnetControl(uint16_t port) : server(port) {}

void TelnetControl::begin() {
  this->server.begin();
  this->server.setNoDelay(true);
}

void TelnetControl::end() {
  if(this->client) this->client.stop();
  this->server.close();
}

bool TelnetControl::hasConnection() {
  return net.connected() || net.softAPOpened;
}

uint8_t TelnetControl::tokenize(const String &line, String *tokens, uint8_t maxTokens) {
  uint8_t count = 0;
  int start = 0;
  while(start < line.length() && count < maxTokens) {
    while(start < line.length() && line[start] == ' ') start++;
    int end = line.indexOf(' ', start);
    if(end == -1) end = line.length();
    if(end > start) tokens[count++] = line.substring(start, end);
    start = end + 1;
  }
  return count;
}

bool TelnetControl::parseCommandToken(const String &token, somfy_commands &cmd) {
  static const char *validCommands[] = {
    "up", "down", "my", "stop", "toggle", "prog", "favorite", "fav", "sunflag", "flag",
    "stepup", "stepdown", "sensor"
  };
  for(auto val : validCommands) {
    if(token.equalsIgnoreCase(val)) {
      cmd = translateSomfyCommand(token);
      return true;
    }
  }
  return false;
}

void TelnetControl::prompt() {
  if(this->client && this->client.connected()) this->client.print(F("> "));
}

void TelnetControl::printHelp() {
  this->client.println(F("ESPSomfy-RTS Telnet Control"));
  this->client.println(F("Commands:"));
  this->client.println(F("  help                   Show this help"));
  this->client.println(F("  list                   List shades and groups"));
  this->client.println(F("  shades                 List shades"));
  this->client.println(F("  groups                 List groups"));
  this->client.println(F("  <shadeId> <cmd> [rep]  Send command to shade"));
  this->client.println(F("  shade <id> <cmd> [rep] Same as above"));
  this->client.println(F("  group <id> <cmd> [rep] Send command to group"));
  this->client.println(F("  <shadeId> pos <0-100>  Move shade to percentage"));
  this->client.println(F("  quit/exit              Close the session"));
  this->prompt();
}

void TelnetControl::listShades() {
  this->client.println(F("Shades:"));
  for(uint8_t i = 0; i < SOMFY_MAX_SHADES; i++) {
    SomfyShade *shade = &somfy.shades[i];
    if(shade->getShadeId() == 255) continue;
    this->client.printf("  %u: %s (pos %.1f%%)", shade->getShadeId(), shade->name, shade->currentPos);
    if(shade->tiltType != tilt_types::none) this->client.printf(" tilt %.1f%%", shade->currentTiltPos);
    this->client.println();
  }
}

void TelnetControl::listGroups() {
  this->client.println(F("Groups:"));
  for(uint8_t i = 0; i < SOMFY_MAX_GROUPS; i++) {
    SomfyGroup *group = &somfy.groups[i];
    if(group->getGroupId() == 255) continue;
    this->client.printf("  %u: %s\n", group->getGroupId(), group->name);
  }
}

void TelnetControl::executeShadeCommand(const String &shadeToken, const String &cmdToken, const String &argToken) {
  if(!cmdToken.length()) {
    this->client.println(F("Shade command missing."));
    return;
  }
  uint8_t shadeId = shadeToken.toInt();
  SomfyShade *shade = somfy.getShadeById(shadeId);
  if(!shade) {
    this->client.printf("Shade %u not found\n", shadeId);
    return;
  }
  if(cmdToken.equalsIgnoreCase("pos") || cmdToken.equalsIgnoreCase("position")) {
    if(!argToken.length()) {
      this->client.println(F("Position command requires a value between 0 and 100."));
      return;
    }
    float pos = argToken.toFloat();
    if(pos < 0.0f || pos > 100.0f) {
      this->client.println(F("Position must be between 0 and 100."));
      return;
    }
    shade->moveToTarget(pos);
    this->client.printf("Moving shade %u (%s) to %.1f%%\n", shadeId, shade->name, pos);
    return;
  }
  somfy_commands cmd;
  if(!this->parseCommandToken(cmdToken, cmd)) {
    this->client.printf("Unknown command '%s'\n", cmdToken.c_str());
    return;
  }
  uint8_t repeat = shade->repeats;
  if(argToken.length()) {
    int r = argToken.toInt();
    if(r > 0) repeat = static_cast<uint8_t>(r);
  }
  shade->sendCommand(cmd, repeat);
  shade->emitState();
  this->client.printf("Sent %s to shade %u (%s)\n", cmdToken.c_str(), shadeId, shade->name);
}

void TelnetControl::executeGroupCommand(const String &groupToken, const String &cmdToken, const String &argToken) {
  if(!cmdToken.length()) {
    this->client.println(F("Group command missing."));
    return;
  }
  uint8_t groupId = groupToken.toInt();
  SomfyGroup *group = somfy.getGroupById(groupId);
  if(!group) {
    this->client.printf("Group %u not found\n", groupId);
    return;
  }
  somfy_commands cmd;
  if(!this->parseCommandToken(cmdToken, cmd)) {
    this->client.printf("Unknown command '%s'\n", cmdToken.c_str());
    return;
  }
  uint8_t repeat = group->repeats;
  if(argToken.length()) {
    int r = argToken.toInt();
    if(r > 0) repeat = static_cast<uint8_t>(r);
  }
  group->sendCommand(cmd, repeat);
  group->emitState();
  this->client.printf("Sent %s to group %u (%s)\n", cmdToken.c_str(), groupId, group->name);
}

void TelnetControl::handleLine(String line) {
  line.trim();
  if(!line.length()) { this->prompt(); return; }

  String tokens[4];
  uint8_t count = this->tokenize(line, tokens, 4);
  if(count == 0) { this->prompt(); return; }

  String action = tokens[0];
  action.toLowerCase();

  if(action == "help" || action == "?") { this->printHelp(); return; }
  if(action == "quit" || action == "exit") { this->client.println(F("Closing connection.")); this->client.stop(); return; }
  if(action == "list") { this->listShades(); this->listGroups(); this->prompt(); return; }
  if(action == "shades") { this->listShades(); this->prompt(); return; }
  if(action == "groups") { this->listGroups(); this->prompt(); return; }
  if(action == "shade" && count >= 3) { this->executeShadeCommand(tokens[1], tokens[2], count >= 4 ? tokens[3] : ""); this->prompt(); return; }
  if(action == "group" && count >= 3) { this->executeGroupCommand(tokens[1], tokens[2], count >= 4 ? tokens[3] : ""); this->prompt(); return; }
  if(isDigit(tokens[0][0])) { this->executeShadeCommand(tokens[0], count >= 2 ? tokens[1] : "", count >= 3 ? tokens[2] : ""); this->prompt(); return; }

  this->client.println(F("Unrecognized command. Type 'help' for options."));
  this->prompt();
}

void TelnetControl::loop() {
  if(!this->hasConnection()) {
    if(this->client) this->client.stop();
    return;
  }
  if(this->server.hasClient()) {
    WiFiClient newClient = this->server.available();
    if(newClient) {
      if(this->client && this->client.connected()) {
        newClient.println(F("Another telnet session is already active."));
        newClient.stop();
      }
      else {
        this->client.stop();
        this->client = newClient;
        this->buffer = "";
        this->lastActivity = millis();
        this->client.println(F("Connected to ESPSomfy-RTS"));
        this->printHelp();
      }
    }
  }

  if(this->client && this->client.connected()) {
    while(this->client.available()) {
      char c = this->client.read();
      this->lastActivity = millis();
      if(c == '\r') continue;
      if(c == '\n') {
        this->handleLine(this->buffer);
        this->buffer = "";
      }
      else {
        this->buffer += c;
        if(this->buffer.length() > 255) this->buffer.remove(0, this->buffer.length() - 255);
      }
    }
    if(millis() - this->lastActivity > TELNET_IDLE_TIMEOUT) {
      this->client.println(F("Session timed out."));
      this->client.stop();
    }
  }
}
