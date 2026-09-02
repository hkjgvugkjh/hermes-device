# hermes-device — Hermes Studio 设备协议库

## 📋 项目概述

**hermes-device** 是一个可复用的 C++ 库，用于将 ESP32 设备通过 Socket.IO (Engine.IO v4) 协议连接到 [Hermes Studio](https://github.com/EKKOLearnAI/hermes-studio) 网关。

支持语音对话、音频播放、设备管理、传感器数据采集等功能。

### 特性

- ✅ Socket.IO 客户端 (WebSocket 传输)
- ✅ MCU 认证流程
- ✅ 语音流 (IMA-ADPCM 4-bit 编解码)
- ✅ 事件驱动架构
- ✅ 网关发现 (mDNS + 网络扫描)
- ✅ NTP 时间同步
- ✅ 闹钟/定时器管理
- ✅ 通知队列
- ✅ 传感器数据管理
- ✅ 连接失败检测与自动重连

---

## 📦 安装

### Arduino IDE

1. 下载 [hermes-device-v1.0.0.zip](https://github.com/hkjgvugkjh/hermes-device/releases/tag/v1.0.0)
2. 打开 IDE → 项目 → 添加库 → 选择 zip 文件

### PlatformIO

```ini
lib_deps =
    https://github.com/hkjgvugkjh/hermes-device.git#v1.0.0
```

### 手动安装

```bash
cd ~/Arduino/libraries/
git clone https://github.com/hkjgvugkjh/hermes-device.git
```

---

## 🚀 快速开始

```cpp
#include <hermes-device.h>

HermesDevice device;

void setup() {
  Serial.begin(115200);
  
  // 初始化设备
  device.begin("HStudio-Device", "global_agent");
  
  // 连接 WiFi
  device.connectWifi("YourSSID", "YourPassword");
  
  // 设置事件回调
  device.onInteraction([](const String& id, const String& status, const String& text) {
    Serial.printf("Interaction %s: %s\n", id.c_str(), status.c_str());
  });
}

void loop() {
  device.loop();  // 必须调用
}
```

---

## 📖 API 参考

### 初始化

```cpp
// 初始化设备
void begin(const String& deviceName = "HStudio-Device", 
           const String& deviceType = "global_agent",
           const String& namespaceName = "/global-agent");

// 配置参数
void setSampleRate(uint32_t rate);       // 默认 16000
void setRecordMaxMs(uint32_t ms);        // 默认 15000
void setRecordMinMs(uint32_t ms);        // 默认 500
void setPort(uint16_t port);             // 默认 8648
```

### WiFi 管理

```cpp
// 连接 WiFi (自动保存)
bool connectWifi(const String& ssid, const String& pass, bool save = true);

// 连接上次保存的 WiFi
bool connectSavedWifi();

// 断开 WiFi
void disconnectWifi();

// 检查连接状态
bool isWifiConnected() const;

// 启动 AP 配网模式
void startSetupAp(const String& ssid, const String& password = "");
bool isSetupApActive() const;
```

### 网关发现

```cpp
// 自动发现网关 (mDNS → 网络扫描 → 保存的 URL)
String discoverGateway();

// 测试指定 IP 是否为 Hermes Studio 网关
bool testGateway(const String& ip);

// 手动设置网关 URL
void setGatewayUrl(const String& url);
const String& getGatewayUrl() const;
```

### 认证

```cpp
// 登录 (account + password + profile)
bool login(const String& account, const String& password, const String& profile);

// 登出
void logout();

// 检查认证状态
bool isAuthenticated() const;
HermesAuthState getAuthState() const;
```

### 语音交互

```cpp
// 开始语音交互
bool startVoiceInteraction(const String& interactionId);

// 发送语音数据块 (ADPCM 编码后)
bool sendVoiceChunk(const String& interactionId, const uint8_t* data, size_t length, uint32_t offset);

// 结束语音交互
bool endVoiceInteraction(const String& interactionId, uint32_t totalBytes);
```

### 状态上报

```cpp
// 上报交互状态
void reportStatus(const String& interactionId = "", const String& status = "", 
                  bool audioPlaying = false, uint32_t queueLength = 0);

// 上报设备就绪
void reportReady();
```

### 闹钟管理

```cpp
// 添加闹钟
bool addAlarm(uint8_t hour, uint8_t minute);

// 删除闹钟
void removeAlarm(uint8_t index);

// 启用/禁用闹钟
void toggleAlarm(uint8_t index, bool enabled);

// 获取闹钟列表
const HermesAlarm* getAlarms() const;
```

### 定时器

```cpp
// 启动定时器 (秒)
void startTimer(uint32_t seconds);

// 停止定时器
void stopTimer();

// 检查运行状态
bool isTimerRunning() const;

// 获取剩余时间 (秒)
uint32_t getTimerRemaining() const;
```

### 传感器

```cpp
// 设置传感器数据
void setSensorData(float temp, float humidity, float batteryV, uint8_t batteryPct);

// 获取传感器数据
const HermesSensorData& getSensorData() const;
```

### 时间同步

```cpp
// NTP 时间同步
bool syncTime(const char* ntpServer = "pool.ntp.org", 
              long gmtOffset = 8*3600, 
              int daylightOffset = 0);

// 检查同步状态
bool isTimeSynced() const;
```

### 通知

```cpp
// 添加通知
void addNotification(const String& title, const String& msg);

// 获取通知列表
const std::vector<HermesNotification>& getNotifications() const;

// 清除所有通知
void clearNotifications();
```

### 事件回调

```cpp
// 交互状态回调
void onInteraction(InteractionCallback cb);

// 音频播放回调
void onAudio(AudioCallback cb);

// 通知回调
void onNotification(NotificationCallback cb);

// 会话清除回调
void onSessionClear(std::function<void()> cb);

// 需要重新认证回调
void onAuthRequired(std::function<void()> cb);
```

### 设备信息

```cpp
const String& getDeviceId() const;      // 设备 ID
const String& getDeviceCode() const;    // 设备代码 (8位十六进制)
const String& getDeviceName() const;    // 设备名称
HermesSocketIO& getSocket();            // 直接访问 Socket.IO 客户端
```

---

## 🔌 硬件要求

| 最低要求 | 推荐 |
|----------|------|
| ESP32 (240MHz, 320KB SRAM) | ESP32-S3 (240MHz, 512KB SRAM) |
| 4MB Flash | 8MB Flash |
| 支持 WiFi 802.11 b/g/n | 支持 WiFi 802.11 b/g/n |

### 依赖库

- `WiFi` — 内置
- `WiFiClientSecure` — 内置
- `HTTPClient` — 内置
- `Preferences` — 内置
- `ESPmDNS` — 内置
- `time` — 内置

---

## 📐 数据结构与枚举

### HermesAuthState

```cpp
enum class HermesAuthState {
  Disconnected,    // 未连接
  Connecting,      // 连接中
  Authenticating,  // 认证中
  Authenticated,   // 已认证
  Failed           // 认证失败
};
```

### HermesInteractionStatus

```cpp
enum class HermesInteractionStatus {
  Idle,      // 空闲
  Listening, // 录音中
  Thinking,  // 思考中
  Playing,   // 播放中
  Error      // 错误
};
```

### HermesSensorData

```cpp
struct HermesSensorData {
  float temperature;      // 温度 (°C)
  float humidity;         // 湿度 (%)
  float batteryVoltage;   // 电池电压 (V)
  uint8_t batteryPercent; // 电池百分比 (%)
  uint32_t lastUpdate;    // 最后更新时间
};
```

---

## 🔧 配置常量

| 常量 | 默认值 | 说明 |
|------|--------|------|
| `HERMES_DEFAULT_SAMPLE_RATE` | 16000 | 采样率 (Hz) |
| `HERMES_DEFAULT_RECORD_MAX_MS` | 15000 | 最大录音时长 (ms) |
| `HERMES_DEFAULT_RECORD_MIN_MS` | 500 | 最小录音时长 (ms) |
| `HERMES_DEFAULT_VAD_THRESHOLD` | 300 | 语音活动检测阈值 |
| `HERMES_MAX_WS_FRAME_SIZE` | 8192 | WebSocket 帧最大尺寸 |
| `HERMES_MAX_NOTIFICATIONS` | 10 | 最大通知数量 |
| `HERMES_MAX_ALARMS` | 5 | 最大闹钟数量 |
| `HERMES_SOCKET_RECONNECT_MS` | 5000 | Socket 重连间隔 (ms) |
| `HERMES_WIFI_CONNECT_TIMEOUT_MS` | 15000 | WiFi 连接超时 (ms) |
| `HERMES_NTP_SYNC_INTERVAL` | 3600 | NTP 同步间隔 (秒) |
| `HERMES_GATEWAY_PORT` | 8648 | 默认网关端口 |

---

## 📦 发布历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.0.0 | 2026-08-28 | 初始版本 |

---

## 📄 许可证

MIT License

---

## 🔗 相关链接

- [hermes-t5 — 墨水屏版本](https://github.com/hkjgvugkjh/hermes-t5)
- [hermes-xrs — 彩屏版本](https://github.com/hkjgvugkjh/hermes-xrs)
- [hermes-box — 完整项目](https://github.com/hkjgvugkjh/hermes-box)
- [Hermes Studio 官方](https://github.com/EKKOLearnAI/hermes-studio)
