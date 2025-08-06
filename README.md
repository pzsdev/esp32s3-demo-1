# ESP32-S3 环境监测项目

## 项目概述
本项目使用ESP32-S3开发板连接DHT11温湿度传感器、BH1750光线传感器和OLED显示屏，实时监测环境数据并上传到InfluxDB数据库。

## 硬件要求
- ESP32-S3-DevKitM-1开发板
- DHT11温湿度传感器
- BH1750光线传感器
- SSD1306 OLED显示屏(128x64)

## 连接方式
- DHT11: GPIO 6
- OLED: I2C (SDA=5, SCL=4)
- BH1750: I2C (SDA=15, SCL=16)

## 软件依赖
- PlatformIO框架
- Arduino框架
- 依赖库:
  - Adafruit DHT sensor library
  - U8g2 (OLED驱动)
  - ESP8266 Influxdb
  - BH1750

## 配置说明
1. 修改`src/main.cpp`中的以下配置:
```cpp
#define WIFI_SSID "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"
#define INFLUXDB_URL "你的InfluxDB地址"
#define INFLUXDB_TOKEN "你的InfluxDB令牌"
#define INFLUXDB_ORG "你的组织名称"
#define INFLUXDB_BUCKET "你的存储桶名称"
```

2. 确保platformio.ini配置正确:
```ini
[env:esp32-s3-devkitm-1]
platform = espressif32
board = esp32-s3-devkitm-1
framework = arduino
lib_deps = 
    adafruit/DHT sensor library@^1.4.6
    olikraus/U8g2@^2.36.12
    tobiasschuerg/ESP8266 Influxdb@^3.13.2
    claws/BH1750@^1.3.0
```

## 使用说明
1. 连接硬件设备
2. 上传程序到ESP32-S3
3. 设备将:
   - 每2秒采集一次传感器数据
   - 在OLED显示屏上显示温湿度和光照数据
   - 通过串口输出监测数据
   - 将数据上传到InfluxDB

## 数据字段
- temperature: 温度(°C)
- humidity: 湿度(%)
- light: 光照强度(lx)

## 注意事项
- 确保所有传感器正确连接
- 首次使用时需要配置WiFi和InfluxDB信息
- 如果传感器读数异常，请检查接线和传感器状态
