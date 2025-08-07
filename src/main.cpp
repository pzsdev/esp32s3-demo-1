#include <Arduino.h>
//esp32s3 连接 dh11、光线传感器、oled显示屏，并把数据传输到 InfluxDB
#include <Wire.h>
#include <U8g2lib.h>
#include <TFT_eSPI.h> // ST7789驱动库
#include <DHT.h>
#include <WiFiMulti.h>
#include <BH1750.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

// =============== 硬件配置 ===============
#define DHTPIN 6
#define DHTTYPE DHT11

#define PIR_PIN 1          // HC-SR312连接引脚
#define ENABLE_PIR 1       // 1-启用PIR传感器 0-禁用
#define DISPLAY_TYPE 1     // 0-OLED 1-ST7789

// ST7789引脚配置
#define TFT_MOSI 11
#define TFT_SCLK 12 
#define TFT_CS   8
#define TFT_DC   9
#define TFT_RST  10
#define TFT_BL   13  // 背光控制

#define DEVICE "ESP32S3"

#define WIFI_SSID "HUAWEI-10HA9N-2.4"
#define WIFI_PASSWORD "wf20210612"

#define INFLUXDB_URL "http://192.168.100.234:8086"
#define INFLUXDB_TOKEN "eO0OXFtfIc4cL4z0NrEQQDmzQqsL1SUXbFGDVHmDATzkuhkZuhYQRjNm020QgOUuBQbW2RHavB4U8voHZMOsFQ=="
#define INFLUXDB_ORG "pengchengcoltd"
#define INFLUXDB_BUCKET "esp32"

#define TZ_INFO "CST-8"
#define PIR_WARMUP_TIME 60000  // HC-SR312需要60秒预热

// =============== 全局对象 ===============
WiFiMulti wifiMulti;

DHT dht(DHTPIN, DHTTYPE);

TwoWire I2CBH1750 = TwoWire(1);
BH1750 lightMeter;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 4, 5);
TFT_eSPI tft = TFT_eSPI(); // ST7789屏幕对象

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
// Point sensor("roomSensor");

#define DISPLAY_BUF_SIZE 32
char displayBuffer[DISPLAY_BUF_SIZE];

// =============== PIR状态机 ===============
enum PIRState {
    PIR_UNINITIALIZED, //0
    PIR_WARMING_UP, // 1
    PIR_READY, // 2
    PIR_ERROR // 3
};

PIRState pirStatus = PIR_UNINITIALIZED;
unsigned long pirWarmupStartTime = 0;
bool currentPirState = false;
bool lastPirState = false;
uint16_t motionCount = 0;

// =============== 数据结构 ===============
struct SensorData {
    float temperature;
    float humidity;
    float light;

    bool dhtValid;
    bool bh1750Valid;

    bool motionDetected;
    uint16_t motionCount;
    String pirStatus;

    String currentTime;
};

// =============== 函数声明 ===============
void initPIRSensorAsync();
void updatePIRStatus();
SensorData readSensorData();
void uploadToInfluxDB(const SensorData& data);
void displayAllData(const SensorData& data);
void displayOnOLED(const SensorData& data);
void printToSerial(const SensorData& data);
String getCurrentTime();
void logWithTimestamp(const String& message);

// =============== 初始化 ===============
void setup() {
    Serial.begin(115200);
    logWithTimestamp("系统启动");

    // 显示设备初始化
#if DISPLAY_TYPE == 0
    // OLED初始化
    Wire.begin(5, 4); // SDA=5, SCL=4
    oled.begin();
    oled.clearBuffer();
    oled.setFont(u8g2_font_7x14B_tr);
    oled.drawStr(0, 15, "System Init...");
    oled.sendBuffer();
#else
    // ST7789初始化
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(0, 0);
    tft.println("System Init...");
#endif

    // 传感器初始化
    dht.begin();
    I2CBH1750.begin(15, 16); // 第二个I2C总线
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2CBH1750);
    pinMode(PIR_PIN, INPUT_PULLUP);  // 启用内部上拉电阻
    initPIRSensorAsync(); // 非阻塞启动PIR

    // WiFi连接
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
    logWithTimestamp("连接WiFi...");
    while (wifiMulti.run() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    logWithTimestamp("WiFi连接成功: " + WiFi.localIP().toString());

    // InfluxDB配置
    // sensor.addTag("device", DEVICE);
    // sensor.addTag("SSID", WIFI_SSID);
    timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

    if (client.validateConnection()) {
        logWithTimestamp("InfluxDB连接成功");
    } else {
        logWithTimestamp("InfluxDB连接失败: " + String(client.getLastErrorMessage()));
    }
}

// =============== 主循环 ===============
void loop() {
    updatePIRStatus(); // 必须首先调用

    static unsigned long lastReadTime = 0;
    if (millis() - lastReadTime >= 2000) { // 2秒采样周期
        SensorData data = readSensorData();
        
    // 统一处理数据上传和显示
    uploadToInfluxDB(data);
    displayAllData(data);
        
        lastReadTime = millis();
    }
    delay(100); // 主循环延迟
}

// =============== PIR传感器异步控制 ===============
void initPIRSensorAsync() {
    if (pirStatus == PIR_UNINITIALIZED) {
#if ENABLE_PIR
        pinMode(PIR_PIN, INPUT);
        pirWarmupStartTime = millis();
        pirStatus = PIR_WARMING_UP;
        logWithTimestamp("HC-SR312开始异步预热");
#else
        pirStatus = PIR_ERROR;
        logWithTimestamp("PIR传感器已禁用");
#endif
    }
}

void updatePIRStatus() {
#if ENABLE_PIR
    // 仅在启用时输出详细状态日志
    static PIRState lastStatus = PIR_UNINITIALIZED;
    if (pirStatus != lastStatus) {
        logWithTimestamp("HC-SR312状态变更: " + String(pirStatus));
        lastStatus = pirStatus;
    }
#else
    // 禁用时只输出一次日志
    static bool firstRun = true;
    if (firstRun) {
        logWithTimestamp("PIR传感器已禁用");
        firstRun = false;
    }
    return;
#endif

    switch (pirStatus) {
        case PIR_WARMING_UP:
            if (millis() - pirWarmupStartTime > PIR_WARMUP_TIME) {
                pirStatus = PIR_READY;
                logWithTimestamp("HC-SR312预热完成");
                
                // 检查初始状态
                if (digitalRead(PIR_PIN)) {
                    logWithTimestamp("警告：PIR初始状态为触发");
                }
                
                logWithTimestamp("PIR 预热完成，当前状态: " + String(digitalRead(PIR_PIN))); // 打印初始电平
            }
            break;
            
        case PIR_READY: {
            // 状态读取与滤波
            static uint32_t lastPirReadTime = 0;
            static uint8_t stableCount = 0;
            
        
            if (millis() - lastPirReadTime > 100) { // 100ms采样间隔
                bool rawState = digitalRead(PIR_PIN);
                
#if ENABLE_PIR
                // 仅在启用时输出调试信息
                static uint32_t lastDebugTime = 0;
                if (millis() - lastDebugTime > 5000) { // 每5秒输出一次调试信息
                    logWithTimestamp("[DEBUG] PIR RAW: " + String(rawState));
                    logWithTimestamp("[DEBUG] GPIO1 Voltage: " + String(analogRead(PIR_PIN)));
                    lastDebugTime = millis();
                }
#endif

                if (rawState != currentPirState) {
                    if (++stableCount > 3) { // 连续3次确认
                        currentPirState = rawState;
                        stableCount = 0;
                        
                        if (currentPirState) {
                            motionCount++;
                            logWithTimestamp("运动检测触发");
                        }
                    }
                } else {
                    stableCount = 0;
                }
                
                lastPirReadTime = millis();
            }
            break;
        }
    }
}

// =============== 数据采集 ===============
SensorData readSensorData() {
    SensorData data;
    data.currentTime = getCurrentTime();
    
    // DHT11读取
    data.humidity = dht.readHumidity();
    data.temperature = dht.readTemperature();
    data.dhtValid = !(isnan(data.humidity) || isnan(data.temperature));
    
    // BH1750读取
    data.light = lightMeter.readLightLevel();
    data.bh1750Valid = (data.light >= 0);
    if (!data.bh1750Valid) data.light = 0;
    
    // PIR状态
    switch (pirStatus) {
        case PIR_UNINITIALIZED: data.pirStatus = "uninitialized"; break;
        case PIR_WARMING_UP: {
            int remain = (PIR_WARMUP_TIME - (millis() - pirWarmupStartTime)) / 1000;
            data.pirStatus = "warming_up_" + String(remain) + "s";
            break;
        }
        case PIR_READY: 
            data.pirStatus = "ready";
            data.motionDetected = currentPirState;
            data.motionCount = motionCount;
            break;
        case PIR_ERROR: data.pirStatus = "disabled"; break;
    }
    
    return data;
}

// =============== 数据上传 ===============
void uploadToInfluxDB(const SensorData& data) {
    // 1. 检查网络和服务
    checkNetwork();
    if (!checkInfluxServer()) {
        
        logWithTimestamp("InfluxDB 服务不可达");
        return;
    }

    static String lastPirStatus = "";
    static unsigned long lastUploadTime = 0;
    
    // === 创建新Point对象保留固定标签 ===
    Point newPoint("roomSensor");
    newPoint.addTag("device", DEVICE);
    newPoint.addTag("SSID", WIFI_SSID);
    
    // === 动态标签 ===
    if(data.pirStatus != lastPirStatus) {
        newPoint.addTag("pir_status", data.pirStatus.c_str());
        lastPirStatus = data.pirStatus;
    }

    // === 添加字段 ===
    if(data.dhtValid) {
        newPoint.addField("temperature", data.temperature);
        newPoint.addField("humidity", data.humidity);
    }
    
    if(data.bh1750Valid) {
        newPoint.addField("light", data.light);
    }

#if ENABLE_PIR
    if(pirStatus == PIR_READY) {
        newPoint.addField("motion", data.motionDetected ? 1 : 0);
        newPoint.addField("motion_count", data.motionCount);
        
        static bool lastMotionState = false;
        if(data.motionDetected && !lastMotionState) {
            newPoint.addField("motion_event", 1);
        }
        lastMotionState = data.motionDetected;
    }
#endif

    // === 上传 ===
    if(millis() - lastUploadTime > 2000 || 
       (pirStatus == PIR_READY && data.motionDetected)) {
        
       logWithTimestamp("[DEBUG] 准备上传的数据: " + newPoint.toLineProtocol());
        
        if(client.writePoint(newPoint)) {
            lastUploadTime = millis();
            logWithTimestamp("数据上传成功");
        } else {
            logWithTimestamp("上传失败: ");
            logWithTimestamp("错误代码: " + String(client.getLastStatusCode()));
            logWithTimestamp("错误信息: " + String(client.getLastErrorMessage()));
        }
    }
}

void checkNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi断开，尝试重连...");
    WiFi.disconnect();
    WiFi.reconnect();
    delay(2000); // 等待重连
  }
}

bool checkInfluxServer() {
  HTTPClient http;
  http.begin(INFLUXDB_URL);
  int code = http.GET();
  http.end();
  return (code == 200 || code == 204); // InfluxDB健康检查通常返回204
}


// =============== 数据显示 ===============
void displayAllData(const SensorData& data) {
#if DISPLAY_TYPE == 0
    displayOnOLED(data);
#else
    displayOnST7789(data);
#endif
    printToSerial(data);
}

void displayOnST7789(const SensorData& data) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    
    // 第一行：标题 + 时间
    tft.setCursor(0, 0);
    tft.print("Room Monitor ");
    tft.setTextSize(1);
    tft.print(data.currentTime.c_str());
    
    // 第二行：温度
    tft.setTextSize(2);
    tft.setCursor(0, 20);
    tft.print("Temp: ");
    if(data.dhtValid) {
        tft.print(data.temperature, 1);
        tft.print("C");
    } else {
        tft.print("--.-C");
    }
    
    // 第三行：湿度
    tft.setCursor(0, 40);
    tft.print("Humid: ");
    if(data.dhtValid) {
        tft.print(data.humidity, 0);
        tft.print("%");
    } else {
        tft.print("--%");
    }
    
    // 第四行：光照
    tft.setCursor(0, 60);
    tft.print("Light: ");
    if(data.bh1750Valid) {
        tft.print((int)data.light);
        tft.print("lx");
    } else {
        tft.print("---lx");
    }
    
    // 状态栏
    tft.setTextSize(1);
    tft.setCursor(0, 220);
    tft.print("WiFi:");
    tft.print((WiFi.status() == WL_CONNECTED) ? "OK" : "OFF");
    tft.print("(");
    tft.print(WiFi.RSSI());
    tft.print("dBm)");
    
    tft.setCursor(180, 220);
    tft.print("PIR:");
    if(pirStatus == PIR_WARMING_UP) {
        int remainSec = (PIR_WARMUP_TIME - (millis() - pirWarmupStartTime)) / 1000;
        tft.printf("%02ds", remainSec);
    } else if(pirStatus == PIR_READY) {
        tft.print(data.motionDetected ? "ACT" : "---");
    } else {
        tft.print("DIS");
    }
}

void displayOnOLED(const SensorData& data) {
    // 使用静态缓冲区避免重复分配
    static char displayBuffer[32];
    
    oled.clearBuffer();
    
    // ===== 第一行：标题 + 时间 =====
    oled.setFont(u8g2_font_7x14B_tr);
    oled.drawStr(0, 15, "Room Monitor");
    
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(90, 12, data.currentTime.c_str());
    
    // ===== 第二行：核心数据（温湿度+光照）=====
    oled.setFont(u8g2_font_7x14_tr); // 统一使用中等大小字体
    
    // 温度显示（左对齐）
    if(data.dhtValid) {
        snprintf(displayBuffer, sizeof(displayBuffer), "%.1fC", data.temperature);
    } else {
        strncpy(displayBuffer, "--.-C", sizeof(displayBuffer));
    }
    oled.drawStr(5, 35, displayBuffer);
    
    // 湿度显示（中间偏左）
    if(data.dhtValid) {
        snprintf(displayBuffer, sizeof(displayBuffer), "%.0f%%", data.humidity);
    } else {
        strncpy(displayBuffer, "--%%", sizeof(displayBuffer)); // 注意双%转义
    }
    oled.drawStr(50, 35, displayBuffer);
    
    // 光照显示（右对齐）
    if(data.bh1750Valid) {
        snprintf(displayBuffer, sizeof(displayBuffer), "%dlx", (int)data.light);
    } else {
        strncpy(displayBuffer, "---lx", sizeof(displayBuffer));
    }
    oled.drawStr(90, 35, displayBuffer);
    
    // ===== 分隔线 =====
    oled.setDrawColor(1);
    oled.drawHLine(0, 40, 128); // 上移分隔线
    
    // ===== 第三行：数据标签 ===== 
    oled.setFont(u8g2_font_5x8_tr); // 小字体标签
    oled.drawStr(5, 50, "Temp");
    oled.drawStr(50, 50, "Humid");
    oled.drawStr(90, 50, "Light");
    
    // ===== 状态栏 =====
    oled.drawHLine(0, 54, 128); // 状态栏分隔线
    
    // WiFi状态（左对齐）
    snprintf(displayBuffer, sizeof(displayBuffer), "WiFi:%s(%ddBm)", 
        (WiFi.status() == WL_CONNECTED) ? "OK" : "OFF",
        WiFi.RSSI());
    oled.drawStr(2, 63, displayBuffer);
    
    // PIR状态（右对齐）
    if(pirStatus == PIR_WARMING_UP) {
        int remainSec = (PIR_WARMUP_TIME - (millis() - pirWarmupStartTime)) / 1000;
        snprintf(displayBuffer, sizeof(displayBuffer), "PIR:%02ds", remainSec); // 两位数显示
    } else if(pirStatus == PIR_READY) {
        snprintf(displayBuffer, sizeof(displayBuffer), "PIR:%s", 
            data.motionDetected ? "ACT" : "---");
    } else {
        snprintf(displayBuffer, sizeof(displayBuffer), "PIR:DIS");
    }
    oled.drawStr(90, 63, displayBuffer);
    
    oled.sendBuffer();
}



void printToSerial(const SensorData& data) {
    String logMsg = "数据 => ";
    if (data.dhtValid) {
        logMsg += "温度:" + String(data.temperature) + "C 湿度:" + String(data.humidity) + "% ";
    } else {
        logMsg += "DHT11无效 ";
    }
    
    logMsg += "光照:" + (data.bh1750Valid ? String(data.light) + "lx " : "N/A ");
    
    if (pirStatus == PIR_READY) {
        logMsg += "运动:" + String(data.motionDetected ? "是" : "否") + 
                 " 计数:" + String(data.motionCount);
    } else {
        logMsg += "PIR:" + data.pirStatus;
    }
    
    logWithTimestamp(logMsg);
    Serial.println();
}

// =============== 工具函数 ===============
String getCurrentTime() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return "NTP Error";
    }
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    return String(timeStr);
}

// 带时间戳的日志函数
void logWithTimestamp(const String& message) {
    static bool timeSynced = false;
    static unsigned long startMillis = millis();
    
    Serial.print("[");
    if(timeSynced) {
        Serial.print(getCurrentTime());
    } else {
        // 在时间同步前使用系统运行时间(毫秒)
        Serial.print(millis());
        Serial.print("ms");
        
        // 检查是否已完成时间同步
        struct tm timeinfo;
        if(getLocalTime(&timeinfo, 0)) {
            timeSynced = true;
        }
    }
    Serial.print("] ");
    Serial.println(message);
}

void safeDrawStr(u8g2_uint_t x, u8g2_uint_t y, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(displayBuffer, DISPLAY_BUF_SIZE, format, args);
    va_end(args);
    oled.drawStr(x, y, displayBuffer);
}
