#include <Arduino.h>
//esp32s3 连接 dh11、光线传感器、oled显示屏，并把数据传输到 InfluxDB
#include <Wire.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <WiFiMulti.h>
#include <BH1750.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

// 定义传感器数据结构体（放在文件顶部）
struct SensorData {
    float temperature;
    float humidity;
    float light;
    bool dhtValid;
    bool bh1750Valid;
    String currentTime;
};

// 其他全局变量和初始化代码...
WiFiMulti wifiMulti;
#define DEVICE "ESP32s3"

#define WIFI_SSID "HUAWEI-10HA9N-2.4"
#define WIFI_PASSWORD "wf20210612"
#define INFLUXDB_URL "http://192.168.100.234:8086"
#define INFLUXDB_TOKEN "eO0OXFtfIc4cL4z0NrEQQDmzQqsL1SUXbFGDVHmDATzkuhkZuhYQRjNm020QgOUuBQbW2RHavB4U8voHZMOsFQ=="
#define INFLUXDB_ORG "pengchengcoltd"
#define INFLUXDB_BUCKET "esp32"
#define TZ_INFO "CST-8"

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point sensor("roomSensor");

TwoWire I2CBH1750 = TwoWire(1);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 4, 5);
#define DHTPIN 6
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;

// 前置声明函数
SensorData readSensorData();
void uploadToInfluxDB(const SensorData& data);
void displayAllData(const SensorData& data);
void displayOnOLED(const SensorData& data);
void printToSerial(const SensorData& data);
String getCurrentTime();
void logWithTimestamp(const String& message);

void setup() {
  Serial.begin(115200);
  logWithTimestamp("系统启动");

  // 初始化第一个 I2C 总线 (Wire: SDA=5, SCL=4) - OLED
  Wire.begin(5, 4);
  logWithTimestamp("正在初始化OLED显示屏...");
  
  // 扫描I2C设备
  logWithTimestamp("扫描I2C总线上的设备...");
  byte error, address;
  int nDevices = 0;
  for(address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      logWithTimestamp("发现I2C设备，地址: 0x" + String(address < 16 ? "0" : "") + String(address, HEX));
      nDevices++;
    }
  }
  if (nDevices == 0) {
    logWithTimestamp("未发现任何I2C设备，请检查连接");
  } else {
    logWithTimestamp("I2C扫描完成");
  }

  logWithTimestamp("oled开始初始化...");
  oled.begin();
  logWithTimestamp("oled初始化结束");
  if(oled.getDisplayWidth() > 0 && oled.getDisplayHeight() > 0) {
    logWithTimestamp("OLED初始化成功");
    logWithTimestamp("显示屏分辨率: " + String(oled.getDisplayWidth()) + "x" + String(oled.getDisplayHeight()));
  } else {
    logWithTimestamp("OLED初始化失败，请检查连接和地址");
  }

  // 初始化第二个 I2C 总线 (I2CBH1750: SDA=15, SCL=16) - BH1750
  I2CBH1750.begin(15, 16);  // SDA=15, SCL=16

  // 检查第二个I2C总线上的设备
  logWithTimestamp("扫描第二个I2C总线上的设备...");
  nDevices = 0;
  for(address = 1; address < 127; address++ ) {
    I2CBH1750.beginTransmission(address);
    error = I2CBH1750.endTransmission();
    if (error == 0) {
      logWithTimestamp("发现I2C设备，地址: 0x" + String(address < 16 ? "0" : "") + String(address, HEX) + " !");
      nDevices++;
    }
  }
  if (nDevices == 0) {
    logWithTimestamp("未发现任何I2C设备");
  }

  // 尝试两种可能的 BH1750 地址
  if(!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2CBH1750)) {
    logWithTimestamp("在地址0x23初始化BH1750失败，尝试0x5C");
    if(!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &I2CBH1750)) {
      logWithTimestamp("无法找到有效的BH1750传感器，请检查接线!");
    } else {
      logWithTimestamp("BH1750在地址0x5C初始化成功");
    }
  } else {
    logWithTimestamp("BH1750在地址0x23初始化成功");
  }

  dht.begin();          // 启动DHT11

  // 显示初始界面
  logWithTimestamp("尝试显示初始界面...");
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tr);
  oled.drawStr(0, 15, "DHT11 Monitor");
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 35, "Initializing...");
  oled.sendBuffer();
  // 通过检查I2C错误来判断显示是否成功
  if(Wire.getWriteError()) {
    logWithTimestamp("OLED显示失败 - I2C通信错误");
  } else {
    logWithTimestamp("OLED显示命令已发送");
  }
  delay(1000);

  WiFi.mode(WIFI_STA);                                              //设置 WiFi 连接
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  logWithTimestamp("正在连接到 WiFi...");                          //连接到 WiFi
  unsigned long lastLogTime = 0;
  int dotCount = 0;
  
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(100);
    
    // 每5秒输出一次连接状态
    if(millis() - lastLogTime > 5000) {
      String dots;
      for(int i=0; i<dotCount; i++) dots += ".";
      logWithTimestamp("连接中" + dots);
      
      dotCount = (dotCount + 1) % 4;
      lastLogTime = millis();
    }
  }

  logWithTimestamp("WiFi连接成功");
  logWithTimestamp("IP地址: " + WiFi.localIP().toString());

  sensor.addTag("device", DEVICE);                                   //添加标签 - 根据需要重复
  sensor.addTag("SSID", WIFI_SSID);

  logWithTimestamp("开始NTP时间同步...");
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");                 //准确时间对于证书验证和批量写入是必要的

  logWithTimestamp("检查InfluxDB连接...");
  if (client.validateConnection())                                   //检查服务器连接
  {
    logWithTimestamp("已连接到InfluxDB: " + String(client.getServerUrl()));
  } 
  else 
  {
    logWithTimestamp("InfluxDB连接失败: " + String(client.getLastErrorMessage()));
  }

}

void loop() {
    delay(2000); // 2秒采样间隔

    // 1. 读取传感器数据
    SensorData data = readSensorData();
    
    // 2. 上传数据到InfluxDB
    uploadToInfluxDB(data);
    
    // 3. 显示数据到OLED和串口
    displayAllData(data);
}

// 读取所有传感器数据
SensorData readSensorData() {
    SensorData data;
    
    // 获取当前时间
    data.currentTime = getCurrentTime();
    
    // 读取DHT11数据
    data.humidity = dht.readHumidity();
    data.temperature = dht.readTemperature();
    data.dhtValid = !(isnan(data.humidity) || isnan(data.temperature));
    
    // 读取BH1750数据
    data.light = lightMeter.readLightLevel();
    data.bh1750Valid = (data.light >= 0);
    if (!data.bh1750Valid) {
        data.light = 0; // 默认值
    }
    
    return data;
}

// 上传数据到InfluxDB
void uploadToInfluxDB(const SensorData& data) {
    if (!data.dhtValid && !data.bh1750Valid) {
        logWithTimestamp("无有效传感器数据，跳过上传");
        return; // 无有效数据不上传
    }

    sensor.clearFields();
    
    if (data.dhtValid) {
        sensor.addField("temperature", data.temperature);
        sensor.addField("humidity", data.humidity);
    }
    
    if (data.bh1750Valid) {
        sensor.addField("light", data.light);
    }

    // 检查WiFi连接
    if (wifiMulti.run() != WL_CONNECTED) {
        logWithTimestamp("WiFi连接丢失");
        return;
    }

    // 写入数据
    logWithTimestamp("正在上传数据到InfluxDB...");
    if (!client.writePoint(sensor)) {
        logWithTimestamp("InfluxDB写入失败: " + String(client.getLastErrorMessage()));
    } else {
        logWithTimestamp("数据上传成功");
    }
}

// 显示所有数据
void displayAllData(const SensorData& data) {
    // OLED显示
    displayOnOLED(data);
    
    // 串口打印
    printToSerial(data);
}

// Optimized OLED display for 128x64 (English only)
void displayOnOLED(const SensorData& data) {
    oled.clearBuffer();
    
    // Line 1: Title (left) + Full time (right)
    oled.setFont(u8g2_font_7x14B_tr);
    oled.drawStr(0, 16, "Room Monitor");
    
    oled.setFont(u8g2_font_5x8_tr);
    int timeWidth = oled.getStrWidth(data.currentTime.c_str());
    oled.drawStr(128 - timeWidth, 12, data.currentTime.c_str());
    
    // Line 2-3: Big temperature and humidity
    oled.setFont(u8g2_font_10x20_tr);
    
    // Temperature (left)
    if(data.dhtValid) {
        String tempStr = String(data.temperature, 1) + "C";
        oled.drawStr(10, 34, tempStr.c_str());
    } else {
        oled.drawStr(10, 34, "--.-C");
    }
    
    // Humidity (right)
    if(data.dhtValid) {
        String humStr = String(data.humidity, 0) + "%";
        int humWidth = oled.getStrWidth(humStr.c_str());
        oled.drawStr(128 - humWidth - 10, 34, humStr.c_str());
    } else {
        oled.drawStr(100, 34, "--%");
    }
    
    // Line 4: Light sensor data
    oled.setFont(u8g2_font_7x14_tr);
    oled.drawStr(10, 50, "Light:");
    
    if(data.bh1750Valid) {
        String lightStr = String(data.light, 0) + " lx";
        oled.drawStr(65, 50, lightStr.c_str());
    } else {
        oled.drawStr(65, 50, "--- lx");
    }
    
    // Status bar (bottom line)
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawHLine(0, 54, 128);
    
    // WiFi status
    String wifiStatus = (wifiMulti.run() == WL_CONNECTED) ? "WiFi:OK" : "WiFi:FAIL";
    oled.drawStr(5, 63, wifiStatus.c_str());
    
    // DB status
    String dbStatus = client.isBufferEmpty() ? "DB:OK" : "DB:FAIL";
    oled.drawStr(50, 63, dbStatus.c_str());
    
    // Sensor errors
    String sensorStatus = "";
    if(!data.dhtValid) sensorStatus += "DHT11 ";
    if(!data.bh1750Valid) sensorStatus += "BH1750";
    if(sensorStatus.length() > 0) {
        int statusWidth = oled.getStrWidth(sensorStatus.c_str());
        oled.drawStr(128 - statusWidth - 5, 63, sensorStatus.c_str());
    }
    
    oled.sendBuffer();
}

// 串口打印函数
void printToSerial(const SensorData& data) {
    String logMessage = "传感器数据 - ";
    
    if (data.dhtValid) {
        logMessage += "温度: " + String(data.temperature) + "°C | 湿度: " + String(data.humidity) + "%";
    } else {
        logMessage += "DHT11读取失败";
    }
    
    if (data.bh1750Valid) {
        logMessage += " | 光照: " + String(data.light) + " lx";
    } else {
        logMessage += " | BH1750读取失败";
    }
    
    logWithTimestamp(logMessage);
}

// 获取当前时间函数
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
