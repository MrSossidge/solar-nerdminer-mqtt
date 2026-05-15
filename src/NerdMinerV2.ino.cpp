#include <Wire.h>

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include <OneButton.h>

#include "mbedtls/md.h"
#include "wManager.h"
#include "mining.h"
#include "monitor.h"
#include "drivers/displays/display.h"
#include "drivers/storage/SDCard.h"
#include "ShaTests/nerdSHA_HWTest.h"
#include "timeconst.h"
#include "mqtt_manager.h"

#ifdef TOUCH_ENABLE
#include "TouchHandler.h"
#endif

#include <soc/soc_caps.h>
//#define HW_SHA256_TEST

// 3 seconds WDT
#define WDT_TIMEOUT 3
// 15 minutes WDT for miner task
#define WDT_MINER_TIMEOUT 900

#ifdef PIN_BUTTON_1
  OneButton button1(PIN_BUTTON_1);
#endif

#ifdef PIN_BUTTON_2
  OneButton button2(PIN_BUTTON_2);
#endif

#ifdef TOUCH_ENABLE
extern TouchHandler touchHandler;
#endif

extern monitor_data mMonitor;

#ifdef SD_ID
  SDCard SDCrd = SDCard(SD_ID);
#else  
  SDCard SDCrd = SDCard();
#endif

/**********************⚡ GLOBAL Vars *******************************/

unsigned long start = millis();
const char* ntpServer = "pool.ntp.org";
TaskHandle_t minerTask1 = NULL;
TaskHandle_t minerTask2 = NULL;

/********* INIT *****/
void setup()
{
  // Init pin 15 to enable 5V external power (LilyGo bug)
  #ifdef PIN_ENABLE5V
      pinMode(PIN_ENABLE5V, OUTPUT);
      digitalWrite(PIN_ENABLE5V, HIGH);
  #endif

  #ifdef MONITOR_SPEED
      Serial.begin(MONITOR_SPEED);
  #else
      Serial.begin(115200);
  #endif

  Serial.setTimeout(0);
  delay(SECOND_MS/10);

  esp_task_wdt_init(WDT_MINER_TIMEOUT, true);
  // Idle task that would reset WDT never runs because core 0 gets fully utilized
  disableCore0WDT();

  #ifdef HW_SHA256_TEST
    while (1) HwShaTest();
  #endif

  // Setup the buttons
  #if defined(PIN_BUTTON_1) && !defined(PIN_BUTTON_2)
    button1.setPressMs(5*SECOND_MS);
    button1.attachClick(switchToNextScreen);
    button1.attachDoubleClick(alternateScreenRotation);
    button1.attachLongPressStart(reset_configuration);
    button1.attachMultiClick(alternateScreenState);
  #endif

  #if defined(PIN_BUTTON_1) && defined(PIN_BUTTON_2)
    button1.setPressMs(5*SECOND_MS);
    button1.attachClick(alternateScreenState);
    button1.attachDoubleClick(alternateScreenRotation);
  #endif

  #if defined(PIN_BUTTON_2)
    button2.setPressMs(5*SECOND_MS);
    button2.attachClick(switchToNextScreen);
    button2.attachLongPressStart(reset_configuration);
  #endif

  /******** INIT NERDMINER ************/
  Serial.println("NerdMiner v2 starting......");

  /******** INIT DISPLAY ************/
  initDisplay();
  
  /******** PRINT INIT SCREEN *****/
  drawLoadingScreen();
  delay(2*SECOND_MS);

  /******** SHOW LED INIT STATUS (devices without screen) *****/
  mMonitor.NerdStatus = NM_waitingConfig;
  doLedStuff(0);

  #ifdef SDMMC_1BIT_FIX
    SDCrd.initSDcard();
  #endif

  /******** INIT WIFI ************/
  init_WifiManager();

  /******** INIT TIME AND MQTT ************/
  mqttTimeInit();
  mqttConnect();

  /******** CHECK MINING WINDOW BEFORE STARTING TASKS *****/
  // If outside the solar window, go straight to sleep
  // This handles the case where the ESP wakes from deep sleep
  // and needs to check if it should mine or sleep again
  if (!isMiningWindow()) {
    int nowSecs = localSecsSinceMidnight();
    int sleepSecs;

    if (nowSecs < miningStartSecs) {
      // Before today's window
      sleepSecs = miningStartSecs - nowSecs;
    } else {
      // After today's window — sleep until tomorrow's start
      sleepSecs = (86400 - nowSecs) + miningStartSecs;
    }

    int sh = sleepSecs / 3600;
    int sm = (sleepSecs % 3600) / 60;
    Serial.printf("[SLEEP] Outside window. Sleeping %dh %dm until window opens...\n", sh, sm);

    // Publish offline status before sleeping
    mqttClient.publish(MQTT_BASE_TOPIC "/status", "sleeping", true);
    mqttClient.publish(MQTT_BASE_TOPIC "/mining_active", "false", true);
    mqttClient.loop();
    delay(1000);

    // Deep sleep until window opens — uses only ~5µA
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSecs * 1000000ULL);
    esp_deep_sleep_start();
    // Execution stops here — ESP restarts from setup() on wake
  }

  /******** CREATE TASK TO PRINT SCREEN *****/
  Serial.println("");
  Serial.println("Initiating tasks...");
  static const char monitor_name[] = "(Monitor)";
  #if defined(CONFIG_IDF_TARGET_ESP32)
  BaseType_t res1 = xTaskCreatePinnedToCore(runMonitor, "Monitor", 9500, (void*)monitor_name, 5, NULL, 1);
  #else
  BaseType_t res1 = xTaskCreatePinnedToCore(runMonitor, "Monitor", 10000, (void*)monitor_name, 5, NULL, 1);
  #endif

  /******** CREATE STRATUM TASK *****/
  static const char stratum_name[] = "(Stratum)";
  #if defined(CONFIG_IDF_TARGET_ESP32) && !defined(ESP32_2432S028R) && !defined(ESP32_2432S028_2USB)
  BaseType_t res2 = xTaskCreatePinnedToCore(runStratumWorker, "Stratum", 12000, (void*)stratum_name, 4, NULL, 1);
  #elif defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  BaseType_t res2 = xTaskCreatePinnedToCore(runStratumWorker, "Stratum", 13500, (void*)stratum_name, 4, NULL, 1);
  #else
  BaseType_t res2 = xTaskCreatePinnedToCore(runStratumWorker, "Stratum", 15000, (void*)stratum_name, 4, NULL, 1);
  #endif

  /******** CREATE MINER TASKS *****/
  minerTask1 = NULL;
  minerTask2 = NULL;

  #ifdef HARDWARE_SHA265
    #if defined(CONFIG_IDF_TARGET_ESP32)
    xTaskCreate(minerWorkerHw, "MinerHw-0", 3584, (void*)0, 3, &minerTask1);
    #else
    xTaskCreate(minerWorkerHw, "MinerHw-0", 4096, (void*)0, 3, &minerTask1);
    #endif
  #else
    #if defined(CONFIG_IDF_TARGET_ESP32)
    xTaskCreate(minerWorkerSw, "MinerSw-0", 5000, (void*)0, 1, &minerTask1);
    #else
    xTaskCreate(minerWorkerSw, "MinerSw-0", 6000, (void*)0, 1, &minerTask1);
    #endif
  #endif
  esp_task_wdt_add(minerTask1);

  #if (SOC_CPU_CORES_NUM >= 2)
    #if defined(CONFIG_IDF_TARGET_ESP32)
    xTaskCreate(minerWorkerSw, "MinerSw-1", 5000, (void*)1, 1, &minerTask2);
    #else
    xTaskCreate(minerWorkerSw, "MinerSw-1", 6000, (void*)1, 1, &minerTask2);
    #endif
    esp_task_wdt_add(minerTask2);
  #endif

  vTaskPrioritySet(NULL, 4);

  /******** MONITOR SETUP *****/
  setup_monitor();
}

void app_error_fault_handler(void *arg) {
  char *stack = (char *)arg;
  esp_log_write(ESP_LOG_ERROR, "APP_ERROR", "Error Stack Code:\n%s", stack);
  esp_restart();
}

void loop() {
  // Keep watching the push buttons
  #ifdef PIN_BUTTON_1
    button1.tick();
  #endif

  #ifdef PIN_BUTTON_2
    button2.tick();
  #endif

  #ifdef TOUCH_ENABLE
    touchHandler.isTouched();
  #endif

  // Keep WiFi manager alive
  wifiManagerProcess();

  // Keep MQTT connection alive
  mqttLoop();

  // Check mining window once per minute and publish stats
  static unsigned long lastWindowCheck = 0;
  if (millis() - lastWindowCheck >= 60000UL) {
    lastWindowCheck = millis();

    if (!isMiningWindow()) {
      // Window has closed — calculate sleep duration
      int nowSecs = localSecsSinceMidnight();
      int sleepSecs;

      if (nowSecs < miningStartSecs) {
        sleepSecs = miningStartSecs - nowSecs;
      } else {
        sleepSecs = (86400 - nowSecs) + miningStartSecs;
      }

      int sh = sleepSecs / 3600;
      int sm = (sleepSecs % 3600) / 60;
      Serial.printf("[SLEEP] Window closed. Sleeping %dh %dm...\n", sh, sm);

      // Publish final stats before sleeping
      unsigned long mElapsed = millis() - start;
      mining_data md = getMiningData(mElapsed);
      mqttPublishStats(
          md.currentHashRate,
          md.completedShares,
          md.totalKHashes,
          md.timeMining,
          md.valids,
          false
      );

      // Give MQTT time to send
      mqttClient.publish(MQTT_BASE_TOPIC "/status", "sleeping", true);
      mqttClient.loop();
      delay(2000);

      // Deep sleep until window opens
      esp_sleep_enable_timer_wakeup((uint64_t)sleepSecs * 1000000ULL);
      mqttClient.publish(MQTT_BASE_TOPIC "/status",        "sleeping", true);
      mqttClient.publish(MQTT_BASE_TOPIC "/mining_active", "false",    true);
      mqttClient.publish(MQTT_BASE_TOPIC "/hashrate",      "0.00",     true);
      mqttClient.publish(MQTT_BASE_TOPIC "/uptime",        "sleeping", true);
      mqttClient.publish(MQTT_BASE_TOPIC "/shares",        "sleeping", true);
      mqttClient.publish(MQTT_BASE_TOPIC "/total_kh",      "sleeping", true);
      mqttClient.publish(MQTT_BASE_TOPIC "/valids",        "sleeping", true);
      mqttClient.loop();
      delay(1000);
      esp_deep_sleep_start();
      // ESP restarts from setup() on wake
    }

    // Inside window — publish stats
    unsigned long mElapsed = millis() - start;
    mining_data md = getMiningData(mElapsed);
    mqttPublishStats(
        md.currentHashRate,
        md.completedShares,
        md.totalKHashes,
        md.timeMining,
        md.valids,
        true
    );
  }

  vTaskDelay(50 / portTICK_PERIOD_MS);
}