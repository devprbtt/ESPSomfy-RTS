#include <Arduino.h>
#include <WiFi.h>
#include "Somfy.h"
#include "Network.h"

#ifndef TELNET_H
#define TELNET_H

class TelnetControl {
  protected:
    WiFiServer server;
    WiFiClient client;
    String buffer;
    uint32_t lastActivity = 0;
    uint8_t tokenize(const String &line, String *tokens, uint8_t maxTokens);
    bool parseCommandToken(const String &token, somfy_commands &cmd);
    void prompt();
    void printHelp();
    void listShades();
    void listGroups();
    void executeShadeCommand(const String &shadeToken, const String &cmdToken, const String &argToken);
    void executeGroupCommand(const String &groupToken, const String &cmdToken, const String &argToken);
    void handleLine(String line);
    bool hasConnection();
  public:
    explicit TelnetControl(uint16_t port = 23);
    void begin();
    void loop();
    void end();
};

#endif
