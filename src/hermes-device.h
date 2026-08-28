/**
 * @file hermes-device.h
 * @brief Hermes Studio Device Protocol Library
 * 
 * A reusable C++ library for connecting ESP32 devices to Hermes Studio
 * via Socket.IO (Engine.IO v4) protocol. Supports voice conversation,
 * audio playback, and device management.
 * 
 * Features:
 * - Socket.IO client with WebSocket transport
 * - MCU authentication flow
 * - Voice stream (ADPCM encoding)
 * - Event-driven architecture
 * - Gateway discovery (mDNS + network scan)
 * 
 * Usage:
 * @code
 *   #include <hermes-device.h>
 *   
 *   HermesDevice device;
 *   device.onEvent([](const String& event, const String& json) {
 *     Serial.printf("Event: %s\n", event.c_str());
 *   });
 *   
 *   void setup() {
 *     device.begin("my-device", "global_agent");
 *     device.connectWifi("SSID", "password");
 *   }
 *   
 *   void loop() {
 *     device.loop();
 *   }
 * @endcode
 * 
 * @author Hermes Agent
 * @version 1.0.0
 * @license MIT
 */

#ifndef _HERMES_DEVICE_H_
#define _HERMES_DEVICE_H_

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include <vector>
#include <functional>

// ============================================================
// Version
// ============================================================
#define HERMES_DEVICE_LIB_VERSION "1.0.0"

// ============================================================
// Configuration Defaults
// ============================================================
#ifndef HERMES_DEFAULT_SAMPLE_RATE
#define HERMES_DEFAULT_SAMPLE_RATE 16000
#endif

#ifndef HERMES_DEFAULT_RECORD_MAX_MS
#define HERMES_DEFAULT_RECORD_MAX_MS 15000
#endif

#ifndef HERMES_DEFAULT_RECORD_MIN_MS
#define HERMES_DEFAULT_RECORD_MIN_MS 500
#endif

#ifndef HERMES_DEFAULT_VAD_THRESHOLD
#define HERMES_DEFAULT_VAD_THRESHOLD 300
#endif

#ifndef HERMES_MAX_WS_FRAME_SIZE
#define HERMES_MAX_WS_FRAME_SIZE 8192
#endif

#ifndef HERMES_MAX_NOTIFICATIONS
#define HERMES_MAX_NOTIFICATIONS 10
#endif

#ifndef HERMES_MAX_ALARMS
#define HERMES_MAX_ALARMS 5
#endif

#ifndef HERMES_SOCKET_RECONNECT_MS
#define HERMES_SOCKET_RECONNECT_MS 5000
#endif

#ifndef HERMES_WIFI_CONNECT_TIMEOUT_MS
#define HERMES_WIFI_CONNECT_TIMEOUT_MS 15000
#endif

#ifndef HERMES_NTP_SYNC_INTERVAL
#define HERMES_NTP_SYNC_INTERVAL 3600
#endif

#ifndef HERMES_GATEWAY_PORT
#define HERMES_GATEWAY_PORT 8648
#endif

// ============================================================
// Data Structures
// ============================================================

enum class HermesAuthState {
  Disconnected,
  Connecting,
  Authenticating,
  Authenticated,
  Failed
};

enum class HermesInteractionStatus {
  Idle,
  Listening,
  Thinking,
  Playing,
  Error
};

struct HermesNotification {
  String title;
  String message;
  uint32_t timestamp;
  bool read;
};

struct HermesAlarm {
  uint8_t hour;
  uint8_t minute;
  bool enabled;
  bool triggered;
};

struct HermesSensorData {
  float temperature;
  float humidity;
  float batteryVoltage;
  uint8_t batteryPercent;
  uint32_t lastUpdate;
};

// ============================================================
// JSON Utilities
// ============================================================

class HermesJson {
public:
  static String escape(const String& s);
  static String unescape(const String& s);
  static String getString(const String& json, const String& key);
  static int getInt(const String& json, const String& key, int defaultValue = 0);
  static float getFloat(const String& json, const String& key, float defaultValue = 0.0f);
  static bool getBool(const String& json, const String& key, bool defaultValue = false);
  static String build(const char* keys[], const String* values, size_t count);
};

// ============================================================
// ADPCM Codec (IMA-ADPCM 4-bit)
// ============================================================

class HermesAdpcm {
public:
  static int encode(const int16_t* samples, size_t sampleCount, uint8_t* output, size_t outputSize);
  static int decode(const uint8_t* input, size_t inputBytes, int16_t* samples, size_t maxSamples);
  
private:
  static const int16_t stepTable[89];
  static const int8_t indexTable[16];
};

// ============================================================
// Socket.IO Client (Engine.IO v4)
// ============================================================

class HermesSocketIO {
public:
  using EventCallback = std::function<void(const String& event, const String& json)>;
  using RawMessageCallback = std::function<void(const String& message);
  
  HermesSocketIO();
  ~HermesSocketIO();
  
  // Connection management
  bool connect(const String& url);
  void disconnect();
  bool isConnected() const { return _connected; }
  bool isNamespaceReady() const { return _namespaceReady; }
  
  // Event sending
  bool sendEvent(const String& event, const String& json);
  bool sendJson(const String& json);
  bool sendNamespaceConnect();
  void sendPing() { sendRawText("2"); }
  void sendPong() { sendRawText("3"); }
  
  // Binary data (for voice)
  bool sendBinary(const uint8_t* data, size_t length);
  
  // Message loop - call this in main loop
  void loop();
  
  // Callbacks
  void onEvent(EventCallback cb) { _eventCb = cb; }
  void onRawMessage(RawMessageCallback cb) { _rawMessageCb = cb; }
  void onConnected(std::function<void()> cb) { _connectedCb = cb; }
  void onDisconnected(std::function<void()> cb) { _disconnectedCb = cb; }
  
  // Connection stats
  uint32_t getConnectTime() const { return _connectTime; }
  uint32_t getLastActivity() const { return _lastActivity; }
  uint8_t getFailureCount() const { return _failureCount; }
  void resetFailureCount() { _failureCount = 0; }
  
  // Auth token injection
  void setAuthToken(const String& token) { _authToken = token; }
  const String& getAuthToken() const { return _authToken; }
  
  // Namespace
  void setNamespace(const String& ns) { _namespace = ns; }
  const String& getNamespace() const { return _namespace; }
  
private:
  WiFiClient _plainClient;
  WiFiClientSecure _secureClient;
  WiFiClient* _client;
  
  bool _connected;
  bool _namespaceReady;
  bool _wsUpgraded;
  String _authToken;
  String _namespace;
  String _host;
  uint16_t _port;
  bool _useSSL;
  
  uint32_t _connectTime;
  uint32_t _lastActivity;
  uint32_t _lastPing;
  uint8_t _failureCount;
  bool _firstData;
  
  EventCallback _eventCb;
  RawMessageCallback _rawMessageCb;
  std::function<void()> _connectedCb;
  std::function<void()> _disconnectedCb;
  
  // WebSocket frame handling
  bool sendRawWsFrame(uint8_t opcode, const uint8_t* data, size_t length);
  bool sendRawWsText(const String& payload);
  bool sendRawText(const String& payload) { return sendRawWsText(payload); }
  bool writeBytes(const uint8_t* data, size_t length);
  bool readBytes(uint8_t* buffer, size_t length, uint32_t timeoutMs = 100);
  
  // Socket.IO message parsing
  void handleMessage(const String& message);
  void handleEnginePacket(const String& packet);
  void handleSocketIoEvent(const String& message);
  bool parseEvent(const String& message, String* event, String* json);
  
  // Connection failure tracking
  void markConnected();
  void markDisconnected();
};

// ============================================================
// Hermes Studio Device (High-level API)
// ============================================================

class HermesDevice {
public:
  using InteractionCallback = std::function<void(const String& interactionId, const String& status, const String& text)>;
  using AudioCallback = std::function<void(const String& url, const String& mimeType, uint32_t sampleRate)>;
  using NotificationCallback = std::function<void(const String& title, const String& message)>;
  
  HermesDevice();
  ~HermesDevice();
  
  // Initialization
  void begin(const String& deviceName = "HStudio-Device", 
             const String& deviceType = "global_agent",
             const String& namespaceName = "/global-agent");
  
  // WiFi
  bool connectWifi(const String& ssid, const String& pass, bool save = true);
  bool connectSavedWifi();
  void disconnectWifi();
  bool isWifiConnected() const { return _wifiConnected; }
  void startSetupAp(const String& ssid, const String& password = "");
  bool isSetupApActive() const { return _apMode; }
  
  // Gateway discovery
  String discoverGateway();
  bool testGateway(const String& ip);
  void setGatewayUrl(const String& url) { _gatewayUrl = url; }
  const String& getGatewayUrl() const { return _gatewayUrl; }
  
  // Authentication
  bool login(const String& account, const String& password, const String& profile);
  void logout();
  bool isAuthenticated() const { return _authState == HermesAuthState::Authenticated; }
  HermesAuthState getAuthState() const { return _authState; }
  
  // Voice interaction
  bool startVoiceInteraction(const String& interactionId);
  bool sendVoiceChunk(const String& interactionId, const uint8_t* data, size_t length, uint32_t offset);
  bool endVoiceInteraction(const String& interactionId, uint32_t totalBytes);
  
  // Status reporting
  void reportStatus(const String& interactionId = "", const String& status = "", 
                    bool audioPlaying = false, uint32_t queueLength = 0);
  void reportReady();
  
  // Event handlers
  void onInteraction(InteractionCallback cb) { _interactionCb = cb; }
  void onAudio(AudioCallback cb) { _audioCb = cb; }
  void onNotification(NotificationCallback cb) { _notificationCb = cb; }
  void onSessionClear(std::function<void()> cb) { _sessionClearCb = cb; }
  void onAuthRequired(std::function<void()> cb) { _authRequiredCb = cb; }
  
  // Notifications
  void addNotification(const String& title, const String& msg);
  const std::vector<HermesNotification>& getNotifications() const { return _notifications; }
  void clearNotifications();
  
  // Alarms
  bool addAlarm(uint8_t hour, uint8_t minute);
  void removeAlarm(uint8_t index);
  void toggleAlarm(uint8_t index, bool enabled);
  const HermesAlarm* getAlarms() const { return _alarms; }
  
  // Timer
  void startTimer(uint32_t seconds);
  void stopTimer();
  bool isTimerRunning() const { return _timerRunning; }
  uint32_t getTimerRemaining() const;
  
  // Sensors
  void setSensorData(float temp, float humidity, float batteryV, uint8_t batteryPct);
  const HermesSensorData& getSensorData() const { return _sensors; }
  
  // Time sync
  bool syncTime(const char* ntpServer = "pool.ntp.org", long gmtOffset = 8*3600, int daylightOffset = 0);
  bool isTimeSynced() const { return _timeSynced; }
  
  // Main loop - call this in Arduino loop()
  void loop();
  
  // Device info
  const String& getDeviceId() const { return _deviceId; }
  const String& getDeviceCode() const { return _deviceCode; }
  const String& getDeviceName() const { return _deviceName; }
  HermesSocketIO& getSocket() { return _socket; }
  
  // Configuration
  void setSampleRate(uint32_t rate) { _sampleRate = rate; }
  void setRecordMaxMs(uint32_t ms) { _recordMaxMs = ms; }
  void setRecordMinMs(uint32_t ms) { _recordMinMs = ms; }
  void setPort(uint16_t port) { _port = port; }
  
  // Web server (optional, for configuration)
  void startWebServer(uint16_t port = 80);
  void stopWebServer();
  
private:
  // Device identity
  String _deviceId;
  String _deviceCode;
  String _deviceName;
  String _deviceType;
  String _namespaceName;
  uint16_t _port;
  
  // Network state
  bool _wifiConnected;
  bool _apMode;
  String _savedSsid;
  String _savedPass;
  String _gatewayUrl;
  
  // Auth state
  HermesAuthState _authState;
  String _account;
  String _password;
  String _profile;
  
  // Socket.IO
  HermesSocketIO _socket;
  
  // Interaction state
  String _currentInteractionId;
  HermesInteractionStatus _interactionStatus;
  uint32_t _sampleRate;
  uint32_t _recordMaxMs;
  uint32_t _recordMinMs;
  
  // Notifications
  std::vector<HermesNotification> _notifications;
  
  // Alarms
  HermesAlarm _alarms[HERMES_MAX_ALARMS];
  
  // Timer
  uint32_t _timerStartMs;
  uint32_t _timerDuration;
  bool _timerRunning;
  
  // Sensors
  HermesSensorData _sensors;
  
  // Time
  bool _timeSynced;
  time_t _lastNtpSync;
  
  // Callbacks
  InteractionCallback _interactionCb;
  AudioCallback _audioCb;
  NotificationCallback _notificationCb;
  std::function<void()> _sessionClearCb;
  std::function<void()> _authRequiredCb;
  
  // Preferences
  Preferences _prefs;
  
  // Internal methods
  void generateDeviceId();
  void loadPreferences();
  void saveCredentials();
  void clearCredentials();
  void handleSocketEvent(const String& event, const String& json);
  void handleMcuAuth(const String& json);
  void handleInteractionStatus(const String& json);
  void handleAudioEnqueue(const String& json);
  void handleSessionClear();
  void handleAuthInvalid();
  void handleReauthRequired();
  void checkAlarms();
  void checkTimer();
  void reconnectSocket();
  
  // Web server
  void* _webServer;
  bool _webServerActive;
};

#endif // _HERMES_DEVICE_H_
