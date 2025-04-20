// ***********************************************************************
// Zigbee_On_Off_Switch.cpp
// このスケッチは「Zigbee ライトスイッチ」のリファレンス実装です。
// ESP32‑H2（協調動作モード : Zigbee コーディネータ）上で動作し、
// 物理ボタンの操作を Zigbee 経由でバインド済みのライトに転送します。
// ボタン処理・Zigbee 処理を別タスクで同時実行する構成です。
// ***********************************************************************
#include <Arduino.h>
// Arduino コアに含まれる基本 API を利用
// ※ Serial/デジタル I/O/FreeRTOS ラッパなど

// ----------------------------------------------------------------------
// ツールメニューで「Zigbee Coordinator」を選択していない場合は
// ここでビルドを停止し、ユーザに設定ミスを知らせます。
// ----------------------------------------------------------------------
#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee coordinator mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
// Espressif 提供の高水準 Zigbee ラッパー
// ZCL コマンド生成・バインディング管理などを簡素化

// スイッチが使う Zigbee エンドポイント番号
// ※ 同一チップ内で複数 EP を使う場合は衝突しない値に変更してください
#define SWITCH_ENDPOINT_NUMBER 5

#define GPIO_INPUT_IO_TOGGLE_SWITCH BOOT_PIN
// BOOT ボタン(GPIO9) をスイッチ入力に流用
// 他のピンを使う場合は変更可
#define PAIR_SIZE(TYPE_STR_PAIR)    (sizeof(TYPE_STR_PAIR) / sizeof(TYPE_STR_PAIR[0]))

// ボタンが実行できる機能を列挙
// 今回はトグルのみ使用し、他の列挙は拡張用として定義
typedef enum {
  SWITCH_ON_CONTROL,          // 点灯
  SWITCH_OFF_CONTROL,         // 消灯
  SWITCH_ONOFF_TOGGLE_CONTROL,// トグル
  SWITCH_LEVEL_UP_CONTROL,    // 明るさを上げる
  SWITCH_LEVEL_DOWN_CONTROL,  // 明るさを下げる
  SWITCH_LEVEL_CYCLE_CONTROL, // 明るさを循環
  SWITCH_COLOR_CONTROL,       // 色を変更
} SwitchFunction;

// 物理ボタンと機能の対応表
// BOOT_PIN に接続されたボタンでライトのトグルを実行
typedef struct {
  uint8_t pin;
  SwitchFunction func;
} SwitchData;

typedef enum {
  SWITCH_IDLE,               // 待機状態
  SWITCH_PRESS_ARMED,        // 押下準備状態
  SWITCH_PRESS_DETECTED,     // 押下検出状態
  SWITCH_PRESSED,            // 押下中
  SWITCH_RELEASE_DETECTED,    // 離した検出状態
} SwitchState;

// ==== ボタン機能マッピング ==============================
// 配列要素を増やすと複数ボタンに対応出来る
static SwitchData buttonFunctionPair[] = {{GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}};

// Zigbee ライトスイッチクラスを生成（EP=5）
ZigbeeSwitch zbSwitch = ZigbeeSwitch(SWITCH_ENDPOINT_NUMBER);

/********************* Zigbee functions **************************/
static void onZbButton(SwitchData *button_func_pair) {
  // 受信した引数から対応する Zigbee コマンドを実行
  // ボタンが押された時のコールバック
  // ここではトグルコマンドを Zigbee 経由で送信します
  if (button_func_pair->func == SWITCH_ONOFF_TOGGLE_CONTROL) {
    // Zigbee Cluster Library の On/Off Toggle コマンドを送信
    // Send toggle command to the light
    Serial.println("Toggling light");
    zbSwitch.lightToggle();
  }
}

/********************* GPIO functions **************************/
static QueueHandle_t gpio_evt_queue = NULL;

// ---------------------------- GPIO 割り込み ----------------------------
// ボタンが押下された瞬間に呼び出され、キューへイベントを送信
static void IRAM_ATTR onGpioInterrupt(void *arg) {
  // 割り込みは最小限の処理に留め、実処理はキューへ
  xQueueSendFromISR(gpio_evt_queue, (SwitchData *)arg, NULL);
}

static void enableGpioInterrupt(bool enabled) {
  // ボタン処理中はチャタリング回避のため割り込みを無効化する
  for (int i = 0; i < PAIR_SIZE(buttonFunctionPair); ++i) {
    if (enabled) {
      // pin 番号ごとに enable/disable を切り替え
      enableInterrupt((buttonFunctionPair[i]).pin);
    } else {
      disableInterrupt((buttonFunctionPair[i]).pin);
    }
  }
}

/********************* Arduino functions **************************/
void setup() {
  // ========================   初期化処理   ==============================
  Serial.begin(115200);
  // シリアルモニタ開始

  // Zigbee デバイス情報（オプション）
  zbSwitch.setManufacturerAndModel("Espressif", "ZigbeeSwitch");

  // Optional to allow multiple light to bind to the switch
  zbSwitch.allowMultipleBinding(true);

  // Core に EP を登録
  Serial.println("Adding ZigbeeSwitch endpoint to Zigbee Core");
  Zigbee.addEndpoint(&zbSwitch);

  // 再起動後 180 秒間はネットワークをオープン
  Zigbee.setRebootOpenNetwork(180);

  // ------- ボタン GPIO / 割り込み設定 -------
  // Init button switch
  for (int i = 0; i < PAIR_SIZE(buttonFunctionPair); i++) {
    pinMode(buttonFunctionPair[i].pin, INPUT_PULLUP);
    /* create a queue to handle gpio event from isr */
    gpio_evt_queue = xQueueCreate(10, sizeof(SwitchData));
    // FreeRTOS キュー：ISR → ループ間でボタンイベントを受け渡す
    if (gpio_evt_queue == 0) {
      Serial.println("Queue creating failed, rebooting...");
      ESP.restart();
    }
    attachInterruptArg(buttonFunctionPair[i].pin, onGpioInterrupt, (void *)(buttonFunctionPair + i), FALLING);
    // 立下りエッジ (FALLING) で割り込み発火
  }

  // Zigbee コア起動（コーディネータとして）
  if (!Zigbee.begin(ZIGBEE_COORDINATOR)) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  }
  // ここまで来れば Zigbee スタックは稼働中

  Serial.println("Waiting for Light to bound to the switch");
  // ライトがバインドされるまで待機
  //Wait for switch to bound to a light:
  while (!zbSwitch.bound()) {
    Serial.printf(".");
    delay(500);
  }

  // バインド済みライトの情報を列挙
  // Optional: List all bound devices and read manufacturer and model name
  std::list<zb_device_params_t *> boundLights = zbSwitch.getBoundDevices();
  for (const auto &device : boundLights) {
    Serial.printf("Device on endpoint %d, short address: 0x%x\r\n", device->endpoint, device->short_addr);
    Serial.printf(
      "IEEE Address: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\r\n", device->ieee_addr[7], device->ieee_addr[6], device->ieee_addr[5], device->ieee_addr[4],
      device->ieee_addr[3], device->ieee_addr[2], device->ieee_addr[1], device->ieee_addr[0]
    );
    Serial.printf("Light manufacturer: %s\r\n", zbSwitch.readManufacturer(device->endpoint, device->short_addr, device->ieee_addr));
    Serial.printf("Light model: %s\r\n", zbSwitch.readModel(device->endpoint, device->short_addr, device->ieee_addr));
  }

  Serial.println();
}

void loop() {
  // ========================   メインループ   ============================
  // 割り込みで受け取ったボタンイベントを状態遷移で評価し、Zigbee へ送信
  // Handle button switch in loop()
  uint8_t pin = 0;
  SwitchData buttonSwitch;
  static SwitchState buttonState = SWITCH_IDLE;
  bool eventFlag = false;

  /* check if there is any queue received, if yes read out the buttonSwitch */
  if (xQueueReceive(gpio_evt_queue, &buttonSwitch, portMAX_DELAY)) {
    pin = buttonSwitch.pin;
    // 受信したらチャタリング防止のため割り込みを一旦無効
    enableGpioInterrupt(false);
    eventFlag = true;
  }
  while (eventFlag) {
    bool value = digitalRead(pin);
    switch (buttonState) {
      case SWITCH_IDLE:           buttonState = (value == LOW) ? SWITCH_PRESS_DETECTED : SWITCH_IDLE; break; // 待機中
      case SWITCH_PRESS_DETECTED: // ボタンを押し続けている間ここ
        buttonState = (value == LOW) ? SWITCH_PRESS_DETECTED : SWITCH_RELEASE_DETECTED; break; // 押下判定中（長押し未対応）
      case SWITCH_RELEASE_DETECTED: // 離した瞬間ここ
        buttonState = SWITCH_IDLE;
        /* callback to button_handler */
        (*onZbButton)(&buttonSwitch); // 離した瞬間 -> コマンド送信
        break;
      default: break;
    }
    if (buttonState == SWITCH_IDLE) {
      // 割り込みを再度有効化しループを抜ける
      enableGpioInterrupt(true);
      eventFlag = false;
      break;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  // 10 秒ごとにバインド済みライトをシリアルへ出力（デバッグ用）
  // print the bound lights every 10 seconds
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 10000) {
    lastPrint = millis();
    // バインド先デバイス一覧を人間が確認しやすい形式で出力
    zbSwitch.printBoundDevices(Serial);
  }
}
