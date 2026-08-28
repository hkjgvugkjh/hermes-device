/**
 * @file hermes-device.cpp
 * @brief Hermes Studio Device Protocol Library Implementation
 */

#include "hermes-device.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_system.h>
#include <esp_rom_sys.h>

// ============================================================
// JSON Utilities Implementation
// ============================================================

String HermesJson::escape(const String& s) {
  String out;
  out.reserve(s.length() * 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

String HermesJson::unescape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '\\' && i + 1 < s.length()) {
      char next = s[++i];
      switch (next) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '/': out += '/'; break;
        default: out += next; break;
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

String HermesJson::getString(const String& json, const String& key) {
  String searchKey = "\"" + key + "\"";
  int keyPos = json.indexOf(searchKey);
  if (keyPos < 0) return "";
  int colonPos = json.indexOf(':', keyPos + searchKey.length());
  if (colonPos < 0) return "";
  int quoteStart = json.indexOf('"', colonPos + 1);
  if (quoteStart < 0) return "";
  int quoteEnd = json.indexOf('"', quoteStart + 1);
  if (quoteEnd < 0) return "";
  return json.substring(quoteStart + 1, quoteEnd);
}

int HermesJson::getInt(const String& json, const String& key, int defaultValue) {
  String searchKey = "\"" + key + "\"";
  int keyPos = json.indexOf(searchKey);
  if (keyPos < 0) return defaultValue;
  int colonPos = json.indexOf(':', keyPos + searchKey.length());
  if (colonPos < 0) return defaultValue;
  int valueStart = colonPos + 1;
  while (valueStart < (int)json.length() && isspace(json[valueStart])) valueStart++;
  if (valueStart >= (int)json.length()) return defaultValue;
  if (json[valueStart] == '"') {
    int quoteEnd = json.indexOf('"', valueStart + 1);
    if (quoteEnd < 0) return defaultValue;
    return json.substring(valueStart + 1, quoteEnd).toInt();
  }
  int valueEnd = valueStart;
  while (valueEnd < (int)json.length() && (isdigit(json[valueEnd]) || json[valueEnd] == '-')) valueEnd++;
  return json.substring(valueStart, valueEnd).toInt();
}

float HermesJson::getFloat(const String& json, const String& key, float defaultValue) {
  String searchKey = "\"" + key + "\"";
  int keyPos = json.indexOf(searchKey);
  if (keyPos < 0) return defaultValue;
  int colonPos = json.indexOf(':', keyPos + searchKey.length());
  if (colonPos < 0) return defaultValue;
  int valueStart = colonPos + 1;
  while (valueStart < (int)json.length() && isspace(json[valueStart])) valueStart++;
  if (valueStart >= (int)json.length()) return defaultValue;
  return json.substring(valueStart).toFloat();
}

bool HermesJson::getBool(const String& json, const String& key, bool defaultValue) {
  String searchKey = "\"" + key + "\"";
  int keyPos = json.indexOf(searchKey);
  if (keyPos < 0) return defaultValue;
  int colonPos = json.indexOf(':', keyPos + searchKey.length());
  if (colonPos < 0) return defaultValue;
  int valueStart = colonPos + 1;
  while (valueStart < (int)json.length() && isspace(json[valueStart])) valueStart++;
  if (valueStart >= (int)json.length()) return defaultValue;
  return json.substring(valueStart, valueStart + 4) == "true";
}

String HermesJson::build(const char* keys[], const String* values, size_t count) {
  String json = "{";
  for (size_t i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "\"" + String(keys[i]) + "\":\"" + escape(values[i]) + "\"";
  }
  json += "}";
  return json;
}

// ============================================================
// ADPCM Codec Implementation
// ============================================================

const int16_t HermesAdpcm::stepTable[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
  143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
  449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
  1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
  3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
  22385, 24623, 27086, 29794, 32767
};

const int8_t HermesAdpcm::indexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

int HermesAdpcm::encode(const int16_t* samples, size_t sampleCount, uint8_t* output, size_t outputSize) {
  int predictor = 0;
  int stepIndex = 0;
  size_t outPos = 0;
  
  for (size_t i = 0; i < sampleCount && outPos < outputSize; i++) {
    int diff = samples[i] - predictor;
    int step = stepTable[stepIndex];
    
    int nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }
    
    if (diff >= step) { nibble |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { nibble |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) { nibble |= 1; }
    
    // Decode nibble to get predicted value
    int diffPred = stepTable[stepIndex] >> 3;
    if (nibble & 4) diffPred += stepTable[stepIndex];
    if (nibble & 2) diffPred += stepTable[stepIndex] >> 1;
    if (nibble & 1) diffPred += stepTable[stepIndex] >> 2;
    
    if (nibble & 8) predictor -= diffPred;
    else predictor += diffPred;
    
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    
    stepIndex += indexTable[nibble];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    
    if (i & 1) {
      output[outPos++] = ((nibble & 0x0F) << 4);
    } else {
      output[outPos] = nibble & 0x0F;
    }
  }
  
  return outPos;
}

int HermesAdpcm::decode(const uint8_t* input, size_t inputBytes, int16_t* samples, size_t maxSamples) {
  int predictor = 0;
  int stepIndex = 0;
  size_t samplePos = 0;
  
  for (size_t i = 0; i < inputBytes && samplePos < maxSamples; i++) {
    uint8_t byte = input[i];
    // Low nibble first, then high nibble
    for (int nibIdx = 0; nibIdx < 2 && samplePos < maxSamples; nibIdx++) {
      int nibble = (nibIdx == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
      int step = stepTable[stepIndex];
      
      int diffPred = step >> 3;
      if (nibble & 4) diffPred += step;
      if (nibble & 2) diffPred += step >> 1;
      if (nibble & 1) diffPred += step >> 2;
      
      if (nibble & 8) predictor -= diffPred;
      else predictor += diffPred;
      
      if (predictor > 32767) predictor = 32767;
      if (predictor < -32768) predictor = -32768;
      
      stepIndex += indexTable[nibble];
      if (stepIndex < 0) stepIndex = 0;
      if (stepIndex > 88) stepIndex = 88;
      
      samples[samplePos++] = (int16_t)predictor;
    }
  }
  
  return samplePos;
}

// ============================================================
// Socket.IO Client Implementation
// ============================================================

HermesSocketIO::HermesSocketIO() 
  : _client(&_plainClient), _connected(false), _namespaceReady(false), 
    _wsUpgraded(false), _port(80), _useSSL(false), _connectTime(0), 
    _lastActivity(0), _lastPing(0), _failureCount(0), _firstData(false) {
  _namespace = "/global-agent";
}

HermesSocketIO::~HermesSocketIO() {
  disconnect();
}

bool HermesSocketIO::connect(const String& url) {
  // Parse URL
  String host = url;
  if (host.startsWith("http://")) host = host.substring(7);
  if (host.startsWith("https://")) {
    host = host.substring(8);
    _useSSL = true;
    _client = &_secureClient;
  } else {
    _useSSL = false;
    _client = &_plainClient;
  }
  
  _port = 80;
  int colon = host.lastIndexOf(':');
  if (colon > 0) {
    _port = host.substring(colon + 1).toInt();
    _host = host.substring(0, colon);
  } else {
    _host = host;
    if (_useSSL) _port = 443;
  }
  
  Serial.printf("Socket.IO: connecting to %s:%d (ssl=%d)\n", _host.c_str(), _port, _useSSL);
  
  if (_client->connect(_host.c_str(), _port)) {
    // Send HTTP WebSocket upgrade request
    String path = "/socket.io/?EIO=4&transport=websocket";
    _client->print("GET " + path + " HTTP/1.1\r\n");
    _client->print("Host: " + _host + "\r\n");
    _client->print("Upgrade: websocket\r\n");
    _client->print("Connection: Upgrade\r\n");
    _client->print("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
    _client->print("Sec-WebSocket-Version: 13\r\n");
    _client->print("\r\n");
    
    // Read HTTP response until empty line
    uint32_t upgradeStart = millis();
    String prevLine;
    while (millis() - upgradeStart < 3000) {
      if (_client->available()) {
        String resp = _client->readStringUntil('\n');
        if (prevLine == "\r" || resp == "\r") break;
        if (resp.length() > 0 && resp != "\r") prevLine = resp;
      }
    }
    
    _wsUpgraded = true;
    _connected = true;
    markConnected();
    
    // Send Engine.IO namespace connect
    delay(100);
    sendNamespaceConnect();
    
    Serial.println("Socket.IO: connected");
    return true;
  }
  
  _failureCount++;
  Serial.printf("Socket.IO: connection failed (%d)\n", _failureCount);
  return false;
}

void HermesSocketIO::disconnect() {
  if (_client->connected()) {
    _client->stop();
  }
  _connected = false;
  _namespaceReady = false;
  _wsUpgraded = false;
  markDisconnected();
}

bool HermesSocketIO::sendRawWsFrame(uint8_t opcode, const uint8_t* data, size_t length) {
  if (!_client->connected()) return false;
  
  uint8_t header[14];
  size_t headerLen = 0;
  header[headerLen++] = 0x80 | (opcode & 0x0F);
  
  if (length < 126) {
    header[headerLen++] = 0x80 | static_cast<uint8_t>(length);
  } else if (length <= 0xFFFF) {
    header[headerLen++] = 0x80 | 126;
    header[headerLen++] = static_cast<uint8_t>((length >> 8) & 0xFF);
    header[headerLen++] = static_cast<uint8_t>(length & 0xFF);
  } else {
    return false;
  }
  
  uint8_t mask[4];
  for (uint8_t i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  for (uint8_t i = 0; i < 4; ++i) header[headerLen++] = mask[i];
  
  if (!writeBytes(header, headerLen)) return false;
  
  constexpr size_t kChunk = 256;
  uint8_t buffer[kChunk];
  size_t offset = 0;
  while (offset < length) {
    size_t n = min(kChunk, length - offset);
    for (size_t i = 0; i < n; ++i) {
      buffer[i] = data[offset + i] ^ mask[(offset + i) & 3];
    }
    if (!writeBytes(buffer, n)) return false;
    offset += n;
    yield();
  }
  return true;
}

bool HermesSocketIO::sendRawWsText(const String& payload) {
  return sendRawWsFrame(0x1, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
}

bool HermesSocketIO::writeBytes(const uint8_t* data, size_t length) {
  if (!_client->connected()) return false;
  size_t written = _client->write(data, length);
  return written == length;
}

bool HermesSocketIO::readBytes(uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  size_t read = 0;
  uint32_t startedAt = millis();
  while (read < length && millis() - startedAt < timeoutMs) {
    int available = _client->available();
    if (available > 0) {
      int n = _client->read(buffer + read, min(static_cast<size_t>(available), length - read));
      if (n > 0) read += static_cast<size_t>(n);
    } else {
      delay(1);
      yield();
    }
  }
  return read == length;
}

bool HermesSocketIO::sendEvent(const String& event, const String& json) {
  if (!_connected || !_namespaceReady || event.length() == 0) return false;
  
  // Inject auth token if needed
  String securedJson = json;
  if (_authToken.length() > 0 && json.length() > 0 && json[0] == '{' && json.indexOf("\"apiToken\"") < 0) {
    securedJson = "{\"apiToken\":\"" + HermesJson::escape(_authToken) + "\"," + json.substring(1);
  }
  
  String payload;
  payload.reserve(securedJson.length() + event.length() + 28);
  payload += "42" + _namespace + ",\"";
  payload += HermesJson::escape(event);
  payload += "\",";
  payload += securedJson;
  payload += "]";
  
  return sendRawWsText(payload);
}

bool HermesSocketIO::sendJson(const String& json) {
  String type = HermesJson::getString(json, "type");
  if (type.length() == 0) type = "mcu.event";
  return sendEvent(type, json);
}

bool HermesSocketIO::sendNamespaceConnect() {
  if (!_connected || _authToken.length() == 0) return false;
  
  // Engine.IO namespace connect with auth token
  String frame = "40" + _namespace;
  sendRawWsText(frame);
  return true;
}

bool HermesSocketIO::sendBinary(const uint8_t* data, size_t length) {
  return sendRawWsFrame(0x2, data, length);
}

void HermesSocketIO::loop() {
  if (!_connected) return;
  if (!_client->connected()) {
    _connected = false;
    _namespaceReady = false;
    markDisconnected();
    return;
  }
  
  _lastActivity = millis();
  
  if (_client->available() < 2) return;
  
  _firstData = true;
  
  while (_client->available() >= 2) {
    uint8_t header[2];
    if (!readBytes(header, 2)) return;
    
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t length = header[1] & 0x7F;
    
    if (length == 126) {
      uint8_t ext[2];
      if (!readBytes(ext, 2)) return;
      length = (static_cast<uint16_t>(ext[0]) << 8) | ext[1];
    } else if (length == 127) {
      uint8_t ext[8];
      if (!readBytes(ext, 8)) return;
      length = 0;
      for (uint8_t i = 0; i < 8; ++i) length = (length << 8) | ext[i];
    }
    
    if (length > HERMES_MAX_WS_FRAME_SIZE) {
      Serial.println("WS frame too large");
      _connected = false;
      _client->stop();
      return;
    }
    
    if (opcode == 0x1) {
      // Text frame
      String payload;
      payload.reserve(length);
      uint8_t buf[256];
      uint64_t remaining = length;
      while (remaining > 0) {
        size_t toRead = min(static_cast<size_t>(remaining), sizeof(buf));
        if (!readBytes(buf, toRead)) break;
        for (size_t i = 0; i < toRead; i++) payload += static_cast<char>(buf[i]);
        remaining -= toRead;
      }
      handleMessage(payload);
    } else if (opcode == 0x2) {
      // Binary frame - skip for now
      uint8_t buf[512];
      uint64_t remaining = length;
      while (remaining > 0) {
        size_t toRead = min(static_cast<size_t>(remaining), sizeof(buf));
        if (!readBytes(buf, toRead)) break;
        remaining -= toRead;
      }
    } else if (opcode == 0x8) {
      // Close frame
      _connected = false;
      _client->stop();
      return;
    } else if (opcode == 0x9) {
      // Ping - send pong
      sendRawWsFrame(0xA, nullptr, 0);
    }
  }
}

void HermesSocketIO::handleMessage(const String& message) {
  if (_rawMessageCb) _rawMessageCb(message);
  
  // Engine.IO v4 protocol
  if (message == "2") {
    // Ping from server
    sendRawWsText("3");
    return;
  }
  if (message.startsWith("0")) {
    // Engine.IO hello - send namespace connect
    sendNamespaceConnect();
    return;
  }
  if (message.startsWith("40" + _namespace)) {
    // Namespace connected
    _namespaceReady = true;
    _failureCount = 0;
    Serial.println("Socket.IO: namespace connected");
    if (_connectedCb) _connectedCb();
    return;
  }
  if (message.startsWith("42")) {
    // Socket.IO event
    handleSocketIoEvent(message);
    return;
  }
}

void HermesSocketIO::handleSocketIoEvent(const String& message) {
  String event;
  String json;
  if (parseEvent(message, &event, &json)) {
    if (_eventCb) _eventCb(event, json);
  }
}

bool HermesSocketIO::parseEvent(const String& message, String* event, String* json) {
  int arrayStart = message.indexOf('[');
  if (arrayStart < 0) return false;
  int firstQuote = message.indexOf('"', arrayStart);
  if (firstQuote < 0) return false;
  int secondQuote = message.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return false;
  int comma = message.indexOf(',', secondQuote + 1);
  int arrayEnd = message.lastIndexOf(']');
  if (comma < 0 || arrayEnd <= comma) return false;
  
  *event = message.substring(firstQuote + 1, secondQuote);
  
  int payloadStart = comma + 1;
  while (payloadStart < arrayEnd && isspace(static_cast<unsigned char>(message[payloadStart]))) ++payloadStart;
  
  int payloadEnd = arrayEnd;
  if (payloadStart < arrayEnd && message[payloadStart] == '{') {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (int i = payloadStart; i < arrayEnd; ++i) {
      char c = message[i];
      if (escaped) { escaped = false; continue; }
      if (c == '\\') { escaped = true; continue; }
      if (c == '"') { inString = !inString; continue; }
      if (inString) continue;
      if (c == '{') ++depth;
      else if (c == '}') {
        --depth;
        if (depth == 0) { payloadEnd = i + 1; break; }
      }
    }
  }
  
  *json = message.substring(payloadStart, payloadEnd);
  return true;
}

void HermesSocketIO::markConnected() {
  _connectTime = millis();
  _lastActivity = millis();
  _firstData = false;
}

void HermesSocketIO::markDisconnected() {
  if (millis() - _connectTime < 5000) {
    _failureCount++;
  } else {
    _failureCount = 0;
  }
  if (_disconnectedCb) _disconnectedCb();
}

// ============================================================
// Hermes Device Implementation
// ============================================================

HermesDevice::HermesDevice() 
  : _port(80), _wifiConnected(false), _apMode(false), 
    _authState(HermesAuthState::Disconnected), _sampleRate(HERMES_DEFAULT_SAMPLE_RATE),
    _recordMaxMs(HERMES_DEFAULT_RECORD_MAX_MS), _recordMinMs(HERMES_DEFAULT_RECORD_MIN_MS),
    _timerStartMs(0), _timerDuration(0), _timerRunning(false),
    _timeSynced(false), _lastNtpSync(0), _webServer(nullptr), _webServerActive(false) {
  memset(_alarms, 0, sizeof(_alarms));
  memset(&_sensors, 0, sizeof(_sensors));
}

HermesDevice::~HermesDevice() {
  stopWebServer();
}

void HermesDevice::begin(const String& deviceName, const String& deviceType, const String& namespaceName) {
  _deviceName = deviceName;
  _deviceType = deviceType;
  _namespaceName = namespaceName;
  
  generateDeviceId();
  _socket.setNamespace(namespaceName);
  
  // Setup socket callbacks
  _socket.onEvent([this](const String& event, const String& json) {
    handleSocketEvent(event, json);
  });
  _socket.onDisconnected([this]() {
    _authState = HermesAuthState::Disconnected;
  });
  
  // Load saved preferences
  loadPreferences();
  
  Serial.printf("HermesDevice: initialized '%s' (ID=%s, Code=%s)\n", 
                _deviceName.c_str(), _deviceId.c_str(), _deviceCode.c_str());
}

void HermesDevice::generateDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  _deviceId = String(buf);
  char codeBuf[9];
  snprintf(codeBuf, sizeof(codeBuf), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
  _deviceCode = String(codeBuf);
}

void HermesDevice::loadPreferences() {
  _prefs.begin("wifi", true);
  _savedSsid = _prefs.getString("ssid", "");
  _savedPass = _prefs.getString("pass", "");
  _prefs.end();
  
  _prefs.begin("mcu", true);
  String token = _prefs.getString("auth_token", "");
  _gatewayUrl = _prefs.getString("active_url", "");
  _account = _prefs.getString("cur_account", "");
  _password = _prefs.getString("cur_password", "");
  _profile = _prefs.getString("cur_profile", "");
  _prefs.end();
  
  _socket.setAuthToken(token);
  
  // Load alarms
  for (uint8_t i = 0; i < HERMES_MAX_ALARMS; i++) {
    String key = "alarm_" + String(i);
    _prefs.begin("alarms", true);
    _alarms[i].hour = _prefs.getUInt((key + "_h").c_str(), 0);
    _alarms[i].minute = _prefs.getUInt((key + "_m").c_str(), 0);
    _alarms[i].enabled = _prefs.getBool((key + "_e").c_str(), false);
    _prefs.end();
  }
}

bool HermesDevice::connectWifi(const String& ssid, const String& pass, bool save) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < HERMES_WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    yield();
  }
  
  _wifiConnected = WiFi.status() == WL_CONNECTED;
  if (_wifiConnected) {
    Serial.printf("WiFi: connected to %s IP=%s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
    if (save) {
      _savedSsid = ssid;
      _savedPass = pass;
      _prefs.begin("wifi", false);
      _prefs.putString("ssid", ssid);
      _prefs.putString("pass", pass);
      _prefs.end();
    }
    discoverGateway();
  }
  return _wifiConnected;
}

bool HermesDevice::connectSavedWifi() {
  if (_savedSsid.length() > 0) {
    return connectWifi(_savedSsid, _savedPass, false);
  }
  return false;
}

void HermesDevice::disconnectWifi() {
  WiFi.disconnect();
  _wifiConnected = false;
}

void HermesDevice::startSetupAp(const String& ssid, const String& password) {
  WiFi.mode(WIFI_AP);
  if (password.length() > 0) {
    WiFi.softAP(ssid.c_str(), password.c_str());
  } else {
    WiFi.softAP(ssid.c_str());
  }
  _apMode = true;
  Serial.printf("AP: started '%s' IP=%s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
}

String HermesDevice::discoverGateway() {
  // mDNS discovery
  if (MDNS.begin("hstudio-device")) {
    int n = MDNS.queryService("http", "tcp");
    for (int i = 0; i < n; i++) {
      uint16_t port = MDNS.port(i);
      if (port == HERMES_GATEWAY_PORT) {
        String ip = MDNS.IP(i).toString();
        _gatewayUrl = "http://" + ip + ":" + String(HERMES_GATEWAY_PORT);
        Serial.printf("Gateway found via mDNS: %s\n", _gatewayUrl.c_str());
        return _gatewayUrl;
      }
    }
  }
  
  // Network scan
  String localIp = WiFi.localIP().toString();
  int lastDot = localIp.lastIndexOf('.');
  String subnet = localIp.substring(0, lastDot + 1);
  
  int scanList[] = {1, 100, 101, 102, 103, 104, 105, 200, 201, 202, 21, 22, 23, 24, 25};
  for (int i = 0; i < sizeof(scanList)/sizeof(scanList[0]); i++) {
    String testUrl = subnet + String(scanList[i]);
    if (testUrl == localIp) continue;
    if (testGateway(testUrl)) {
      _gatewayUrl = "http://" + testUrl + ":" + String(HERMES_GATEWAY_PORT);
      Serial.printf("Gateway found via scan: %s\n", _gatewayUrl.c_str());
      return _gatewayUrl;
    }
  }
  
  // Fallback to saved URL
  if (_gatewayUrl.length() > 0) {
    Serial.printf("Using saved gateway: %s\n", _gatewayUrl.c_str());
    return _gatewayUrl;
  }
  
  Serial.println("Gateway not found");
  return "";
}

bool HermesDevice::testGateway(const String& ip) {
  HTTPClient http;
  http.setTimeout(500);
  String url = "http://" + ip + ":" + String(HERMES_GATEWAY_PORT) + "/api/hermes/health";
  if (!http.begin(url)) return false;
  int code = http.GET();
  http.end();
  return code > 0;
}

bool HermesDevice::login(const String& account, const String& password, const String& profile) {
  if (_gatewayUrl.length() == 0) return false;
  
  _account = account;
  _password = password;
  _profile = profile;
  
  String endpoint = _gatewayUrl;
  while (endpoint.endsWith("/")) endpoint.remove(endpoint.length() - 1);
  endpoint += "/api/auth/mcu-login";
  
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(endpoint)) return false;
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Hermes-Device-Id", _deviceId);
  http.addHeader("X-Hermes-Device-Name", _deviceName);
  
  String payload = "{\"token\":\"" + HermesJson::escape(_deviceId) + 
                   "\",\"id\":\"" + HermesJson::escape(_deviceId) + 
                   "\",\"device_code\":\"" + HermesJson::escape(_deviceCode) + 
                   "\",\"device_type\":\"" + _deviceType + 
                   "\",\"source\":\"" + _deviceType + 
                   "\",\"account\":\"" + HermesJson::escape(account) + 
                   "\",\"password\":\"" + HermesJson::escape(password) + 
                   "\",\"relayMode\":\"lan\"}";
  
  int code = http.POST(payload);
  String response = http.getString();
  http.end();
  
  Serial.printf("Login: HTTP %d: %s\n", code, response.c_str());
  
  if (code >= 200 && code < 300) {
    String token = HermesJson::getString(response, "token");
    if (token.length() > 0) {
      _socket.setAuthToken(token);
      saveCredentials();
      _authState = HermesAuthState::Authenticated;
      reconnectSocket();
      return true;
    }
  }
  
  _authState = HermesAuthState::Failed;
  return false;
}

void HermesDevice::logout() {
  _socket.setAuthToken("");
  _socket.disconnect();
  clearCredentials();
  _authState = HermesAuthState::Disconnected;
}

void HermesDevice::saveCredentials() {
  _prefs.begin("mcu", false);
  _prefs.putString("auth_token", _socket.getAuthToken());
  _prefs.putString("active_url", _gatewayUrl);
  _prefs.putString("cur_account", _account);
  _prefs.putString("cur_password", _password);
  _prefs.putString("cur_profile", _profile);
  _prefs.end();
}

void HermesDevice::clearCredentials() {
  _prefs.begin("mcu", false);
  _prefs.remove("auth_token");
  _prefs.remove("active_url");
  _prefs.remove("relay_url");
  _prefs.end();
}

bool HermesDevice::startVoiceInteraction(const String& interactionId) {
  if (!_socket.isConnected() || !_socket.isNamespaceReady()) return false;
  
  _currentInteractionId = interactionId;
  _interactionStatus = HermesInteractionStatus::Listening;
  
  String json = "{\"type\":\"voice.stream.start\",\"interactionId\":\"" + HermesJson::escape(interactionId) + 
                "\",\"mimeType\":\"audio/x-ima-adpcm\",\"frameFormat\":\"hadp-chunk-v1\"" +
                ",\"sampleRate\":" + String(_sampleRate) + 
                ",\"channels\":1,\"bitsPerSample\":16" +
                ",\"profile\":\"" + HermesJson::escape(_profile) + "\"}";
  return _socket.sendJson(json);
}

bool HermesDevice::sendVoiceChunk(const String& interactionId, const uint8_t* data, size_t length, uint32_t offset) {
  if (!_socket.isConnected() || !_socket.isNamespaceReady()) return false;
  
  String payload = "451-/global-agent,[\"voice.stream.chunk\",{\"type\":\"voice.stream.chunk\"";
  payload += ",\"interactionId\":\"" + HermesJson::escape(interactionId) + "\"";
  payload += ",\"offset\":" + String(offset);
  payload += ",\"bytes\":" + String(length);
  payload += ",\"data\":{\"_placeholder\":true,\"num\":0}}]";
  
  return _socket.sendRawWsText(payload) && _socket.sendBinary(data, length);
}

bool HermesDevice::endVoiceInteraction(const String& interactionId, uint32_t totalBytes) {
  if (!_socket.isConnected() || !_socket.isNamespaceReady()) return false;
  
  String json = "{\"type\":\"voice.stream.end\",\"interactionId\":\"" + HermesJson::escape(interactionId) + 
                "\",\"bytes\":" + String(totalBytes) + "}";
  _interactionStatus = HermesInteractionStatus::Thinking;
  return _socket.sendJson(json);
}

void HermesDevice::reportStatus(const String& interactionId, const String& status, 
                                 bool audioPlaying, uint32_t queueLength) {
  if (!_socket.isConnected() || !_socket.isNamespaceReady()) return;
  
  String json = "{\"apiToken\":\"" + HermesJson::escape(_socket.getAuthToken()) + 
                "\",\"type\":\"mcu.status\",\"interactionId\":\"" + HermesJson::escape(interactionId) + 
                "\",\"status\":\"" + HermesJson::escape(status) + 
                "\",\"audioPlaying\":" + String(audioPlaying ? "true" : "false") + 
                ",\"queueLength\":" + String(queueLength) + 
                ",\"socketClients\":1,\"socketConnected\":true" +
                ",\"active_device\":\"" + HermesJson::escape(_deviceId) + 
                "\",\"profile\":\"" + HermesJson::escape(_profile) + "\"}";
  _socket.sendJson(json);
}

void HermesDevice::reportReady() {
  if (!_socket.isConnected() || !_socket.isNamespaceReady()) return;
  
  String json = "{\"type\":\"mcu.ready\",\"id\":\"" + HermesJson::escape(_deviceId) + 
                "\",\"active_device\":\"" + HermesJson::escape(_deviceId) +
                "\",\"profile\":\"" + HermesJson::escape(_profile) + 
                "\",\"capabilities\":{\"display\":true,\"audio_queue\":true,\"audio_playback\":true,\"pcm_stream\":false}}";
  _socket.sendJson(json);
}

void HermesDevice::handleSocketEvent(const String& event, const String& json) {
  String type = HermesJson::getString(json, "type");
  
  if (type == "mcu.auth") {
    handleMcuAuth(json);
  } else if (type == "auth.invalid") {
    handleAuthInvalid();
  } else if (type == "interaction.status") {
    handleInteractionStatus(json);
  } else if (type == "audio.enqueue") {
    handleAudioEnqueue(json);
  } else if (type == "mcu.session.clear") {
    handleSessionClear();
  } else if (type == "mcu.reauth.required") {
    handleReauthRequired();
  }
}

void HermesDevice::handleMcuAuth(const String& json) {
  String authOk = "{\"apiToken\":\"" + HermesJson::escape(_socket.getAuthToken()) + 
                  "\",\"type\":\"mcu.auth.ok\",\"ok\":true,\"id\":\"" + HermesJson::escape(_deviceId) + "\"}";
  String frame = "42" + _namespaceName + ",\"" + "mcu.auth.ok" + "\"," + authOk + "]";
  _socket.sendRawWsText(frame);
  
  // Send status
  String statusJson = "{\"apiToken\":\"" + HermesJson::escape(_socket.getAuthToken()) + 
                      "\",\"type\":\"mcu.status\",\"interactionId\":\"\",\"status\":\"\"" +
                      ",\"audioPlaying\":false,\"queueLength\":0" +
                      ",\"socketClients\":1,\"socketConnected\":true" +
                      ",\"active_device\":\"" + HermesJson::escape(_deviceId) + 
                      "\",\"profile\":\"" + HermesJson::escape(_profile) + "\"}";
  _socket.sendJson(statusJson);
}

void HermesDevice::handleInteractionStatus(const String& json) {
  String interactionId = HermesJson::getString(json, "interactionId");
  String status = HermesJson::getString(json, "status");
  String text = HermesJson::getString(json, "text");
  
  _currentInteractionId = interactionId;
  
  if (_interactionCb) {
    _interactionCb(interactionId, status, text);
  }
}

void HermesDevice::handleAudioEnqueue(const String& json) {
  String url = HermesJson::getString(json, "url");
  String mimeType = HermesJson::getString(json, "mimeType");
  uint32_t sampleRate = HermesJson::getInt(json, "sampleRate");
  if (sampleRate == 0) sampleRate = _sampleRate;
  
  if (_audioCb) {
    _audioCb(url, mimeType, sampleRate);
  }
}

void HermesDevice::handleSessionClear() {
  _currentInteractionId = "";
  _interactionStatus = HermesInteractionStatus::Idle;
  if (_sessionClearCb) _sessionClearCb();
}

void HermesDevice::handleAuthInvalid() {
  _socket.setAuthToken("");
  _socket.disconnect();
  clearCredentials();
  _authState = HermesAuthState::Failed;
  if (_authRequiredCb) _authRequiredCb();
}

void HermesDevice::handleReauthRequired() {
  if (_account.length() > 0 && _password.length() > 0) {
    login(_account, _password, _profile);
  } else {
    handleAuthInvalid();
  }
}

void HermesDevice::addNotification(const String& title, const String& msg) {
  HermesNotification n;
  n.title = title;
  n.message = msg;
  n.timestamp = millis();
  n.read = false;
  _notifications.insert(_notifications.begin(), n);
  if (_notifications.size() > HERMES_MAX_NOTIFICATIONS) {
    _notifications.pop_back();
  }
  if (_notificationCb) _notificationCb(title, msg);
}

void HermesDevice::clearNotifications() {
  _notifications.clear();
}

bool HermesDevice::addAlarm(uint8_t hour, uint8_t minute) {
  for (uint8_t i = 0; i < HERMES_MAX_ALARMS; i++) {
    if (!_alarms[i].enabled) {
      _alarms[i].hour = hour;
      _alarms[i].minute = minute;
      _alarms[i].enabled = true;
      _alarms[i].triggered = false;
      
      String key = "alarm_" + String(i);
      _prefs.begin("alarms", false);
      _prefs.putUInt((key + "_h").c_str(), hour);
      _prefs.putUInt((key + "_m").c_str(), minute);
      _prefs.putBool((key + "_e").c_str(), true);
      _prefs.end();
      return true;
    }
  }
  return false;
}

void HermesDevice::removeAlarm(uint8_t index) {
  if (index < HERMES_MAX_ALARMS) {
    _alarms[index].enabled = false;
    String key = "alarm_" + String(index);
    _prefs.begin("alarms", false);
    _prefs.putBool((key + "_e").c_str(), false);
    _prefs.end();
  }
}

void HermesDevice::toggleAlarm(uint8_t index, bool enabled) {
  if (index < HERMES_MAX_ALARMS) {
    _alarms[index].enabled = enabled;
    String key = "alarm_" + String(index);
    _prefs.begin("alarms", false);
    _prefs.putBool((key + "_e").c_str(), enabled);
    _prefs.end();
  }
}

void HermesDevice::startTimer(uint32_t seconds) {
  _timerDuration = seconds;
  _timerStartMs = millis();
  _timerRunning = true;
}

void HermesDevice::stopTimer() {
  _timerRunning = false;
}

uint32_t HermesDevice::getTimerRemaining() const {
  if (!_timerRunning) return 0;
  uint32_t elapsed = (millis() - _timerStartMs) / 1000;
  return _timerDuration > elapsed ? _timerDuration - elapsed : 0;
}

void HermesDevice::setSensorData(float temp, float humidity, float batteryV, uint8_t batteryPct) {
  _sensors.temperature = temp;
  _sensors.humidity = humidity;
  _sensors.batteryVoltage = batteryV;
  _sensors.batteryPercent = batteryPct;
  _sensors.lastUpdate = millis();
}

bool HermesDevice::syncTime(const char* ntpServer, long gmtOffset, int daylightOffset) {
  if (!_wifiConnected) return false;
  if (_timeSynced && (time(nullptr) - _lastNtpSync) < HERMES_NTP_SYNC_INTERVAL) return true;
  
  configTime(gmtOffset, daylightOffset, ntpServer);
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    _timeSynced = true;
    _lastNtpSync = time(nullptr);
    Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return true;
  }
  return false;
}

void HermesDevice::checkAlarms() {
  if (!_timeSynced) return;
  
  time_t now;
  time(&now);
  struct tm* ti = localtime(&now);
  
  for (uint8_t i = 0; i < HERMES_MAX_ALARMS; i++) {
    if (_alarms[i].enabled && !_alarms[i].triggered) {
      if (_alarms[i].hour == ti->tm_hour && _alarms[i].minute == ti->tm_min) {
        _alarms[i].triggered = true;
        addNotification("Alarm", "Time's up!");
      }
    }
    if (_alarms[i].triggered && (_alarms[i].hour != ti->tm_hour || _alarms[i].minute != ti->tm_min)) {
      _alarms[i].triggered = false;
    }
  }
}

void HermesDevice::checkTimer() {
  if (_timerRunning) {
    uint32_t elapsed = (millis() - _timerStartMs) / 1000;
    if (elapsed >= _timerDuration) {
      _timerRunning = false;
      addNotification("Timer", "Time's up!");
    }
  }
}

void HermesDevice::reconnectSocket() {
  if (_gatewayUrl.length() > 0 && _socket.getAuthToken().length() > 0) {
    _socket.connect(_gatewayUrl);
  }
}

void HermesDevice::loop() {
  // Socket.IO loop
  if (_socket.isConnected()) {
    _socket.loop();
  } else if (_wifiConnected && _socket.getAuthToken().length() > 0 && _socket.getFailureCount() < 5) {
    if (millis() - _socket.getLastActivity() > HERMES_SOCKET_RECONNECT_MS) {
      reconnectSocket();
    }
  }
  
  // Periodic tasks
  static uint32_t lastPeriodic = 0;
  if (millis() - lastPeriodic > 1000) {
    lastPeriodic = millis();
    
    if (_wifiConnected && (!_timeSynced || (time(nullptr) - _lastNtpSync) > HERMES_NTP_SYNC_INTERVAL)) {
      syncTime();
    }
    
    checkAlarms();
    checkTimer();
  }
}

void HermesDevice::startWebServer(uint16_t port) {
  // Note: Web server implementation is device-specific
  // Users should implement this based on their hardware
  _webServerActive = true;
}

void HermesDevice::stopWebServer() {
  _webServerActive = false;
}
