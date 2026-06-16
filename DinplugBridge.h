#ifndef DINPLUG_BRIDGE_H
#define DINPLUG_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

class DinplugBridge {
  public:
    DinplugBridge();
    void begin();
    void loop();
    void printHelp(Stream &out);
    void printStatus(Stream &out);
    void printMappings(Stream &out);
    bool setGatewayHost(const char *host, String &message);
    bool setAutoConnect(bool enabled, String &message);
    bool connectNow(String &message);
    bool disconnect(String &message);
    bool clearMappings(String &message);
    bool removeMapping(uint8_t index, String &message);
    bool addMapping(uint16_t keypadId, uint16_t buttonId, const char *actionName,
                    const char *targetTypeName, uint8_t targetId,
                    const char *commandName, int16_t value, String &message);
    void toJSON(JsonObject obj);
    void mappingsToJSON(JsonArray arr);

  private:
    static const uint16_t kDinplugPort = 23;
    static const uint8_t kMaxMappings = 48;
    static const unsigned long kReconnectIntervalMs = 5000;
    static const unsigned long kKeepAliveIntervalMs = 10000;
    static const unsigned long kRxTimeoutMs = 25000;
    static const char *kConfigPath;

    enum ActionType : uint8_t {
      ActionPress = 0,
      ActionRelease = 1,
      ActionHold = 2,
      ActionDouble = 3
    };
    enum TargetType : uint8_t {
      TargetShade = 0,
      TargetGroup = 1
    };
    enum CommandType : uint8_t {
      CommandSomfy = 0,
      CommandTarget = 1,
      CommandCycle = 2
    };
    struct Mapping {
      uint16_t keypadId = 0;
      uint16_t buttonId = 0;
      uint8_t action = ActionPress;
      uint8_t targetType = TargetShade;
      uint8_t targetId = 0;
      uint8_t commandType = CommandSomfy;
      int16_t value = 0;
      char command[16] = "My";
    };

    WiFiClient client;
    String rxBuffer;
    char gatewayHost[65];
    bool autoConnect = false;
    bool wasConnected = false;
    unsigned long lastAttemptMs = 0;
    unsigned long lastKeepAliveMs = 0;
    unsigned long lastRxMs = 0;
    Mapping mappings[kMaxMappings];
    uint8_t mappingCount = 0;

    bool loadConfig();
    bool saveConfig();
    bool ensureConnected(bool forceNow);
    bool sendCommand(const String &cmd);
    void processLine(const String &line);
    void handleButtonEvent(uint16_t keypadId, uint16_t buttonId, ActionType action);
    bool applyMapping(const Mapping &mapping, String &detail);
    const char *actionToString(uint8_t action) const;
    const char *targetTypeToString(uint8_t targetType) const;
    const char *commandTypeToString(uint8_t commandType, const char *command) const;
    bool parseAction(const char *name, uint8_t &action) const;
    bool parseTargetType(const char *name, uint8_t &targetType) const;
    String connectionLabel();
};

extern DinplugBridge dinplugBridge;

#endif
