#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Ticker.h>

#define RFID_RX D5     // Подключите TX модуля RFID к пину D5 (GPIO14)
#define RFID_TX D6     // Подключите RX модуля RFID к пину D6 (GPIO12)
#define EEPROM_SIZE 64 // Размер памяти EEPROM
#define BUZZER_PIN D7  // Пин, к которому подключён KY-012
#define RELAY_PIN1 D8  // Пин реле, который управляет соленоидом

// Параметры карт
#define CARD_SIZE           5 // Размер UID карты
#define MASTER_CARD_ADDR    0 // Адрес хранения мастер-карты
#define ALLOWED_CARDS_ADDR 10 // Адрес хранения разрешённых карт
#define MAX_CARDS           5 // Максимум дополнительных карт

// Таймаут до аварийного рестарта
#define RESTART_LIMIT 60000
#define RELAY_OPEN 1
#define RELAY_CLOSED 0

#define DEFAULT_BAUD_RATE 9600

// Настройки Wi-Fi
const char* ssid     = "Your_WiFi_SSID"; // Имя Wi-Fi сети
const char* password = "passw0rd123";   // Пароль от Wi-Fi

// Настройки MQTT
const int   mqtt_json_buffer_size = 512;               // Расширенный буффер для большинства возможных json'ов
const char* mqtt_server           = "192.168.1.5";     // IP-адрес вашего MQTT-брокера (например, Home Assistant)
const int   mqtt_port             = 1883;              // Стандартный порт для MQTT-брокера
const char* mqtt_user             = "homeassistant";   // Имя пользователя MQTT (если требуется)
const char* mqtt_password         = "mypass_secure";   // Пароль MQTT (если требуется)
const char* mqtt_topic_lock_op    = "home/lock/op";    // Топик для управления замком
const char* mqtt_topic_lock_state = "home/lock/state"; // Топик для синхронизации статусов

const char* mqtt_topic_locker_discovery       = "homeassistant/lock/whiskey_locker/config";          // Топик для дискавери устройства
const char* mqtt_topic_btn_reboot_discovery   = "homeassistant/button/whiskey_locker/whiskey_locker_reboot/config"; // Топик для дискавери кнопки ребута
const char* mqtt_topic_btn_add_card_discovery = "homeassistant/button/whiskey_locker/whiskey_locker_setup/config";  // Топик для дискавери кнопки новой карты

// Флаг подключения к Wi-Fi
bool wifiConnected = false;
enum OperatingMode {
  NORMAL,
  CONNECTING,
  WAITING_FOR_SETUP,
  WAITING_FOR_NEW_CARD
};

// Текущий статус замка в HomeAssistant
enum LockStatus {
  OPEN,
  BOOTING,
  SETUP,
  LOCKED
};
const char* LockStatusStrings[] = {
  "open",
  "offline",
  "waiting_for_setup",
  "locked"
};

// Конфигурационные методы
void prepareRFID();
void prepareSolenoid();
void setupWatchdog();
void setupHomeAssistant();

// Сеть
void taskEstablishWiFiConnection();
void taskSubscribeToMQTTBroker();

// Методы работы с mqtt брокером
void mqttMainLooper();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttProceedOnTopicMessageReceived(String msg);
void publishMqttStatus(LockStatus status);
void startMqttDiscovery();

// Декларируем функции для работы с картами и замком
void reboot();
void activateSolenoid(int waitMs);
void forceCloseLocker();
bool checkAndPrintMasterCard();
void listenForRFID(unsigned char uid[CARD_SIZE]);
void processCard(unsigned char cardUID[CARD_SIZE]);
bool isCardUnset(unsigned char uid[CARD_SIZE]);
void saveMasterCard(unsigned char uid[CARD_SIZE]);
void readMasterCard(unsigned char uid[CARD_SIZE]);
void addAllowedCard(unsigned char uid[CARD_SIZE]);
bool isCardAllowed(unsigned char uid[CARD_SIZE]);
bool compareUID(unsigned char uid1[CARD_SIZE], unsigned char uid2[CARD_SIZE]);
void printCardUID(unsigned char uid[CARD_SIZE]);

// Внутренний стейт замка
void setOperationMode(OperatingMode mode);
bool isCurrentlyOperating(OperatingMode mode);
void resetOperatingMode();

// Утилитные методы для работы с JSON и HomeAssistant
JsonObject createDeviceInfo(JsonDocument& doc);
JsonObject createAvailabilityInfo(JsonDocument& doc);

// Серийные TX/RX порты для чтения данных антенны
SoftwareSerial rfidSerial(RFID_RX, RFID_TX);

// Умный дом
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Текущий режим замка
OperatingMode currentMode = NORMAL;
Ticker timer;

void setup() {
  Serial.begin(DEFAULT_BAUD_RATE);
  setOperationMode(CONNECTING);

  prepareRFID();
  prepareSolenoid();
  setupWatchdog();
  setupHomeAssistant();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setOperationMode(CONNECTING);

    setupHomeAssistant();
  }

  if (rfidSerial.available()) {
    unsigned char cardUID[CARD_SIZE] = {0};
    listenForRFID(cardUID);
    processCard(cardUID);
  }

  mqttMainLooper();

  // update watchdog
  ESP.wdtFeed();
}

void prepareRFID() {
  rfidSerial.begin(DEFAULT_BAUD_RATE);
  EEPROM.begin(EEPROM_SIZE);
  
  checkAndPrintMasterCard();
  Serial.println("RFID System ready to interact");
}

void prepareSolenoid() {
  pinMode(RELAY_PIN1, OUTPUT);
}

void setupWatchdog() {
  // таймер аварийного рестарта
  ESP.wdtEnable(RESTART_LIMIT);
  Serial.println("Emergency restart watchdog has been attached");
}

void setupHomeAssistant() {
  if (!isCurrentlyOperating(CONNECTING)) {
    Serial.println("ERROR! This method should be called only in Connecting mode!");
    return;
  }

  WiFi.mode(WIFI_STA);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(mqtt_json_buffer_size);
  mqttClient.setCallback(mqttCallback);

  taskEstablishWiFiConnection();
  taskSubscribeToMQTTBroker();

  if (!checkAndPrintMasterCard()) {
    setOperationMode(WAITING_FOR_SETUP);
  } else {
    setOperationMode(NORMAL);
  }

  startMqttDiscovery();
}

void taskEstablishWiFiConnection() {
  if (!isCurrentlyOperating(CONNECTING)) {
    Serial.println("Состояния Wi-Fi: успешно!");
    return;
  }

  Serial.println();
  Serial.print("Соединение с Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi подключено");
    Serial.print("IP-адрес: ");
    Serial.println(WiFi.localIP());

    wifiConnected = true;
  } else {
    Serial.println("\nНе удалось подключиться к Wi-Fi. Попробуем снова.");
    delay(2000);
  }
}

void taskSubscribeToMQTTBroker() {
  while (!wifiConnected) {
    Serial.println("Ждём Wi-FI...");
    delay(1000);
    return;
  }

  while (!mqttClient.connected()) {
    Serial.print("Подключение к MQTT...");
    if (mqttClient.connect("ESP8266Client", mqtt_user, mqtt_password)) {
      Serial.println("Подключено!");

      mqttClient.subscribe(mqtt_topic_lock_op);
      publishMqttStatus(BOOTING);
      forceCloseLocker();
    } else {
      Serial.print("Ошибка, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Попробуем снова через 5 секунд");
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Получено сообщение [");
  Serial.print(topic);
  Serial.print("]: ");
  String message;
  for (int i = 0; i < length; i++) {
    message += (char) payload[i];
  }
  Serial.println(message);

  // Управление замком
  if (String(topic) == mqtt_topic_lock_op) {
    mqttProceedOnTopicMessageReceived(message);
  }
}

void mqttMainLooper() {
  if (wifiConnected && mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
}

void mqttProceedOnTopicMessageReceived(String msg) {
  if (msg == "lock") {
    forceCloseLocker();
  }

  if (msg == "open") {
    openLocker();
  }

  if (msg == "restart") {
    reboot();
  }

  if (msg == "new_card") {
    Serial.println("Wait to setup new safe card");

    setOperationMode(WAITING_FOR_NEW_CARD);
    timer.once(10, resetOperatingMode);
    publishMqttStatus(SETUP);
  }
}

void singleShortBeep() {
  tone(BUZZER_PIN, 1000);
  delay(200);
  noTone(BUZZER_PIN);
  delay(50);
}

void singleLongBeep() {
  tone(BUZZER_PIN, 1000);
  delay(1000);
  noTone(BUZZER_PIN);
}

void doubleShortBeep() {
  singleShortBeep();
  singleShortBeep();
}

void tripleShortBeep() {
  singleShortBeep();
  singleShortBeep();
  singleShortBeep();
}

void publishMqttStatus(LockStatus status) {
  mqttClient.publish(mqtt_topic_lock_state, LockStatusStrings[status]);
  Serial.print("The status has been sent: ");
  Serial.println(LockStatusStrings[status]);
}

void startMqttDiscovery() {
  // StaticJsonDocument для замка
  StaticJsonDocument<512> lockDoc;
  lockDoc["unique_id"] = "ha_wlock_khaotika";
  lockDoc["object_id"] = "whiskey_locker";
  lockDoc["name"] = "Wiskey Locker";
  lockDoc["state_topic"] = mqtt_topic_lock_state;
  lockDoc["command_topic"] = mqtt_topic_lock_op;
  lockDoc["payload_lock"] = "lock";
  lockDoc["payload_unlock"] = "open";
  lockDoc["state_locked"] = "locked";
  lockDoc["state_unlocked"] = "open";
  lockDoc["state_jammed"] = "offline";
  lockDoc["optimistic"] = false;
  lockDoc["qos"] = 2;
  createDeviceInfo(lockDoc);

  // Отправка замка
  char lockPayload[512];
  serializeJson(lockDoc, lockPayload);
  Serial.println("Sending discovery data:");
  Serial.println(lockPayload);
  mqttClient.publish(mqtt_topic_locker_discovery, lockPayload);

  // StaticJsonDocument для кнопки Reboot
  StaticJsonDocument<512> rebootDoc;
  rebootDoc["unique_id"] = "whiskey_locker_reboot";
  rebootDoc["object_id"] = "whiskey_locker_reboot";
  rebootDoc["name"] = "Reboot NodeMCU";
  rebootDoc["command_topic"] = mqtt_topic_lock_op;
  rebootDoc["payload_press"] = "restart";
  rebootDoc["entity_category"] = "config";
  rebootDoc["device_class"] = "restart";
  createAvailabilityInfo(rebootDoc);
  createDeviceInfo(rebootDoc);

  // Отправка кнопки Reboot
  char rebootPayload[512];
  serializeJson(rebootDoc, rebootPayload);
  Serial.println("Sending discovery data:");
  Serial.println(rebootPayload);
  mqttClient.publish(mqtt_topic_btn_reboot_discovery, rebootPayload);

  // StaticJsonDocument для кнопки Add a new card
  StaticJsonDocument<512> setupDoc;
  setupDoc["unique_id"] = "whiskey_locker_setup";
  setupDoc["object_id"] = "whiskey_locker_setup";
  setupDoc["name"] = "Add a new card";
  setupDoc["command_topic"] = mqtt_topic_lock_op;
  setupDoc["payload_press"] = "new_card";
  setupDoc["entity_category"] = "config";
  setupDoc["device_class"] = "identify";
  createAvailabilityInfo(setupDoc);
  createDeviceInfo(setupDoc);

  // Отправка кнопки Add a new card
  char setupPayload[512];
  serializeJson(setupDoc, setupPayload);
  Serial.println("Sending discovery data:");
  Serial.println(setupPayload);
  mqttClient.publish(mqtt_topic_btn_add_card_discovery, setupPayload);
  Serial.println("[MQTT] The discovery config has been sent!");
}

void reboot() {
  publishMqttStatus(BOOTING);
  ESP.restart();
}

void activateSolenoid(int waitMs) {
  Serial.println("Operating the locker solenoid");

  // Вращаем сервопривод
  digitalWrite(RELAY_PIN1, RELAY_OPEN);
  delay(waitMs); // оставляем замок открытым на 5с

  // Остановка
  digitalWrite(RELAY_PIN1, RELAY_CLOSED);

  Serial.println("End the solenoid operation");
}

// Returns true if mastercard was set, false otherwise
bool checkAndPrintMasterCard() {
  unsigned char masterUID[CARD_SIZE] = {0};
  readMasterCard(masterUID);

  if (!isCardUnset(masterUID)) {
    Serial.println("Retrieving master card from mem");
    printCardUID(masterUID);
    return true;
  }

  return false;
}

void listenForRFID(unsigned char uid[CARD_SIZE]) {
  int index = 0;
  bool isReading = false;
  while (rfidSerial.available() && index < CARD_SIZE) {
    byte c = rfidSerial.read();

    if (c == 0x02) { // Начало пакета
      isReading = true;
    } else if (c == 0x03) { // Конец пакета
      isReading = false;
    } else if (isReading) {
      uid[index++] = c; // Добавляем символы к строке

      #ifdef DEBUG
      Serial.println("read() function, and the RFID raw data: ");
      for (int i = 0; i < index; ++i) {
          Serial.println();
          Serial.print(uid[i], HEX);
          Serial.print('\t');
      }
      Serial.println();
      #endif
    }

    delay(10); // Стабилизация чтения
  }
}

void processCard(unsigned char cardUID[CARD_SIZE]) {
  if (isCardUnset(cardUID)) {
    memset(cardUID, 0, CARD_SIZE); // Очистить данные
    return;
  }

  if (isCurrentlyOperating(WAITING_FOR_SETUP)) {
    // Записываем первую карту как мастер-карту
    saveMasterCard(cardUID);
    Serial.println("Master card set!");
    printCardUID(cardUID);
    tripleShortBeep();
    setOperationMode(NORMAL);
    return;
  }

  if (isCurrentlyOperating(WAITING_FOR_NEW_CARD)) {
    addAllowedCard(cardUID);
    Serial.println("New additional card was saved!");
    printCardUID(cardUID);
    tripleShortBeep();
    setOperationMode(NORMAL);
    return;
  }

  if (!isCurrentlyOperating(NORMAL)) {
    Serial.println("Unexpected state... stop processing.");
    return;
  }

  if (isCardAllowed(cardUID)) {
    Serial.println("Allowed card detected. Access granted.");
    accessGranted();
    return;
  }

  accessDenied();
}

/*
  High level operators over the locker itself and card access
*/

void openLocker() {
  publishMqttStatus(OPEN);
  doubleShortBeep();

  activateSolenoid(5000); // оставляем соленоид открытым на 5 секунд
  publishMqttStatus(LOCKED);
}

void forceCloseLocker() {
  singleLongBeep();
  // activateSolenoid(50); // проверяем работу соленоида, открываем его на 50мс
  publishMqttStatus(LOCKED);
}

void accessGranted() {
  openLocker();
}

void accessDenied() {
  Serial.println("Unknown card detected. Access denied.");
  singleLongBeep();
}

/*
  RFID cards processing methods
*/

bool isCardUnset(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < CARD_SIZE; i++) {
    unsigned char c = uid[i];
    if (c != 0xFF && c != 0x00) {
      return false;
    }
  }
  return true;
}

// Saves card data to EEPROM perm memory
void saveMasterCard(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < CARD_SIZE; i++) {
    EEPROM.write(MASTER_CARD_ADDR + i, uid[i]);
  }
  EEPROM.commit();
  Serial.println("Master card saved.");
}

void readMasterCard(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < CARD_SIZE; i++) {
    uid[i] = EEPROM.read(MASTER_CARD_ADDR + i);
  }
}

void addAllowedCard(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < MAX_CARDS; i++) {
    int addr = ALLOWED_CARDS_ADDR + i * CARD_SIZE;
    if (EEPROM.read(addr) == 0xFF) { // Пустая ячейка
      for (int j = 0; j < CARD_SIZE; j++) {
        EEPROM.write(addr + j, uid[j]);
      }
      EEPROM.commit();
      Serial.println("Allowed card added.");
      return;
    }
  }
  Serial.println("No space for additional cards.");
}

bool isCardAllowed(unsigned char uid[CARD_SIZE]) {
  unsigned char masterUID[CARD_SIZE] = {0};
  readMasterCard(masterUID);

  // First we check whether this card is masterCard
  if (compareUID(uid, masterUID)) {
      return true;
    }

  for (int i = 0; i < MAX_CARDS; i++) {
    int addr = ALLOWED_CARDS_ADDR + i * CARD_SIZE;
    unsigned char storedUID[CARD_SIZE] = {0};
    for (int j = 0; j < CARD_SIZE; j++) {
      storedUID[j] = EEPROM.read(addr + j);
    }

    if (compareUID(uid, storedUID)) {
      return true;
    }
  }

  return false;
}

bool compareUID(unsigned char uid1[CARD_SIZE], unsigned char uid2[CARD_SIZE]) {
  for (int i = 0; i < CARD_SIZE; i++) {
    if (uid1[i] != uid2[i]) {
      return false;
    }
  }

  return true;
}

void printCardUID(unsigned char uid[CARD_SIZE]) {
  Serial.print("Card UID: ");
  for (int i = 0; i < CARD_SIZE; i++) {
    Serial.print(uid[i], HEX);
    if (i < CARD_SIZE - 1) {
      Serial.print(":");
    }
  }
  Serial.println();
}

void setOperationMode(OperatingMode mode) {
  currentMode = mode;
}

bool isCurrentlyOperating(OperatingMode mode) {
  return currentMode == mode;
}

void resetOperatingMode() {
  currentMode = NORMAL;
  publishMqttStatus(LOCKED);
}

/*
  Utility JSON methods
*/

JsonObject createDeviceInfo(JsonDocument& doc) {
  JsonObject device = doc.createNestedObject("device");
  device["identifiers"][0] = "esp8266mod_arch_sketch001";
  device["manufacturer"] = "Arduino";
  device["model"] = "Node MCU";
  device["name"] = "Custom wiskey locker";
  return device;
}

JsonObject createAvailabilityInfo(JsonDocument& doc) {
  JsonObject availability = doc.createNestedObject("availability");
  availability["topic"] = mqtt_topic_lock_state;
  availability["payload_available"] = "locked";
  availability["payload_not_available"] = "offline";
  return availability;
}
