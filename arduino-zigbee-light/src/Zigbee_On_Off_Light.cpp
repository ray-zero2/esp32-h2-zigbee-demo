// ***********************************************************************
// Zigbee_On_Off_Light.cpp
// このスケッチは「Zigbee ライトバルブ」のリファレンス実装です。
// ESP32‑H2 を Zigbee エンドデバイスとして動作させ、
// On/Off Cluster を用いて光の点灯 / 消灯を制御します。
// ボタン長押しでファクトリーリセット、短押しで手動トグルが可能です。
// ***********************************************************************
#include <Arduino.h>

// Arduino 基本APIを利用 (GPIO / Serial / delay など)

// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @brief This example demonstrates simple Zigbee light bulb.
 *
 * The example demonstrates how to use Zigbee library to create a end device light bulb.
 * The light bulb is a Zigbee end device, which is controlled by a Zigbee coordinator.
 *
 * Proper Zigbee mode must be selected in Tools->Zigbee mode
 * and also the correct partition scheme must be selected in Tools->Partition Scheme.
 *
 * Please check the README.md for instructions and more detailed description.
 *
 * Created by Jan Procházka (https://github.com/P-R-O-C-H-Y/)
 */

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
#include "esp_timer.h"

#include "esp_ieee802154.h"   // RSSI 用
volatile int8_t g_last_rssi = -127;   // 最近の RSSI 値
// Espressif の高水準 Zigbee ライブラリ
// ZCL 生成・ネットワーク管理を簡略化

/* Zigbee light bulb configuration */
// ---- Zigbee エンドポイントと GPIO 定義 ----
#define ZIGBEE_LIGHT_ENDPOINT 10     // 本バルブが応答するエンドポイント番号
uint8_t led = RGB_BUILTIN;           // 制御対象の LED ピン (RGB 組込み LED を使用)
uint8_t button = BOOT_PIN;           // BOOT ボタンを工場出荷状態リセットに転用

ZigbeeLight zbLight = ZigbeeLight(ZIGBEE_LIGHT_ENDPOINT); // Zigbee Light クラスを生成 (EP=10)

/********************* RGB LED functions **************************/


void my_rx_callback(/* Zigbee frame info */) {
  g_last_rssi = esp_ieee802154_get_recent_rssi();  // dBm
}

static void IRAM_ATTR print_signal_timer(void *arg)
{
    // NOTE: シリアル送信は ISR 不可。フラグを立てても良いが
    // Arduino core の task スケジューラは safe print に対応しているので OK
    Serial.printf("RSSI=%3d dBm\n", g_last_rssi);
}

void setLED(bool value) {
  my_rx_callback();  // RSSI 更新
  // Zigbee コールバックから呼ばれ、LED 出力を更新
  digitalWrite(led, value);
}

/********************* Arduino functions **************************/
void setup() {
  // ========================   初期化   ==============================
  Serial.begin(115200);
  // シリアルモニタ開始

  // ----- LED 初期化 -----
  pinMode(led, OUTPUT);
  digitalWrite(led, LOW);

  // ----- BOOT ボタン (工場出荷リセット用) 初期化 -----
  pinMode(button, INPUT_PULLUP);

  // Zigbee デバイス情報 (任意)
  zbLight.setManufacturerAndModel("Espressif", "ZBLightBulb");

  // LED 状態変化時のコールバック登録
  zbLight.onLightChange(setLED);

  // Zigbee Core にエンドポイントを登録
  Serial.println("Adding ZigbeeLight endpoint to Zigbee Core");
  Zigbee.addEndpoint(&zbLight);

  // Zigbee スタック起動 (エンドデバイス)
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  }
  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");    // ネットワーク参加待ち
    delay(100);
  }

  const esp_timer_create_args_t tcfg = {
    .callback = &print_signal_timer,
    .name = "sig_out"
  };
  esp_timer_handle_t h;
  esp_timer_create(&tcfg, &h);
  esp_timer_start_periodic(h, 500 * 1000);

  Serial.println();
}

void loop() {
  // ========================   メインループ   ============================
  // ボタン入力を監視し、短押しで On/Off トグル / 長押しで工場出荷リセット
  // Checking button for factory reset
  if (digitalRead(button) == LOW) {  // Push button pressed
    // ----- ボタンが押されたのでデバウンス -----
    // Key debounce handling
    delay(100);
    int startTime = millis();
    while (digitalRead(button) == LOW) {
      delay(50);
      if ((millis() - startTime) > 3000) {
        // If key pressed for more than 3secs, factory reset Zigbee and reboot
        // Zigbee NVS 情報を消去し、再起動後にペアリング待ち状態へ
        Serial.println("Resetting Zigbee to factory and rebooting in 1s.");
        delay(1000);
        Zigbee.factoryReset();
      }
    }
    // 短押し → 現在のライト状態を反転
    // Toggle light by pressing the button
    zbLight.setLight(!zbLight.getLightState());
  }
  delay(100);
}
