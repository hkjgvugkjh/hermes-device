/**
 * @file hermes-device.h
 * @brief Hermes Studio 设备协议库 (Device Protocol Library)
 * 
 * 一个可复用的 C++ 库，用于将 ESP32 设备通过 Socket.IO (Engine.IO v4) 
 * 协议连接到 Hermes Studio 网关。支持语音对话、音频播放、设备管理等功能。
 * 
 * 协议栈:
 *   ┌─────────────────────────────────────────┐
 *   │  Hermes Studio 网关 (Node.js + Socket.IO)│
 *   ├─────────────────────────────────────────┤
 *   │  Engine.IO v4  ← 传输层 (WebSocket)     │
 *   ├─────────────────────────────────────────┤
 *   │  Socket.IO    ← 事件层 (namespace)      │
 *   ├─────────────────────────────────────────┤
 *   │  MCU Protocol ← 应用层 (JSON events)    │
 *   └─────────────────────────────────────────┘
 * 
 * 核心功能:
 * - Socket.IO 客户端 (WebSocket 传输)
 * - MCU 认证流程 (HTTP POST + Socket.IO 事件)
 * - 语音流 (IMA-ADPCM 4-bit 编解码)
 * - 事件驱动架构 (回调函数)
 * - 网关发现 (mDNS + 网络扫描)
 * - NTP 时间同步
 * - 闹钟/定时器管理
 * - 通知队列
 * - 传感器数据管理
 * - 连接失败检测与自动重连
 * 
 * 使用示例:
 * @code
 *   #include <hermes-device.h>
 *   
 *   HermesDevice device;
 *   
 *   void setup() {
 *     Serial.begin(115200);
 *     device.begin("HStudio-Device", "global_agent");
 *     device.connectWifi("YourSSID", "YourPassword");
 *     device.onInteraction([](const String& id, const String& status, const String& text) {
 *       Serial.printf("交互 %s: %s\n", id.c_str(), status.c_str());
 *     });
 *   }
 *   
 *   void loop() {
 *     device.loop();  // 必须在主循环中调用
 *   }
 * @endcode
 * 
 * 版本历史:
 * - v1.0.0 (2026-08-28): 初始版本
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
// 版本信息
// ============================================================
#define HERMES_DEVICE_LIB_VERSION "1.0.0"

// ============================================================
// 配置常量 (可通过 #ifndef 在项目中自定义)
// ============================================================

// --- 音频配置 ---
#ifndef HERMES_DEFAULT_SAMPLE_RATE
#define HERMES_DEFAULT_SAMPLE_RATE 16000    // 采样率 (Hz), 16kHz 为语音识别最佳
#endif

#ifndef HERMES_DEFAULT_RECORD_MAX_MS
#define HERMES_DEFAULT_RECORD_MAX_MS 15000  // 最大录音时长 (ms), 防止内存溢出
#endif

#ifndef HERMES_DEFAULT_RECORD_MIN_MS
#define HERMES_DEFAULT_RECORD_MIN_MS 500    // 最小录音时长 (ms), 过滤误触
#endif

#ifndef HERMES_DEFAULT_VAD_THRESHOLD
#define HERMES_DEFAULT_VAD_THRESHOLD 300    // 语音活动检测阈值 (0-4095)
#endif

// --- 网络配置 ---
#ifndef HERMES_MAX_WS_FRAME_SIZE
#define HERMES_MAX_WS_FRAME_SIZE 8192       // WebSocket 帧最大尺寸 (字节)
#endif

#ifndef HERMES_SOCKET_RECONNECT_MS
#define HERMES_SOCKET_RECONNECT_MS 5000     // Socket 重连间隔 (ms)
#endif

#ifndef HERMES_WIFI_CONNECT_TIMEOUT_MS
#define HERMES_WIFI_CONNECT_TIMEOUT_MS 15000 // WiFi 连接超时 (ms)
#endif

#ifndef HERMES_GATEWAY_PORT
#define HERMES_GATEWAY_PORT 8648            // Hermes Studio 默认端口
#endif

// --- 功能限制 ---
#ifndef HERMES_MAX_NOTIFICATIONS
#define HERMES_MAX_NOTIFICATIONS 10         // 最大通知数量
#endif

#ifndef HERMES_MAX_ALARMS
#define HERMES_MAX_ALARMS 5                 // 最大闹钟数量
#endif

// --- 时间同步 ---
#ifndef HERMES_NTP_SYNC_INTERVAL
#define HERMES_NTP_SYNC_INTERVAL 3600       // NTP 同步间隔 (秒)
#endif

// ============================================================
// 数据结构定义
// ============================================================

/**
 * 认证状态枚举
 * 描述设备与网关的认证流程各阶段
 */
enum class HermesAuthState {
  Disconnected,     // 未连接
  Connecting,       // 连接中 (正在建立 WebSocket)
  Authenticating,   // 认证中 (等待 mcu.auth 事件)
  Authenticated,    // 已认证 (可以正常通信)
  Failed            // 认证失败 (token 过期或错误)
};

/**
 * 交互状态枚举
 * 描述语音交互流程各阶段
 */
enum class HermesInteractionStatus {
  Idle,             // 空闲 (等待用户触发)
  Listening,        // 录音中 (正在采集音频)
  Thinking,         // 思考中 (网关正在处理)
  Playing,          // 播放中 (正在播放 AI 回复)
  Error             // 错误 (交互失败)
};

/**
 * 通知结构体
 * 存储单条通知消息
 */
struct HermesNotification {
  String title;         // 通知标题
  String message;       // 通知内容
  uint32_t timestamp;   // 时间戳 (Unix epoch)
  bool read;            // 是否已读
};

/**
 * 闹钟结构体
 * 存储单个闹钟设置
 */
struct HermesAlarm {
  uint8_t hour;         // 小时 (0-23)
  uint8_t minute;       // 分钟 (0-59)
  bool enabled;         // 是否启用
  bool triggered;       // 是否已触发 (当天)
};

/**
 * 传感器数据结构体
 * 存储环境传感器读数
 */
struct HermesSensorData {
  float temperature;    // 温度 (°C)
  float humidity;       // 湿度 (%)
  float batteryVoltage; // 电池电压 (V)
  uint8_t batteryPercent; // 电池百分比 (%)
  uint32_t lastUpdate;  // 最后更新时间 (ms)
};

// ============================================================
// JSON 工具类
// ============================================================

/**
 * JSON 工具类
 * 提供 JSON 字符串的解析和构建功能
 * 注意: 这是一个轻量级实现，不支持嵌套对象
 */
class HermesJson {
public:
  /**
   * 转义 JSON 特殊字符
   * @param s 原始字符串
   * @return 转义后的字符串
   */
  static String escape(const String& s);
  
  /**
   * 反转义 JSON 特殊字符
   * @param s 转义后的字符串
   * @return 原始字符串
   */
  static String unescape(const String& s);
  
  /**
   * 从 JSON 字符串中提取字符串值
   * @param json JSON 字符串
   * @param key 键名
   * @return 值字符串，未找到返回空字符串
   */
  static String getString(const String& json, const String& key);
  
  /**
   * 从 JSON 字符串中提取整数值
   * @param json JSON 字符串
   * @param key 键名
   * @param defaultValue 默认值
   * @return 整数值
   */
  static int getInt(const String& json, const String& key, int defaultValue = 0);
  
  /**
   * 从 JSON 字符串中提取浮点数值
   * @param json JSON 字符串
   * @param key 键名
   * @param defaultValue 默认值
   * @return 浮点数值
   */
  static float getFloat(const String& json, const String& key, float defaultValue = 0.0f);
  
  /**
   * 从 JSON 字符串中提取布尔值
   * @param json JSON 字符串
   * @param key 键名
   * @param defaultValue 默认值
   * @return 布尔值
   */
  static bool getBool(const String& json, const String& key, bool defaultValue = false);
  
  /**
   * 构建 JSON 字符串 (仅支持字符串值)
   * @param keys 键名数组
   * @param values 值数组
   * @param count 键值对数量
   * @return JSON 字符串
   */
  static String build(const char* keys[], const String* values, size_t count);
};

// ============================================================
// ADPCM 编解码器 (IMA-ADPCM 4-bit)
// ============================================================

/**
 * ADPCM 编解码器
 * 实现 IMA-ADPCM 4-bit 编解码算法
 * 
 * 特点:
 * - 压缩比: 4:1 (16bit PCM → 4bit ADPCM)
 * - 采样率: 16kHz
 * - 应用场景: 语音数据压缩传输
 * 
 * 数据格式:
 * - 每个字节包含 2 个样本 (低4位 + 高4位)
 * - 帧头包含 predictor 和 stepIndex
 */
class HermesAdpcm {
public:
  /**
   * 编码: PCM → ADPCM
   * @param samples PCM 样本数组 (16bit 有符号)
   * @param sampleCount 样本数量
   * @param output ADPCM 输出缓冲区
   * @param outputSize 输出缓冲区大小
   * @return 实际输出字节数
   */
  static int encode(const int16_t* samples, size_t sampleCount, uint8_t* output, size_t outputSize);
  
  /**
   * 解码: ADPCM → PCM
   * @param input ADPCM 输入缓冲区
   * @param inputBytes 输入字节数
   * @param samples PCM 输出缓冲区
   * @param maxSamples 最大样本数
   * @return 实际输出样本数
   */
  static int decode(const uint8_t* input, size_t inputBytes, int16_t* samples, size_t maxSamples);
  
private:
  // IMA-ADPCM 步长表 (89 个条目)
  static const int16_t stepTable[89];
  
  // 索引调整表 (16 个条目)
  static const int8_t indexTable[16];
};

// ============================================================
// Socket.IO 客户端 (Engine.IO v4)
// ============================================================

/**
 * Socket.IO 客户端类
 * 实现 Engine.IO v4 + Socket.IO 协议
 * 
 * 连接流程:
 * 1. 建立 TCP 连接
 * 2. 发送 HTTP WebSocket 升级请求
 * 3. 等待 101 Switching Protocols 响应
 * 4. 等待 Engine.IO hello 包 (0{...})
 * 5. 发送 namespace connect (40/global-agent)
 * 6. 等待 namespace 确认
 * 7. 连接完成，可以发送事件
 * 
 * 事件格式:
 * - 发送: 42/global-agent,["eventName",{json}]
 * - 接收: 42/global-agent,["eventName",{json}]
 * - 二进制: 451-/global-agent,["voice.stream.chunk",{...}] + binary frame
 */
class HermesSocketIO {
public:
  // 回调函数类型定义
  using EventCallback = std::function<void(const String& event, const String& json)>;
  using RawMessageCallback = std::function<void(const String& message);
  
  HermesSocketIO();
  ~HermesSocketIO();
  
  /**
   * 连接到 Socket.IO 服务器
   * @param url 服务器 URL (如 "http://10.10.168.101:8648")
   * @return true=连接成功, false=连接失败
   */
  bool connect(const String& url);
  
  /**
   * 断开连接
   */
  void disconnect();
  
  /**
   * 检查是否已连接
   */
  bool isConnected() const { return _connected; }
  
  /**
   * 检查 namespace 是否已就绪
   */
  bool isNamespaceReady() const { return _namespaceReady; }
  
  /**
   * 发送事件
   * @param event 事件名称
   * @param json JSON payload
   * @return true=发送成功
   */
  bool sendEvent(const String& event, const String& json);
  
  /**
   * 发送 JSON 消息 (自动提取 type 字段作为事件名)
   * @param json JSON 字符串
   * @return true=发送成功
   */
  bool sendJson(const String& json);
  
  /**
   * 发送 namespace connect 请求
   * @return true=发送成功
   */
  bool sendNamespaceConnect();
  
  /**
   * 发送 ping (Engine.IO 心跳)
   */
  void sendPing() { sendRawText("2"); }
  
  /**
   * 发送 pong (Engine.IO 心跳响应)
   */
  void sendPong() { sendRawText("3"); }
  
  /**
   * 发送二进制数据 (用于语音流)
   * @param data 数据缓冲区
   * @param length 数据长度
   * @return true=发送成功
   */
  bool sendBinary(const uint8_t* data, size_t length);
  
  /**
   * 主循环处理 (必须在 loop() 中调用)
   * 处理接收到的消息、心跳、重连等
   */
  void loop();
  
  // --- 回调函数设置 ---
  void onEvent(EventCallback cb) { _eventCb = cb; }
  void onRawMessage(RawMessageCallback cb) { _rawMessageCb = cb; }
  void onConnected(std::function<void()> cb) { _connectedCb = cb; }
  void onDisconnected(std::function<void()> cb) { _disconnectedCb = cb; }
  
  // --- 连接统计 ---
  uint32_t getConnectTime() const { return _connectTime; }
  uint32_t getLastActivity() const { return _lastActivity; }
  uint8_t getFailureCount() const { return _failureCount; }
  void resetFailureCount() { _failureCount = 0; }
  
  // --- 认证令牌 ---
  void setAuthToken(const String& token) { _authToken = token; }
  const String& getAuthToken() const { return _authToken; }
  
  // --- 命名空间 ---
  void setNamespace(const String& ns) { _namespace = ns; }
  const String& getNamespace() const { return _namespace; }
  
private:
  // 网络客户端
  WiFiClient _plainClient;          // 明文连接
  WiFiClientSecure _secureClient;   // SSL 连接
  WiFiClient* _client;              // 当前使用的客户端
  
  // 连接状态
  bool _connected;                  // 是否已连接
  bool _namespaceReady;             // namespace 是否就绪
  bool _wsUpgraded;                 // WebSocket 是否已升级
  String _authToken;                // 认证令牌
  String _namespace;                // 命名空间 (默认 /global-agent)
  String _host;                     // 主机地址
  uint16_t _port;                   // 端口
  bool _useSSL;                     // 是否使用 SSL
  
  // 连接统计
  uint32_t _connectTime;            // 连接建立时间
  uint32_t _lastActivity;           // 最后活动时间
  uint32_t _lastPing;               // 最后 ping 时间
  uint8_t _failureCount;            // 连续失败次数
  bool _firstData;                  // 是否收到过数据
  
  // 回调函数
  EventCallback _eventCb;
  RawMessageCallback _rawMessageCb;
  std::function<void()> _connectedCb;
  std::function<void()> _disconnectedCb;
  
  // --- WebSocket 帧处理 ---
  bool sendRawWsFrame(uint8_t opcode, const uint8_t* data, size_t length);
  bool sendRawWsText(const String& payload);
  bool sendRawText(const String& payload) { return sendRawWsText(payload); }
  bool writeBytes(const uint8_t* data, size_t length);
  bool readBytes(uint8_t* buffer, size_t length, uint32_t timeoutMs = 100);
  
  // --- Socket.IO 消息解析 ---
  void handleMessage(const String& message);
  void handleEnginePacket(const String& packet);
  void handleSocketIoEvent(const String& message);
  bool parseEvent(const String& message, String* event, String* json);
  
  // --- 连接状态管理 ---
  void markConnected();
  void markDisconnected();
};

// ============================================================
// Hermes Studio 设备 (高级 API)
// ============================================================

/**
 * Hermes Studio 设备类
 * 提供高级 API 用于连接 Hermes Studio 网关
 * 
 * 封装了 WiFi 管理、网关发现、认证、语音交互等功能
 * 使用 hermes-device 协议库作为底层实现
 */
class HermesDevice {
public:
  // 回调函数类型定义
  using InteractionCallback = std::function<void(const String& interactionId, const String& status, const String& text)>;
  using AudioCallback = std::function<void(const String& url, const String& mimeType, uint32_t sampleRate)>;
  using NotificationCallback = std::function<void(const String& title, const String& message)>;
  
  HermesDevice();
  ~HermesDevice();
  
  // --- 初始化 ---
  /**
   * 初始化设备
   * @param deviceName 设备名称
   * @param deviceType 设备类型 (默认 "global_agent")
   * @param namespaceName 命名空间 (默认 "/global-agent")
   */
  void begin(const String& deviceName = "HStudio-Device", 
             const String& deviceType = "global_agent",
             const String& namespaceName = "/global-agent");
  
  // --- WiFi 管理 ---
  bool connectWifi(const String& ssid, const String& pass, bool save = true);
  bool connectSavedWifi();
  void disconnectWifi();
  bool isWifiConnected() const { return _wifiConnected; }
  void startSetupAp(const String& ssid, const String& password = "");
  bool isSetupApActive() const { return _apMode; }
  
  // --- 网关发现 ---
  String discoverGateway();
  bool testGateway(const String& ip);
  void setGatewayUrl(const String& url) { _gatewayUrl = url; }
  const String& getGatewayUrl() const { return _gatewayUrl; }
  
  // --- 认证 ---
  bool login(const String& account, const String& password, const String& profile);
  void logout();
  bool isAuthenticated() const { return _authState == HermesAuthState::Authenticated; }
  HermesAuthState getAuthState() const { return _authState; }
  
  // --- 语音交互 ---
  bool startVoiceInteraction(const String& interactionId);
  bool sendVoiceChunk(const String& interactionId, const uint8_t* data, size_t length, uint32_t offset);
  bool endVoiceInteraction(const String& interactionId, uint32_t totalBytes);
  
  // --- 状态上报 ---
  void reportStatus(const String& interactionId = "", const String& status = "", 
                    bool audioPlaying = false, uint32_t queueLength = 0);
  void reportReady();
  
  // --- 事件回调 ---
  void onInteraction(InteractionCallback cb) { _interactionCb = cb; }
  void onAudio(AudioCallback cb) { _audioCb = cb; }
  void onNotification(NotificationCallback cb) { _notificationCb = cb; }
  void onSessionClear(std::function<void()> cb) { _sessionClearCb = cb; }
  void onAuthRequired(std::function<void()> cb) { _authRequiredCb = cb; }
  
  // --- 通知管理 ---
  void addNotification(const String& title, const String& msg);
  const std::vector<HermesNotification>& getNotifications() const { return _notifications; }
  void clearNotifications();
  
  // --- 闹钟管理 ---
  bool addAlarm(uint8_t hour, uint8_t minute);
  void removeAlarm(uint8_t index);
  void toggleAlarm(uint8_t index, bool enabled);
  const HermesAlarm* getAlarms() const { return _alarms; }
  
  // --- 定时器 ---
  void startTimer(uint32_t seconds);
  void stopTimer();
  bool isTimerRunning() const { return _timerRunning; }
  uint32_t getTimerRemaining() const;
  
  // --- 传感器 ---
  void setSensorData(float temp, float humidity, float batteryV, uint8_t batteryPct);
  const HermesSensorData& getSensorData() const { return _sensors; }
  
  // --- 时间同步 ---
  bool syncTime(const char* ntpServer = "pool.ntp.org", long gmtOffset = 8*3600, int daylightOffset = 0);
  bool isTimeSynced() const { return _timeSynced; }
  
  // --- 主循环 ---
  void loop();
  
  // --- 设备信息 ---
  const String& getDeviceId() const { return _deviceId; }
  const String& getDeviceCode() const { return _deviceCode; }
  const String& getDeviceName() const { return _deviceName; }
  HermesSocketIO& getSocket() { return _socket; }
  
  // --- 配置 ---
  void setSampleRate(uint32_t rate) { _sampleRate = rate; }
  void setRecordMaxMs(uint32_t ms) { _recordMaxMs = ms; }
  void setRecordMinMs(uint32_t ms) { _recordMinMs = ms; }
  void setPort(uint16_t port) { _port = port; }
  
  // --- Web 服务器 (配网用) ---
  void startWebServer(uint16_t port = 80);
  void stopWebServer();
  
private:
  // 设备标识
  String _deviceId;                 // 设备 ID (基于 MAC)
  String _deviceCode;               // 设备代码 (8位十六进制)
  String _deviceName;               // 设备名称
  String _deviceType;               // 设备类型
  String _namespaceName;            // 命名空间
  uint16_t _port;                   // 网关端口
  
  // 网络状态
  bool _wifiConnected;              // WiFi 是否已连接
  bool _apMode;                     // 是否处于 AP 配网模式
  String _savedSsid;                // 保存的 WiFi SSID
  String _savedPass;                // 保存的 WiFi 密码
  String _gatewayUrl;               // 网关 URL
  
  // 认证状态
  HermesAuthState _authState;       // 认证状态
  String _account;                  // 账号
  String _password;                 // 密码
  String _profile;                  // Profile
  
  // Socket.IO
  HermesSocketIO _socket;           // Socket.IO 客户端
  
  // 交互状态
  String _currentInteractionId;     // 当前交互 ID
  HermesInteractionStatus _interactionStatus; // 交互状态
  uint32_t _sampleRate;             // 采样率
  uint32_t _recordMaxMs;            // 最大录音时长
  uint32_t _recordMinMs;            // 最小录音时长
  
  // 通知队列
  std::vector<HermesNotification> _notifications;
  
  // 闹钟数组
  HermesAlarm _alarms[HERMES_MAX_ALARMS];
  
  // 定时器
  uint32_t _timerStartMs;           // 定时器开始时间
  uint32_t _timerDuration;          // 定时器时长
  bool _timerRunning;               // 定时器是否运行
  
  // 传感器数据
  HermesSensorData _sensors;
  
  // 时间同步
  bool _timeSynced;                 // 时间是否已同步
  time_t _lastNtpSync;              // 最后 NTP 同步时间
  
  // 回调函数
  InteractionCallback _interactionCb;
  AudioCallback _audioCb;
  NotificationCallback _notificationCb;
  std::function<void()> _sessionClearCb;
  std::function<void()> _authRequiredCb;
  
  // 持久化存储
  Preferences _prefs;
  
  // 内部方法
  void generateDeviceId();          // 生成设备 ID
  void loadPreferences();           // 加载保存的配置
  void saveCredentials();           // 保存认证信息
  void clearCredentials();          // 清除认证信息
  void handleSocketEvent(const String& event, const String& json);
  void handleMcuAuth(const String& json);
  void handleInteractionStatus(const String& json);
  void handleAudioEnqueue(const String& json);
  void handleSessionClear();
  void handleAuthInvalid();
  void handleReauthRequired();
  void checkAlarms();               // 检查闹钟
  void checkTimer();                // 检查定时器
  void reconnectSocket();           // 重连 Socket
  
  // Web 服务器
  void* _webServer;
  bool _webServerActive;
};

#endif // _HERMES_DEVICE_H_
