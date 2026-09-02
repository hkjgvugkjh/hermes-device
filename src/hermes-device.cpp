/**
 * @file hermes-device.cpp
 * @brief Hermes Studio 设备协议库实现
 * 
 * 实现 hermes-device.h 中声明的所有类和函数
 * 包括: JSON 工具、ADPCM 编解码、Socket.IO 客户端、HermesDevice 设备类
 * 
 * 版本历史:
 * - v1.0.0 (2026-08-28): 初始版本
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
// JSON 工具实现
// ============================================================

/**
 * 转义 JSON 特殊字符
 * 将双引号、反斜杠、换行等字符转义为 JSON 安全格式
 */
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

/**
 * 反转义 JSON 特殊字符
 * 将 JSON 转义序列还原为原始字符
 */
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

/**
 * 从 JSON 字符串中提取字符串值
 * 简单实现，不支持嵌套对象和数组
 */
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

/**
 * 从 JSON 字符串中提取整数值
 * 支持字符串格式 ("123") 和数字格式 (123)
 */
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

/**
 * 从 JSON 字符串中提取浮点数值
 */
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

/**
 * 从 JSON 字符串中提取布尔值
 * 检查前4个字符是否为 "true"
 */
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

/**
 * 构建 JSON 字符串 (仅支持字符串值)
 * 格式: {"key1":"value1","key2":"value2"}
 */
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
// ADPCM 编解码器实现 (IMA-ADPCM 4-bit)
// ============================================================

/**
 * IMA-ADPCM 步长表 (89 个条目)
 * 定义了量化步长的变化范围: 7 ~ 32767
 * 步长越大，量化精度越低，动态范围越大
 */
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

/**
 * 索引调整表 (16 个条目)
 * 根据编码后的 nibble (4-bit) 值调整 stepIndex
 * 高4位为符号位，低3位决定调整幅度
 */
const int8_t HermesAdpcm::indexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

/**
 * ADPCM 编码: PCM → ADPCM
 * 
 * 算法流程:
 * 1. 计算差值: diff = sample - predictor
 * 2. 根据步长量化差值为 4-bit nibble
 * 3. 解码 nibble 更新 predictor
 * 4. 调整 stepIndex
 * 
 * 输出格式: 每字节包含 2 个样本 (低4位 + 高4位)
 */
int HermesAdpcm::encode(const int16_t* samples, size_t sampleCount, uint8_t* output, size_t outputSize) {
  int predictor = 0;      // 预测值 (初始为 0)
  int stepIndex = 0;      // 步长索引 (初始为 0)
  size_t outPos = 0;
  
  for (size_t i = 0; i < sampleCount && outPos < outputSize; i++) {
    int diff = samples[i] - predictor;
    int step = stepTable[stepIndex];
    
    // 编码为 4-bit nibble
    int nibble = 0;
    if (diff < 0) { nibble = 8; diff = -diff; }  // 符号位
    
    if (diff >= step) { nibble |= 4; diff -= step; }  // bit2
    step >>= 1;
    if (diff >= step) { nibble |= 2; diff -= step; }  // bit1
    step >>= 1;
    if (diff >= step) { nibble |= 1; }                // bit0
    
    // 解码 nibble 更新 predictor (用于下次预测)
    int diffPred = stepTable[stepIndex] >> 3;
    if (nibble & 4) diffPred += stepTable[stepIndex];
    if (nibble & 2) diffPred += stepTable[stepIndex] >> 1;
    if (nibble & 1) diffPred += stepTable[stepIndex] >> 2;
    
    if (nibble & 8) predictor -= diffPred;
    else predictor += diffPred;
    
    // 限幅
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    
    // 调整步长索引
    stepIndex += indexTable[nibble];
    if (stepIndex < 0) stepIndex = 0;
    if (stepIndex > 88) stepIndex = 88;
    
    // 打包输出: 偶数样本在低4位，奇数样本在高4位
    if (i & 1) {
      output[outPos++] = ((nibble & 0x0F) << 4);
    } else {
      output[outPos] = nibble & 0x0F;
    }
  }
  
  return outPos;
}

/**
 * ADPCM 解码: ADPCM → PCM
 * 
 * 算法流程:
 * 1. 从字节中提取 4-bit nibble (先低后高)
 * 2. 根据步长和 nibble 计算差值
 * 3. 更新 predictor
 * 4. 调整 stepIndex
 * 
 * 输出: 16-bit PCM 样本
 */
int HermesAdpcm::decode(const uint8_t* input, size_t inputBytes, int16_t* samples, size_t maxSamples) {
  int predictor = 0;
  int stepIndex = 0;
  size_t samplePos = 0;
  
  for (size_t i = 0; i < inputBytes && samplePos < maxSamples; i++) {
    uint8_t byte = input[i];
    // 每个字节包含 2 个样本: 先低4位，后高4位
    for (int nibIdx = 0; nibIdx < 2 && samplePos < maxSamples; nibIdx++) {
      int nibble = (nibIdx == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
      int step = stepTable[stepIndex];
      
      // 计算差值
      int diffPred = step >> 3;
      if (nibble & 4) diffPred += step;
      if (nibble & 2) diffPred += step >> 1;
      if (nibble & 1) diffPred += step >> 2;
      
      if (nibble & 8) predictor -= diffPred;
      else predictor += diffPred;
      
      // 限幅
      if (predictor > 32767) predictor = 32767;
      if (predictor < -32768) predictor = -32768;
      
      // 调整步长索引
      stepIndex += indexTable[nibble];
      if (stepIndex < 0) stepIndex = 0;
      if (stepIndex > 88) stepIndex = 88;
      
      samples[samplePos++] = (int16_t)predictor;
    }
  }
  
  return samplePos;
}

// ============================================================
// Socket.IO 客户端实现
// ============================================================

HermesSocketIO::HermesSocketIO() 
  : _client(&_plainClient), _connected(false), _namespaceReady(false), 
    _wsUpgraded(false), _port(80), _useSSL(false), _connectTime(0),
    _lastActivity(0), _lastPing(0), _failureCount(0), _firstData(false),
    _proxyPort(0) {
  _namespace = "/global-agent";
}

HermesSocketIO::~HermesSocketIO() {
  disconnect();
}

// ============================================================
// 代理配置
// ============================================================

void HermesSocketIO::setProxy(const String& host, uint16_t port, const String& user, const String& pass) {
  _proxyHost = host;
  _proxyPort = port;
  _proxyUser = user;
  _proxyPass = pass;
  Serial.printf("[Proxy] 配置代理: %s:%d\n", host.c_str(), port);
}

void HermesSocketIO::clearProxy() {
  _proxyHost = "";
  _proxyPort = 0;
  _proxyUser = "";
  _proxyPass = "";
}

/**
 * 通过 HTTP CONNECT 代理建立隧道
 * 返回 true=隧道建立成功
 */
bool HermesSocketIO::connectThroughProxy(const String& targetHost, uint16_t targetPort) {
  if (_proxyHost.length() == 0) return false;
  
  Serial.printf("[Proxy] 连接到代理服务器 %s:%d...\n", _proxyHost.c_str(), _proxyPort);
  
  // 1. 连接到代理服务器
  if (!_client->connect(_proxyHost.c_str(), _proxyPort)) {
    Serial.println("[Proxy] 代理服务器连接失败");
    return false;
  }
  
  // 2. 发送 HTTP CONNECT 请求
  Serial.printf("[Proxy] 请求 CONNECT %s:%d\n", targetHost.c_str(), targetPort);
  _client->print("CONNECT " + targetHost + ":" + String(targetPort) + " HTTP/1.1\r\n");
  _client->print("Host: " + targetHost + ":" + String(targetPort) + "\r\n");
  
  // 添加代理认证（如果需要）
  if (_proxyUser.length() > 0) {
    String auth = _proxyUser + ":" + _proxyPass;
    String authBase64 = base64Encode(auth);
    _client->print("Proxy-Authorization: Basic " + authBase64 + "\r\n");
  }
  
  _client->print("Proxy-Connection: Keep-Alive\r\n");
  _client->print("\r\n");
  
  // 3. 等待 200 Connection Established
  uint32_t start = millis();
  String response = "";
  String prevLine;
  
  while (millis() - start < 10000) {
    if (_client->available()) {
      String line = _client->readStringUntil('\n');
      
      if (response.length() == 0) {
        response = line;  // 保存第一行 (状态行)
      }
      
      if (prevLine == "\r" || line == "\r") break;
      if (line.length() > 0 && line != "\r") prevLine = line;
    }
  }
  
  // 4. 检查响应状态
  if (response.indexOf("200") > 0) {
    Serial.println("[Proxy] 隧道建立成功");
    
    // 读取剩余空行
    while (_client->available()) {
      String line = _client->readStringUntil('\n');
      if (line == "\r" || line.length() == 0) break;
    }
    
    return true;
  }
  
  Serial.printf("[Proxy] 隧道建立失败: %s\n", response.c_str());
  _client->stop();
  return false;
}

/**
 * Base64 编码（用于代理认证）
 */
String HermesSocketIO::base64Encode(const String& input) {
  static const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  
  String output;
  int i = 0;
  uint8_t arr3[3], arr4[4];
  int len = input.length();
  
  for (int j = 0; j < len; j++) {
    arr3[i++] = input[j];
    if (i == 3) {
      arr4[0] = (arr3[0] & 0xFC) >> 2;
      arr4[1] = ((arr3[0] & 0x03) << 4) | ((arr3[1] & 0xF0) >> 4);
      arr4[2] = ((arr3[1] & 0x0F) << 2) | ((arr3[2] & 0xC0) >> 6);
      arr4[3] = arr3[2] & 0x3F;
      
      for (int k = 0; k < 4; k++) output += b64Table[arr4[k]];
      i = 0;
    }
  }
  
  if (i > 0) {
    for (int j = i; j < 3; j++) arr3[j] = 0;
    arr4[0] = (arr3[0] & 0xFC) >> 2;
    arr4[1] = ((arr3[0] & 0x03) << 4) | ((arr3[1] & 0xF0) >> 4);
    arr4[2] = ((arr3[1] & 0x0F) << 2) | ((arr3[2] & 0xC0) >> 6);
    
    for (int j = 0; j < i + 1; j++) output += b64Table[arr4[j]];
    while (i++ < 3) output += '=';
  }
  
  return output;
}

/**
 * 连接到 Socket.IO 服务器
 * 
 * 连接流程:
 * 1. 解析 URL (支持 http:// 和 https://)
 * 2. 建立 TCP 连接
 * 3. 发送 HTTP WebSocket 升级请求
 * 4. 等待 101 Switching Protocols 响应
 * 5. 发送 namespace connect
 */
bool HermesSocketIO::connect(const String& url) {
  // 解析 URL
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
  
  // 解析端口
  _port = 80;
  int colon = host.lastIndexOf(':');
  if (colon > 0) {
    _port = host.substring(colon + 1).toInt();
    _host = host.substring(0, colon);
  } else {
    _host = host;
    if (_useSSL) _port = 443;
  }
  
  Serial.printf("Socket.IO: 连接到 %s:%d (ssl=%d, proxy=%s)\n", 
                _host.c_str(), _port, _useSSL, _proxyHost.length() > 0 ? "yes" : "no");
  
  // 建立 TCP 连接（直连或通过代理隧道）
  bool connected = false;
  if (_proxyHost.length() > 0) {
    // 通过 HTTP CONNECT 代理建立隧道
    connected = connectThroughProxy(_host, _port);
  } else {
    // 直连
    connected = _client->connect(_host.c_str(), _port);
  }
  
  if (!connected) {
    _failureCount++;
    Serial.printf("Socket.IO: 连接失败 (%d)\n", _failureCount);
    return false;
  }
  
  // 发送 HTTP WebSocket 升级请求
  String path = "/socket.io/?EIO=4&transport=websocket";
  _client->print("GET " + path + " HTTP/1.1\r\n");
  _client->print("Host: " + _host + "\r\n");
  _client->print("Upgrade: websocket\r\n");
  _client->print("Connection: Upgrade\r\n");
  _client->print("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
  _client->print("Sec-WebSocket-Version: 13\r\n");
  _client->print("\r\n");
  
  // 读取 HTTP 响应直到空行
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
  
  // 发送 Engine.IO namespace connect
  delay(100);
  sendNamespaceConnect();
  
  Serial.println("Socket.IO: 连接成功");
  return true;
}

/**
 * 断开连接
 */
void HermesSocketIO::disconnect() {
  if (_client->connected()) {
    _client->stop();
  }
  _connected = false;
  _namespaceReady = false;
  _wsUpgraded = false;
  markDisconnected();
}

/**
 * 发送 WebSocket 帧
 * @param opcode 帧类型 (0x1=文本, 0x2=二进制, 0x8=关闭, 0x9=ping, 0xA=pong)
 * @param data 数据缓冲区
 * @param length 数据长度
 */
bool HermesSocketIO::sendRawWsFrame(uint8_t opcode, const uint8_t* data, size_t length) {
  if (!_client->connected()) return false;
  
  uint8_t header[14];
  size_t headerLen = 0;
  header[headerLen++] = 0x80 | (opcode & 0x0F);  // FIN + opcode
  
  // 长度编码 (支持掩码)
  if (length < 126) {
    header[headerLen++] = 0x80 | static_cast<uint8_t>(length);
  } else if (length <= 0xFFFF) {
    header[headerLen++] = 0x80 | 126;
    header[headerLen++] = static_cast<uint8_t>((length >> 8) & 0xFF);
    header[headerLen++] = static_cast<uint8_t>(length & 0xFF);
  } else {
    return false;  // 不支持超过 65535 字节
  }
  
  // 生成掩码 (客户端到服务器必须掩码)
  uint8_t mask[4];
  for (uint8_t i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(esp_random() & 0xFF);
  for (uint8_t i = 0; i < 4; ++i) header[headerLen++] = mask[i];
  
  if (!writeBytes(header, headerLen)) return false;
  
  // 分块发送并掩码
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

/**
 * 发送文本帧
 */
bool HermesSocketIO::sendRawWsText(const String& payload) {
  return sendRawWsFrame(0x1, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
}

/**
 * 写入字节到 TCP 连接
 */
bool HermesSocketIO::writeBytes(const uint8_t* data, size_t length) {
  if (!_client->connected()) return false;
  size_t written = _client->write(data, length);
  return written == length;
}

/**
 * 从 TCP 连接读取字节 (带超时)
 */
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

/**
 * 发送事件 (自动注入 apiToken)
 * 格式: 42/namespace,["eventName",{json}]
 */
bool HermesSocketIO::sendEvent(const String& event, const String& json) {
  if (!_connected || !_namespaceReady || event.length() == 0) return false;
  
  // 自动注入 apiToken (如果 JSON 中没有)
  String securedJson = json;
  if (_authToken.length() > 0 && json.length() > 0 && json[0] == '{' && json.indexOf("\"apiToken\"") < 0) {
    securedJson = "{\"apiToken\":\"" + HermesJson::escape(_authToken) + "\"," + json.substring(1);
  }
  
  // 构建 Socket.IO 事件帧
  String payload;
  payload.reserve(securedJson.length() + event.length() + 28);
  payload += "42" + _namespace + ",\"";
  payload += HermesJson::escape(event);
  payload += "\",";
  payload += securedJson;
  payload += "]";
  
  return sendRawWsText(payload);
}

/**
 * 发送 JSON 消息 (自动提取 type 字段作为事件名)
 */
bool HermesSocketIO::sendJson(const String& json) {
  String type = HermesJson::getString(json, "type");
  if (type.length() == 0) type = "mcu.event";
  return sendEvent(type, json);
}

/**
 * 发送 namespace connect 请求
 * 格式: 40/namespace
 */
bool HermesSocketIO::sendNamespaceConnect() {
  if (!_connected || _authToken.length() == 0) return false;
  
  String frame = "40" + _namespace;
  sendRawWsText(frame);
  return true;
}

/**
 * 发送二进制帧
 */
bool HermesSocketIO::sendBinary(const uint8_t* data, size_t length) {
  return sendRawWsFrame(0x2, data, length);
}

/**
 * 主循环处理
 * 处理接收到的 WebSocket 帧
 */
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
    
    // 扩展长度
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
    
    // 检查帧大小
    if (length > HERMES_MAX_WS_FRAME_SIZE) {
      Serial.println("WS 帧过大");
      _connected = false;
      _client->stop();
      return;
    }
    
    // 处理不同类型的帧
    if (opcode == 0x1) {
      // 文本帧
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
      // 二进制帧
      uint8_t buf[512];
      uint64_t remaining = length;
      size_t totalRead = 0;
      uint8_t* binaryBuf = nullptr;
      if (length > 0) {
        binaryBuf = (uint8_t*)malloc(length);
        if (binaryBuf) {
          while (remaining > 0) {
            size_t toRead = min(static_cast<size_t>(remaining), sizeof(buf));
            if (!readBytes(buf, toRead)) break;
            memcpy(binaryBuf + totalRead, buf, toRead);
            totalRead += toRead;
            remaining -= toRead;
          }
          if (_binaryCb) _binaryCb(binaryBuf, totalRead);
        }
      }
      if (binaryBuf) free(binaryBuf);
    } else if (opcode == 0x8) {
      // 关闭帧
      _connected = false;
      _client->stop();
      return;
    } else if (opcode == 0x9) {
      // Ping - 回复 pong
      sendRawWsFrame(0xA, nullptr, 0);
    }
  }
}

/**
 * 处理接收到的消息
 * Engine.IO 协议:
 * - "0": Hello (连接初始化)
 * - "3": Pong (心跳响应)
 * - "40": Namespace 确认
 * - "42": 事件
 */
void HermesSocketIO::handleMessage(const String& message) {
  if (_rawMessageCb) _rawMessageCb(message);
  
  // Engine.IO v4 协议
  if (message == "2") {
    // Ping from server
    sendRawWsText("3");
    return;
  }
  if (message.startsWith("0")) {
    // Engine.IO hello - 发送 namespace connect
    sendNamespaceConnect();
    return;
  }
  if (message.startsWith("40")) {
    // Namespace 确认
    _namespaceReady = true;
    if (_connectedCb) _connectedCb();
    return;
  }
  if (message.startsWith("42")) {
    // 事件
    handleSocketIoEvent(message);
    return;
  }
}

/**
 * 处理 Socket.IO 事件
 * 格式: 42/namespace,["eventName",{json}]
 */
void HermesSocketIO::handleSocketIoEvent(const String& message) {
  String event, json;
  if (parseEvent(message, &event, &json)) {
    if (_eventCb) _eventCb(event, json);
  }
}

/**
 * 解析 Socket.IO 事件消息
 * @param message Socket.IO 消息
 * @param event 输出: 事件名
 * @param json 输出: JSON payload
 * @return true=解析成功
 */
bool HermesSocketIO::parseEvent(const String& message, String* event, String* json) {
  int arrayStart = message.indexOf('[');
  if (arrayStart < 0) return false;
  
  String arrayContent = message.substring(arrayStart + 1, message.lastIndexOf(']'));
  
  // 提取事件名 (第一个字符串)
  int firstQuote = arrayContent.indexOf('"');
  if (firstQuote < 0) return false;
  int secondQuote = arrayContent.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return false;
  
  *event = arrayContent.substring(firstQuote + 1, secondQuote);
  
  // 提取 JSON payload
  int comma = arrayContent.indexOf(',', secondQuote + 1);
  if (comma < 0) {
    *json = "";
    return true;
  }
  
  *json = arrayContent.substring(comma + 1);
  json->trim();
  
  return true;
}

/**
 * 标记连接成功
 */
void HermesSocketIO::markConnected() {
  _connectTime = millis();
  _lastActivity = millis();
  _firstData = false;
}

/**
 * 标记连接断开
 */
void HermesSocketIO::markDisconnected() {
  _connectTime = 0;
  _lastActivity = 0;
  if (_disconnectedCb) _disconnectedCb();
}

// ============================================================
// HermesDevice 事件处理
// ============================================================

/**
 * 处理 Socket.IO 事件
 * 根据事件名分发到对应的处理函数
 * 
 * 支持的事件类型:
 * - mcu.auth: 认证请求 (网关要求设备认证)
 * - mcu.interaction.status: 交互状态更新
 * - mcu.audio.enqueue: 音频播放请求
 * - mcu.session.clear: 会话清除
 * - mcu.auth.invalid: 认证失败
 * - mcu.reauth.required: 需要重新认证
 */
void HermesDevice::handleSocketEvent(const String& event, const String& json) {
  if (event == "mcu.auth") {
    handleMcuAuth(json);
  } else if (event == "mcu.interaction.status") {
    handleInteractionStatus(json);
  } else if (event == "mcu.audio.enqueue") {
    handleAudioEnqueue(json);
  } else if (event == "mcu.session.clear") {
    handleSessionClear();
  } else if (event == "mcu.auth.invalid") {
    handleAuthInvalid();
  } else if (event == "mcu.reauth.required") {
    handleReauthRequired();
  }
}

/**
 * 处理认证请求 (mcu.auth)
 * 
 * 认证流程:
 * 1. 网关发送 mcu.auth 事件 (包含挑战码)
 * 2. 设备回复 mcu.auth.ok (包含 apiToken)
 * 3. 设备发送 mcu.ready (包含能力声明)
 * 
 * JSON 格式: {"type":"mcu.auth","id":"...","challenge":"..."}
 */
void HermesDevice::handleMcuAuth(const String& json) {
  // 回复认证成功
  String okJson = "{\"type\":\"mcu.auth.ok\",\"apiToken\":\"" + _socket.getAuthToken() + 
                  "\",\"ok\":true,\"id\":\"" + _deviceId + "\"}";
  _socket.sendEvent("mcu.auth.ok", okJson);
  
  // 上报就绪状态
  reportReady();
  
  _authState = HermesAuthState::Authenticated;
  _ui(HermesUiState::READY, "IP: " + WiFi.localIP().toString());
  Serial.println("HermesDevice: 认证成功");
}

/**
 * 处理交互状态更新 (mcu.interaction.status)
 * 
 * 状态流转:
 * - listening → thinking → playing → idle
 * - 任何状态 → error
 * 
 * JSON 格式: {"type":"mcu.interaction.status","id":"...","status":"...","text":"..."}
 */
void HermesDevice::handleInteractionStatus(const String& json) {
  String id = HermesJson::getString(json, "id");
  String status = HermesJson::getString(json, "status");
  String text = HermesJson::getString(json, "text");
  
  if (_interactionCb) {
    _interactionCb(id, status, text);
  }
  
  // 更新内部状态
  if (status == "listening") _interactionStatus = HermesInteractionStatus::Listening;
  else if (status == "thinking") _interactionStatus = HermesInteractionStatus::Thinking;
  else if (status == "playing") _interactionStatus = HermesInteractionStatus::Playing;
  else if (status == "idle") _interactionStatus = HermesInteractionStatus::Idle;
  else if (status == "error") _interactionStatus = HermesInteractionStatus::Error;
}

/**
 * 处理音频播放请求 (mcu.audio.enqueue)
 * 
 * 网关通过此事件要求设备播放音频
 * 音频数据通过 URL 提供，设备需要下载后播放
 * 
 * JSON 格式: {"type":"mcu.audio.enqueue","url":"...","mimeType":"audio/mpeg","sampleRate":16000}
 */
void HermesDevice::handleAudioEnqueue(const String& json) {
  String url = HermesJson::getString(json, "url");
  String mimeType = HermesJson::getString(json, "mimeType");
  uint32_t sampleRate = HermesJson::getInt(json, "sampleRate", 16000);
  
  if (_audioCb) {
    _audioCb(url, mimeType, sampleRate);
  }
}

/**
 * 处理会话清除 (mcu.session.clear)
 * 网关通知设备清除当前会话状态
 */
void HermesDevice::handleSessionClear() {
  _currentInteractionId = "";
  _interactionStatus = HermesInteractionStatus::Idle;
  
  if (_sessionClearCb) {
    _sessionClearCb();
  }
  
  Serial.println("HermesDevice: 会话已清除");
}

/**
 * 处理认证失败 (mcu.auth.invalid)
 * 网关拒绝设备的认证请求 (token 过期或无效)
 */
void HermesDevice::handleAuthInvalid() {
  _authState = HermesAuthState::Failed;
  Serial.println("HermesDevice: 认证失败");
  
  if (_authRequiredCb) {
    _authRequiredCb();
  }
}

/**
 * 处理重新认证请求 (mcu.reauth.required)
 * 网关要求设备重新进行认证
 */
void HermesDevice::handleReauthRequired() {
  _authState = HermesAuthState::Disconnected;
  Serial.println("HermesDevice: 需要重新认证");
  
  // 尝试重新登录
  if (_account.length() > 0) {
    login(_account, _password, _profile);
  } else if (_authRequiredCb) {
    _authRequiredCb();
  }
}

/**
 * 通知 UI 状态变化
 */
void HermesDevice::_ui(HermesUiState state, const String& text) {
  _uiState = state;
  if (_uiStateCb) _uiStateCb(state, text);
  Serial.printf("[UI] state=%d text=%s\n", (int)state, text.c_str());
}

/**
 * 构造函数
 * 初始化所有成员变量为默认值
 */
HermesDevice::HermesDevice() 
  : _wifiConnected(false), _apMode(false), _authState(HermesAuthState::Disconnected),
    _port(8648), _sampleRate(HERMES_DEFAULT_SAMPLE_RATE), 
    _recordMaxMs(HERMES_DEFAULT_RECORD_MAX_MS), _recordMinMs(HERMES_DEFAULT_RECORD_MIN_MS),
    _timerStartMs(0), _timerDuration(0), _timerRunning(false),
    _timeSynced(false), _lastNtpSync(0), _webServer(nullptr), _webServerActive(false),
    _uiState(HermesUiState::BOOT), _proxyPort(0), _useProxy(false) {
  _deviceName = "HStudio-Device";
  _deviceType = "global_agent";
  _namespaceName = "/global-agent";
}

// ============================================================
// 代理配置
// ============================================================

void HermesDevice::setProxyConfig(const String& host, uint16_t port, const String& user, const String& pass) {
  _proxyHost = host;
  _proxyPort = port;
  _proxyUser = user;
  _proxyPass = pass;
  _useProxy = true;
  
  // 同时配置到 Socket.IO 客户端
  _socket.setProxy(host, port, user, pass);
  
  Serial.printf("[Proxy] 设备代理配置: %s:%d\n", host.c_str(), port);
}

void HermesDevice::clearProxyConfig() {
  _proxyHost = "";
  _proxyPort = 0;
  _proxyUser = "";
  _proxyPass = "";
  _useProxy = false;
  
  _socket.clearProxy();
  
  Serial.println("[Proxy] 设备代理已清除");
}

/**
 * 析构函数
 * 断开 Socket.IO 连接
 */
HermesDevice::~HermesDevice() {
  _socket.disconnect();
}

/**
 * 初始化设备
 * 生成设备 ID，加载保存的配置
 */
void HermesDevice::begin(const String& deviceName, const String& deviceType, const String& namespaceName) {
  _deviceName = deviceName;
  _deviceType = deviceType;
  _namespaceName = namespaceName;
  
  generateDeviceId();
  loadPreferences();
  
  _socket.setNamespace(namespaceName);
  
  _ui(HermesUiState::BOOT, _deviceName + " " + HERMES_DEVICE_LIB_VERSION);
  Serial.printf("HermesDevice: 初始化完成 (ID=%s, Code=%s)\n", _deviceId.c_str(), _deviceCode.c_str());
}

/**
 * 生成设备 ID (基于 MAC 地址)
 */
void HermesDevice::generateDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  _deviceId = String(buf);
  
  // 设备代码: 后8位
  _deviceCode = _deviceId.substring(4);
}

/**
 * 加载保存的配置
 */
void HermesDevice::loadPreferences() {
  _prefs.begin("hermes", true);
  _savedSsid = _prefs.getString("wifi/ssid", "");
  _savedPass = _prefs.getString("wifi/pass", "");
  _gatewayUrl = _prefs.getString("mcu/active_url", "");
  _proxyHost = _prefs.getString("mcu/proxy_host", "");
  _proxyPort = _prefs.getUShort("mcu/proxy_port", 0);
  _proxyUser = _prefs.getString("mcu/proxy_user", "");
  _proxyPass = _prefs.getString("mcu/proxy_pass", "");
  _prefs.end();
  
  // 如果有保存的代理配置，自动应用
  if (_proxyHost.length() > 0 && _proxyPort > 0) {
    _useProxy = true;
    _socket.setProxy(_proxyHost, _proxyPort, _proxyUser, _proxyPass);
    Serial.printf("[Proxy] 已加载代理配置: %s:%d\n", _proxyHost.c_str(), _proxyPort);
  }
}

/**
 * 保存认证信息
 */
void HermesDevice::saveCredentials() {
  _prefs.begin("hermes", false);
  _prefs.putString("wifi/ssid", _savedSsid);
  _prefs.putString("wifi/pass", _savedPass);
  _prefs.putString("mcu/active_url", _gatewayUrl);
  _prefs.putString("mcu/proxy_host", _proxyHost);
  _prefs.putUShort("mcu/proxy_port", _proxyPort);
  _prefs.putString("mcu/proxy_user", _proxyUser);
  _prefs.putString("mcu/proxy_pass", _proxyPass);
  _prefs.end();
}

/**
 * 清除认证信息
 */
void HermesDevice::clearCredentials() {
  _prefs.begin("hermes", false);
  _prefs.remove("mcu/auth_token");
  _prefs.end();
}

/**
 * 连接 WiFi
 */
bool HermesDevice::connectWifi(const String& ssid, const String& pass, bool save) {
  _ui(HermesUiState::WIFI_CONNECTING, ssid);
  Serial.printf("WiFi: 连接到 %s\n", ssid.c_str());
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < HERMES_WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    _wifiConnected = true;
    if (save) {
      _savedSsid = ssid;
      _savedPass = pass;
      saveCredentials();
    }
    String ip = WiFi.localIP().toString();
    _ui(HermesUiState::WIFI_OK, "IP: " + ip);
    Serial.printf("WiFi: 已连接, IP=%s\n", ip.c_str());
    return true;
  }
  
  _ui(HermesUiState::ERROR, "WiFi failed");
  Serial.println("WiFi: 连接失败");
  return false;
}

/**
 * 连接保存的 WiFi
 */
bool HermesDevice::connectSavedWifi() {
  if (_savedSsid.length() == 0) return false;
  return connectWifi(_savedSsid, _savedPass, false);
}

/**
 * 断开 WiFi
 */
void HermesDevice::disconnectWifi() {
  WiFi.disconnect();
  _wifiConnected = false;
}

/**
 * 启动 AP 配网模式
 */
void HermesDevice::startSetupAp(const String& ssid, const String& password) {
  WiFi.mode(WIFI_AP);
  if (password.length() > 0) {
    WiFi.softAP(ssid.c_str(), password.c_str());
  } else {
    WiFi.softAP(ssid.c_str());
  }
  _apMode = true;
  _ui(HermesUiState::AP_CONFIG, ssid);
  Serial.printf("AP: %s 已启动, IP=%s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
}

/**
 * 自动发现网关
 * 策略: mDNS → 网络扫描 → 保存的 URL
 */
String HermesDevice::discoverGateway() {
  _ui(HermesUiState::DISCOVER, "Finding gateway...");
  
  // 1. 尝试 mDNS
  Serial.println("Gateway: 尝试 mDNS 发现...");
  int n = MDNS.queryService("http", "tcp");
  for (int i = 0; i < n; i++) {
    String name = MDNS.hostname(i);
    if (name.indexOf("hermes") >= 0 || name.indexOf("studio") >= 0) {
      String url = "http://" + MDNS.IP(i).toString() + ":" + String(MDNS.port(i));
      Serial.printf("Gateway: mDNS 发现 %s\n", url.c_str());
      return url;
    }
  }
  
  // 2. 尝试保存的 URL
  if (_gatewayUrl.length() > 0 && testGateway(_gatewayUrl)) {
    return _gatewayUrl;
  }
  
  // 3. 网络扫描 (常见 IP)
  Serial.println("Gateway: 扫描网络...");
  for (int i = 1; i < 255; i++) {
    String ip = "10.10.168." + String(i);
    if (testGateway(ip)) {
      return "http://" + ip + ":8648";
    }
  }
  
  Serial.println("Gateway: 未找到");
  return "";
}

/**
 * 测试指定 IP 是否为 Hermes Studio 网关
 */
bool HermesDevice::testGateway(const String& ip) {
  HTTPClient http;
  http.begin(ip + "/api/hermes/health");
  http.setTimeout(2000);
  int code = http.GET();
  http.end();
  return code == 200;
}

/**
 * 登录 (HTTP POST + Socket.IO 认证)
 */
bool HermesDevice::login(const String& account, const String& password, const String& profile) {
  _account = account;
  _password = password;
  _profile = profile;
  
  _ui(HermesUiState::LOGIN, "Logging in...");
  Serial.printf("Login: 登录 %s\n", account.c_str());
  
  // HTTP POST 获取 token
  HTTPClient http;
  http.begin(_gatewayUrl + "/api/auth/mcu-login");
  http.addHeader("Content-Type", "application/json");
  
  String payload = "{\"token\":\"" + _deviceId + "\",\"id\":\"" + _deviceId + 
                   "\",\"device_code\":\"" + _deviceCode + "\",\"device_type\":\"" + 
                   _deviceType + "\",\"source\":\"global_agent\",\"account\":\"" + 
                   account + "\",\"password\":\"" + password + "\",\"relayMode\":\"lan\"}";
  
  int code = http.POST(payload);
  if (code != 200) {
    Serial.printf("Login: HTTP 失败 %d\n", code);
    _ui(HermesUiState::ERROR, "Login failed: " + String(code));
    return false;
  }
  
  String response = http.getString();
  http.end();
  
  // 提取 token
  String token = HermesJson::getString(response, "token");
  if (token.length() == 0) {
    Serial.println("Login: 无 token");
    _ui(HermesUiState::ERROR, "No token");
    return false;
  }
  
  _socket.setAuthToken(token);
  _authState = HermesAuthState::Authenticating;
  
  _ui(HermesUiState::SOCKET, "Connecting...");
  
  // 连接 Socket.IO
  if (_socket.connect(_gatewayUrl)) {
    return true;
  }
  
  _ui(HermesUiState::ERROR, "Socket failed");
  return false;
}

/**
 * 登出
 */
void HermesDevice::logout() {
  _socket.disconnect();
  _authState = HermesAuthState::Disconnected;
  clearCredentials();
}

/**
 * 开始语音交互
 */
bool HermesDevice::startVoiceInteraction(const String& interactionId) {
  _currentInteractionId = interactionId;
  _interactionStatus = HermesInteractionStatus::Listening;
  
  String json = "{\"type\":\"voice.stream.start\",\"id\":\"" + interactionId + "\"}";
  return _socket.sendEvent("voice.stream.start", json);
}

/**
 * 发送语音数据块
 */
bool HermesDevice::sendVoiceChunk(const String& interactionId, const uint8_t* data, size_t length, uint32_t offset) {
  // 发送文本帧 (元数据)
  String json = "{\"type\":\"voice.stream.chunk\",\"id\":\"" + interactionId + 
                "\"," + String(offset) + "}";
  _socket.sendEvent("voice.stream.chunk", json);
  
  // 发送二进制帧 (实际数据)
  return _socket.sendBinary(data, length);
}

/**
 * 结束语音交互
 */
bool HermesDevice::endVoiceInteraction(const String& interactionId, uint32_t totalBytes) {
  _interactionStatus = HermesInteractionStatus::Thinking;
  
  String json = "{\"type\":\"voice.stream.end\",\"id\":\"" + interactionId + 
                "\",\"totalBytes\":" + String(totalBytes) + "}";
  return _socket.sendEvent("voice.stream.end", json);
}

/**
 * 上报状态
 */
void HermesDevice::reportStatus(const String& interactionId, const String& status, 
                                bool audioPlaying, uint32_t queueLength) {
  String json = "{\"type\":\"mcu.status\",\"id\":\"" + interactionId + 
                "\",\"status\":\"" + status + "\",\"audioPlaying\":" + 
                (audioPlaying ? "true" : "false") + ",\"queueLength\":" + 
                String(queueLength) + "}";
  _socket.sendEvent("mcu.status", json);
}

/**
 * 上报就绪
 */
void HermesDevice::reportReady() {
  String json = "{\"type\":\"mcu.ready\",\"id\":\"" + _deviceId + 
                "\",\"active_device\":true,\"profile\":\"" + _profile + 
                "\",\"capabilities\":{\"display\":true,\"audio_queue\":true," +
                "\"audio_playback\":true,\"pcm_stream\":false}}";
  _socket.sendEvent("mcu.ready", json);
}

/**
 * 添加通知
 */
void HermesDevice::addNotification(const String& title, const String& msg) {
  if (_notifications.size() >= HERMES_MAX_NOTIFICATIONS) {
    _notifications.erase(_notifications.begin());  // 移除最旧的
  }
  HermesNotification n = {title, msg, millis(), false};
  _notifications.push_back(n);
  if (_notificationCb) _notificationCb(title, msg);
}

/**
 * 清除所有通知
 */
void HermesDevice::clearNotifications() {
  _notifications.clear();
}

/**
 * 添加闹钟
 */
bool HermesDevice::addAlarm(uint8_t hour, uint8_t minute) {
  for (uint8_t i = 0; i < HERMES_MAX_ALARMS; i++) {
    if (!_alarms[i].enabled) {
      _alarms[i].hour = hour;
      _alarms[i].minute = minute;
      _alarms[i].enabled = true;
      _alarms[i].triggered = false;
      return true;
    }
  }
  return false;  // 闹钟已满
}

/**
 * 删除闹钟
 */
void HermesDevice::removeAlarm(uint8_t index) {
  if (index < HERMES_MAX_ALARMS) {
    _alarms[index].enabled = false;
  }
}

/**
 * 启用/禁用闹钟
 */
void HermesDevice::toggleAlarm(uint8_t index, bool enabled) {
  if (index < HERMES_MAX_ALARMS) {
    _alarms[index].enabled = enabled;
    _alarms[index].triggered = false;
  }
}

/**
 * 启动定时器
 */
void HermesDevice::startTimer(uint32_t seconds) {
  _timerStartMs = millis();
  _timerDuration = seconds * 1000;
  _timerRunning = true;
}

/**
 * 停止定时器
 */
void HermesDevice::stopTimer() {
  _timerRunning = false;
}

/**
 * 获取定时器剩余时间 (秒)
 */
uint32_t HermesDevice::getTimerRemaining() const {
  if (!_timerRunning) return 0;
  uint32_t elapsed = millis() - _timerStartMs;
  if (elapsed >= _timerDuration) return 0;
  return (_timerDuration - elapsed) / 1000;
}

/**
 * 设置传感器数据
 */
void HermesDevice::setSensorData(float temp, float humidity, float batteryV, uint8_t batteryPct) {
  _sensors.temperature = temp;
  _sensors.humidity = humidity;
  _sensors.batteryVoltage = batteryV;
  _sensors.batteryPercent = batteryPct;
  _sensors.lastUpdate = millis();
}

/**
 * NTP 时间同步
 */
bool HermesDevice::syncTime(const char* ntpServer, long gmtOffset, int daylightOffset) {
  configTime(gmtOffset, daylightOffset, ntpServer);
  
  uint32_t start = millis();
  time_t now;
  time(&now);
  
  while (now < 1000000000 && millis() - start < 10000) {
    delay(500);
    time(&now);
  }
  
  if (now > 1000000000) {
    _timeSynced = true;
    _lastNtpSync = now;
    return true;
  }
  
  return false;
}

/**
 * 主循环处理 — 统一状态机驱动
 *
 * 状态流转:
 *   BOOT → WIFI_CONNECTING → WIFI_OK → DISCOVER → LOGIN → SOCKET → READY
 *                   ↓ (失败)
 *              AP_CONFIG (持续等待，不重启)
 */
void HermesDevice::loop() {
  static uint32_t lastReconnectAttempt = 0;
  
  _socket.loop();
  
  // AP 配网模式 — 持续等待，不自动重启
  if (_apMode) {
    // TODO: 处理 Web 配网请求
    return;
  }
  
  // WiFi 未连接 → 尝试连接
  if (!_wifiConnected) {
    if (_savedSsid.length() > 0) {
      connectSavedWifi();
    }
    return;
  }
  
  // WiFi 已连接但未开始发现 → 启动发现
  if (_wifiConnected && _gatewayUrl.length() == 0 && _authState == HermesAuthState::Disconnected) {
    String gw = discoverGateway();
    if (gw.length() > 0) {
      _gatewayUrl = gw;
      saveCredentials();
    } else {
      // 未找到网关，进入 AP 配网
      startSetupAp(_deviceName + "-" + _deviceCode.substring(4), "12345678");
      return;
    }
  }
  
  // 有网关但未登录 → 登录
  if (_gatewayUrl.length() > 0 && _authState == HermesAuthState::Disconnected) {
    if (_account.length() > 0) {
      login(_account, _password, _profile);
    }
    return;
  }
  
  // 认证后 Socket 断开 → 重连
  if (_authState == HermesAuthState::Authenticated && !_socket.isConnected()) {
    if (millis() - lastReconnectAttempt > HERMES_SOCKET_RECONNECT_MS) {
      lastReconnectAttempt = millis();
      reconnectSocket();
    }
    return;
  }
  
  // 已就绪 — 检查闹钟/定时器
  checkAlarms();
  checkTimer();
}

/**
 * 检查闹钟
 */
void HermesDevice::checkAlarms() {
  if (!_timeSynced) return;
  
  time_t now;
  time(&now);
  struct tm* tm = localtime(&now);
  
  for (uint8_t i = 0; i < HERMES_MAX_ALARMS; i++) {
    if (_alarms[i].enabled && !_alarms[i].triggered) {
      if (tm->tm_hour == _alarms[i].hour && tm->tm_min == _alarms[i].minute) {
        _alarms[i].triggered = true;
        // 触发闹钟 (播放提示音等)
        Serial.printf("闹钟 %d: %02d:%02d 触发\n", i, _alarms[i].hour, _alarms[i].minute);
      }
    }
  }
}

/**
 * 检查定时器
 */
void HermesDevice::checkTimer() {
  if (_timerRunning && getTimerRemaining() == 0) {
    _timerRunning = false;
    Serial.println("定时器结束");
  }
}

/**
 * 重连 Socket
 */
void HermesDevice::reconnectSocket() {
  if (!_socket.isConnected() && _gatewayUrl.length() > 0) {
    _socket.connect(_gatewayUrl);
  }
}

/**
 * 启动 Web 服务器 (配网用)
 */
void HermesDevice::startWebServer(uint16_t port) {
  // 实现略 (使用 WebServer 库)
}

/**
 * 停止 Web 服务器
 */
void HermesDevice::stopWebServer() {
  _webServerActive = false;
}
