#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- 設定 ----------------
const char* ssid = "***";
const char* password = "***";
const char* deviceID = "SATO-TEMP-01";
const char* udpAddress = "255.255.255.255";
const int udpPort = 50007;
const int udpPort2 = 50008;

const int BL_PIN = 4;
const int PWR_PIN = 14;
const int ADC_PIN = 34;
const int ONE_WIRE_BUS = 17;

// ---------------- インスタンス ----------------
WiFiUDP udp;
WiFiUDP udp2;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// LovyanGFX: TTGO T-Display 用カスタム設定
class LGFX_TTGO : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST7789 _panel;
public:
  LGFX_TTGO() {
    auto cfg = _bus.config();
    cfg.spi_host = SPI2_HOST;
    cfg.freq_write = 40000000;
    cfg.pin_sclk = 18;
    cfg.pin_mosi = 19;
    cfg.pin_dc = 16;
    _bus.config(cfg);
    _panel.setBus(&_bus);
    auto pcfg = _panel.config();
    pcfg.pin_cs = 5;
    pcfg.pin_rst = 23;
    pcfg.panel_width = 135;
    pcfg.panel_height = 240;
    pcfg.offset_x = 52;
    pcfg.offset_y = 40;
    pcfg.invert = true;
    _panel.config(pcfg);
    setPanel(&_panel);
  }
};
LGFX_TTGO tft;
LGFX_Sprite sprite(&tft);

// ---------------- グローバル変数 ----------------
float currentTemp = 0.0;
unsigned long lastUpdate = 0;
unsigned long lastTempRequest = 0;
unsigned long lastLog1sec = 0;
unsigned long lastLog3min = 0;
unsigned long sleepTimer = 0;

// ---------------- 関数 ----------------
float getVbat() {
  uint16_t v = analogRead(ADC_PIN);
  return ((float)v / 4095.0) * 3.3 * 2.0;
}

void setup() {
  Serial.begin(115200);
  
  // 電源保持ピンの有効化
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);

  // センサー初期化（プログラムによるプルアップ適用）
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
  sensors.begin();
  sensors.setWaitForConversion(false); // 非同期モード：変換完了を待たずに自走

  // 通信・液晶の初期化
  WiFi.begin(ssid, password);
  tft.begin();
  tft.setRotation(1);
  sprite.createSprite(240, 135);
  
  // バックライト設定
  ledcAttach(BL_PIN, 5000, 8);
  ledcWrite(BL_PIN, 10);

  sleepTimer = millis(); 
}

void loop() {
  unsigned long now = millis();

  // 1. 温度取得（1秒周期：非同期）
  if (now - lastTempRequest >= 1000) {
    currentTemp = sensors.getTempCByIndex(0);
    sensors.requestTemperatures();
    lastTempRequest = now;

    // スリープ判定の規律：50度超でタイマーリセット、50度以下が1分続くと眠る
    if (currentTemp > 50.0 || currentTemp == DEVICE_DISCONNECTED_C) {
      sleepTimer = now;
    } else {
      if (now - sleepTimer >= 60000) {
        sprite.fillScreen(TFT_BLACK);
        sprite.pushSprite(0, 0);
        delay(100);
        esp_deep_sleep_start();
      }
    }
  }

  // 電圧測定（描画・判定用）
  float vbat = getVbat();

  // 2. 通信・描画処理（100ms周期）
  if (now - lastUpdate >= 100) {
    lastUpdate = now;

    // UDP送信セクション
    if (WiFi.status() == WL_CONNECTED) {
      // ポート50007：1秒周期（いじめ試験用）
      if (now - lastLog1sec >= 1000) {
        udp.beginPacket(udpAddress, udpPort);
        udp.printf("%s,STRESS,T:%.1f,V:%.2f\n", deviceID, currentTemp, vbat);
        udp.endPacket();
        lastLog1sec = now;
      }

      // ポート50008：3分周期（本命ログ用）
      if (now - lastLog3min >= 180000) {
        udp2.beginPacket(udpAddress, udpPort2);
        udp2.printf("%s,LOG,T:%.1f,V:%.2f\n", deviceID, currentTemp, vbat);
        udp2.endPacket();
        lastLog3min = now;
      }
    }

    // 3. 液晶描画セクション
    sprite.fillScreen(TFT_BLACK);

    // メイン温度表示
    if (currentTemp == DEVICE_DISCONNECTED_C) {
      sprite.setTextColor(TFT_RED);
      sprite.drawCenterString("SENSOR ERROR", 120, 30);
    } else {
      sprite.setTextColor(TFT_GREEN);
      sprite.setFont(&fonts::Font7);
      sprite.setTextSize(1.5);
      sprite.drawCenterString(String(currentTemp, 1), 120, 30);
    }

    // インジケーター：色の規律
    sprite.setFont(&fonts::Font0);
    sprite.setTextSize(3);

    // 電圧状態 (C:充電 / V:放電) 3色の信号
    sprite.setCursor(10, 90);
    if (vbat > 4.0) { // 充電中判定
      if (vbat > 4.9)      sprite.setTextColor(TFT_BLUE);
      else if (vbat > 4.6) sprite.setTextColor(TFT_YELLOW);
      else                 sprite.setTextColor(TFT_RED);
      sprite.print("C");
    } else { // 放電中判定
      if (vbat > 4.0)      sprite.setTextColor(TFT_BLUE);
      else if (vbat > 3.7) sprite.setTextColor(TFT_YELLOW);
      else                 sprite.setTextColor(TFT_RED);
      sprite.print("V");
    }

    // UDP送信状態 (U)
    if (WiFi.status() == WL_CONNECTED) {
      sprite.setCursor(10, 115);
      sprite.setTextColor(TFT_CYAN);
      sprite.print("U");
    }

    sprite.pushSprite(0, 0);
  }

  // ハードウェアの安定を担保する溜め
  delay(750);
}
