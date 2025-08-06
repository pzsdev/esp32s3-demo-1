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

void setup() {
  Serial.begin(115200);

  // 初始化第一个 I2C 总线 (Wire: SDA=5, SCL=4) - OLED
  Wire.begin(5, 4);
  oled.begin();

  // 初始化第二个 I2C 总线 (I2CBH1750: SDA=15, SCL=16) - BH1750
  I2CBH1750.begin(15, 16);  // SDA=15, SCL=16
  // lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2CBH1750);  // 使用自定义 I2C 总线

  // 检查 I2C 设备是否存在
  byte error, address;
  int nDevices = 0;
  for(address = 1; address < 127; address++ ) {
    I2CBH1750.beginTransmission(address);
    error = I2CBH1750.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address<16) Serial.print("0");
      Serial.print(address,HEX);
      Serial.println("  !");
      nDevices++;
    }
  }
  if (nDevices == 0) {
    Serial.println("No I2C devices found");
  }

  // 尝试两种可能的 BH1750 地址
  if(!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2CBH1750)) {
    Serial.println("Failed to initialize BH1750 at address 0x23, trying 0x5C");
    if(!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &I2CBH1750)) {
      Serial.println("Could not find a valid BH1750 sensor, check wiring!");
    } else {
      Serial.println("BH1750 initialized at address 0x5C");
    }
  } else {
    Serial.println("BH1750 initialized at address 0x23");
  }

  dht.begin();          // 启动DHT11

  // 显示初始界面
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tr);
  oled.drawStr(0, 15, "DHT11 Monitor");
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 35, "Initializing...");
  oled.sendBuffer();
  delay(1000);

  WiFi.mode(WIFI_STA);                                              //设置 WiFi 连接
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("正在连接到 WiFi");                               //连接到 WiFi
  while (wifiMulti.run() != WL_CONNECTED) 
  {
    Serial.print(".");
    delay(100);
  }
  Serial.println();

  sensor.addTag("device", DEVICE);                                   //添加标签 - 根据需要重复
  sensor.addTag("SSID", WIFI_SSID);

  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");                 //准确时间对于证书验证和批量写入是必要的

  if (client.validateConnection())                                   //检查服务器连接
  {
    Serial.print("已连接到 InfluxDB: ");
    Serial.println(client.getServerUrl());
  } 
  else 
  {
    Serial.print("InfluxDB 连接失败: ");
    Serial.println(client.getLastErrorMessage());
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
        Serial.println("WiFi连接丢失");
        return;
    }

    // 写入数据
    if (!client.writePoint(sensor)) {
        Serial.print("InfluxDB写入失败: ");
        Serial.println(client.getLastErrorMessage());
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
    Serial.print("时间: ");
    Serial.print(data.currentTime);
    
    if (data.dhtValid) {
        Serial.print(" | 温度: ");
        Serial.print(data.temperature);
        Serial.print("°C");
        Serial.print(" | 湿度: ");
        Serial.print(data.humidity);
        Serial.print("%");
    } else {
        Serial.print(" | DHT11读取失败");
    }
    
    if (data.bh1750Valid) {
        Serial.print(" | 光照: ");
        Serial.print(data.light);
        Serial.print(" lx");
    } else {
        Serial.print(" | BH1750读取失败");
    }
    
    Serial.println();
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