#ifndef TELNET_SERVER_H
#define TELNET_SERVER_H

#include <WiFi.h>
#include "Somfy.h"

class TelnetServer {
  public:
    TelnetServer();
    void begin();
    void loop();
  private:
    WiFiServer server;
    WiFiClient client;
    char inputBuffer[128];
    size_t inputLength = 0;
    uint32_t lastActivity = 0;
    struct ShadeSnapshot {
      bool present = false;
      uint8_t shadeId = 255;
      uint32_t remoteAddress = 0;
      int8_t position = -1;
      int8_t target = -1;
      int8_t direction = 0;
      int8_t tiltPosition = -1;
      int8_t tiltTarget = -1;
      int8_t tiltDirection = 0;
      uint8_t flags = 0;
      char name[21] = "";
      tilt_types tiltType = tilt_types::none;
    } snapshots[SOMFY_MAX_SHADES];
    void resetInput();
    void handleLine(char *line);
    void sendPrompt();
    void printHelp();
    void printAllShades();
    void printShadeState(SomfyShade *shade);
    void printShadeJson(SomfyShade *shade, const char *evt = "state");
    bool parseId(const char *token, uint8_t *outId);
    void emitLiveUpdates();
    void snapshotShade(ShadeSnapshot &snap, SomfyShade *shade);
    bool hasShadeChanged(ShadeSnapshot &snap, SomfyShade *shade);
};

#endif
