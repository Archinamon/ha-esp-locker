#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <Ticker.h>

#define DEFAULT_BAUD_RATE 9600

#define RFID_RX     D5     // Connect the RFID module's TX to D5 pin (GPIO14)
#define RFID_TX     D6     // Connect the RFID module's RX to D6 pin (GPIO12)
#define BUZZER_PIN  D7     // Buzzer KY-012 pin
#define RELAY_PIN1  D8     // Relay pin to operate the solenoid
#define EEPROM_SIZE 64     // Memory EEPROM size

// Cards settings
#define CARD_SIZE           5 // Size of UID card
#define MASTER_CARD_ADDR    0 // Master-card address
#define ALLOWED_CARDS_ADDR 10 // Additional cards address
#define MAX_CARDS           5 // Limit of additional cards

// Emergency reboot timeout
#define RESTART_LIMIT 60000

// Open/closethe relay on NO-channel
#define RELAY_OPEN   1
#define RELAY_CLOSED 0

// Wi-Fi settings
const char* ssid     = "Your_WiFi_SSID";
const char* password = "passw0rd123";

// MQTT settings
const int   mqtt_json_buffer_size = 1024;              // Extended buffer for mostly all of json's
const char* mqtt_server           = "192.168.1.5";     // IP-address of your MQTT-broker (e.g., Home Assistant)
const int   mqtt_port             = 1883;              // Default port of MQTT-broker
const char* mqtt_user             = "esp_locker";      // Username of MQTT-client (if you need it)
const char* mqtt_password         = "8622esphomelock"; // Pass of MQTT-client (if you need it)
const char* mqtt_topic_lock_op    = "home/lock/op";    // Lock operations topic
const char* mqtt_topic_lock_state = "home/lock/state"; // Sync statuses topic
const char* mqtt_topic_lock_logs  = "home/lock/logs";  // Debug logs topic (be careful to use only in trusted network)

const char* mqtt_topic_locker_discovery       = "homeassistant/lock/whiskey_locker/config";                         // The device discovery topic
const char* mqtt_topic_btn_reboot_discovery   = "homeassistant/button/whiskey_locker/whiskey_locker_reboot/config"; // Reboot button discovery topic
const char* mqtt_topic_btn_add_card_discovery = "homeassistant/button/whiskey_locker/whiskey_locker_setup/config";  // Add-new-card button discovery topic

// Wi-Fi params & mode
bool wifiConnected = false;
enum OperatingMode {
  NORMAL,
  CONNECTING,
  WAITING_FOR_SETUP,
  WAITING_FOR_NEW_CARD
};

// Current status in HomeAssistant
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

// Configs...
void setupOTAUpdate();
void prepareRFID();
void prepareSolenoid();
void setupWatchdog();
void setupHomeAssistant();

// Network
void taskEstablishWiFiConnection();
void taskSubscribeToMQTTBroker();

// MQTT-broker
void mqttMainLooper();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttProceedOnTopicMessageReceived(String msg);
void publishMqttStatus(LockStatus status);
void startMqttDiscovery();

// Common methods for locker & cards processing
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
String printCardUID(unsigned char uid[CARD_SIZE]);

// Internal locker state
void setOperationMode(OperatingMode mode);
bool isCurrentlyOperating(OperatingMode mode);
void resetOperatingMode();

// Utilities for JSON & HomeAssistant
JsonObject createDeviceInfo(JsonDocument& doc);
JsonObject createAvailabilityInfo(JsonDocument& doc);

// RFID antenna serial input
SoftwareSerial rfidSerial(RFID_RX, RFID_TX);

// Home Assistant
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// OTA updates http-server
ESP8266WebServer httpServer(1488);
ESP8266HTTPUpdateServer httpUpdater;

// Current locker state
OperatingMode currentMode = NORMAL;
Ticker timer;

// Вебсервер для обновления
ESP8266WebServer httpServer(1488);
ESP8266HTTPUpdateServer httpUpdater;

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

  httpServer.handleClient();

  if (rfidSerial.available()) {
    unsigned char cardUID[CARD_SIZE] = {0};
    listenForRFID(cardUID);
    processCard(cardUID);
  }

  mqttMainLooper();

  // update watchdog
  ESP.wdtFeed();
}

void setupOTAUpdate() {
  httpUpdater.setup(&httpServer, "/firmware", "arkhiotika", password);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  publishDebugLogs("HTTP Update Server ready");
}

void handleNotFound() {
  httpServer.send(404, "text/plain", "GO FUCK YOURSELF MOTHERFUCKER!");
}

void prepareRFID() {
  rfidSerial.begin(DEFAULT_BAUD_RATE);
  EEPROM.begin(EEPROM_SIZE);
  
  checkAndPrintMasterCard();
  publishDebugLogs("RFID System ready to interact");
}

void prepareSolenoid() {
  pinMode(RELAY_PIN1, OUTPUT);
}

void setupWatchdog() {
  ESP.wdtEnable(RESTART_LIMIT);
  publishDebugLogs("Emergency restart watchdog has been attached");
}

void setupHomeAssistant() {
  if (!isCurrentlyOperating(CONNECTING)) {
    publishDebugLogs("ERROR! This method should be called only in Connecting mode!");
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
    publishDebugLogs("Wi-Fi state: connected!");
    return;
  }

  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected!");
    Serial.print("Current IP: ");
    Serial.println(WiFi.localIP());

    wifiConnected = true;

    setupOTAUpdate();
  } else {
    publishDebugLogs("\nCoudn't connect to Wi-Fi. Trying again.");
    delay(2000);
  }
}

void taskSubscribeToMQTTBroker() {
  while (!wifiConnected) {
    publishDebugLogs("Awaiting Wi-FI...");
    delay(1000);
    return;
  }

  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    if (mqttClient.connect("ESP8266Client", mqtt_user, mqtt_password)) {
      publishDebugLogs("Connection established!");

      mqttClient.subscribe(mqtt_topic_lock_op);
      publishMqttStatus(BOOTING);
      forceCloseLocker();
    } else {
      Serial.print("Error, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" Trying again in 5 seconds");
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char) payload[i];
  }

  // Operating the locker
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
  publishDebugLogs(String("Got message: " + msg).c_str());

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
    publishDebugLogs("Wait to setup new safe card");

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
  publishDebugLogs("The status has been sent: ");
  publishDebugLogs(LockStatusStrings[status]);
}

void startMqttDiscovery() {
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

  // The device data itself
  char lockPayload[512];
  serializeJson(lockDoc, lockPayload);
  mqttClient.publish(mqtt_topic_locker_discovery, lockPayload);

  // Reboot buton
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

  // Send the Reboot button data
  char rebootPayload[512];
  serializeJson(rebootDoc, rebootPayload);
  mqttClient.publish(mqtt_topic_btn_reboot_discovery, rebootPayload);

  // Add a new card button
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

  // Send the Add a new card button data
  char setupPayload[512];
  serializeJson(setupDoc, setupPayload);
  mqttClient.publish(mqtt_topic_btn_add_card_discovery, setupPayload);
  publishDebugLogs("[MQTT] The discovery config has been sent!");
}

void reboot() {
  publishMqttStatus(BOOTING);
  ESP.restart();
}

void activateSolenoid(int waitMs) {
  publishDebugLogs("Operating the locker solenoid");

  // Close the relay for solenoid to open th elocker
  digitalWrite(RELAY_PIN1, RELAY_OPEN);
  delay(waitMs); // оставляем замок открытым на 5с

  // Open the relay
  digitalWrite(RELAY_PIN1, RELAY_CLOSED);

  publishDebugLogs("End the solenoid operation");
}

// Returns true if mastercard was set, false otherwise
bool checkAndPrintMasterCard() {
  unsigned char masterUID[CARD_SIZE] = {0};
  readMasterCard(masterUID);

  if (!isCardUnset(masterUID)) {
    publishDebugLogs("Retrieving master card from mem");
    publishDebugLogs(printCardUID(masterUID).c_str());
    return true;
  }

  return false;
}

void listenForRFID(unsigned char uid[CARD_SIZE]) {
  int index = 0;
  bool isReading = false;
  while (rfidSerial.available() && index < CARD_SIZE) {
    byte c = rfidSerial.read();

    if (c == 0x02) { // Batch start
      isReading = true;
    } else if (c == 0x03) { // End of batch
      isReading = false;
    } else if (isReading) {
      uid[index++] = c; // adding symbols...

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

    delay(10); // Stabilize reading
  }
}

void processCard(unsigned char cardUID[CARD_SIZE]) {
  if (isCardUnset(cardUID)) {
    memset(cardUID, 0, CARD_SIZE);
    return;
  }

  if (isCurrentlyOperating(WAITING_FOR_SETUP)) {
    saveMasterCard(cardUID);
    publishDebugLogs(printCardUID(cardUID).c_str());
    tripleShortBeep();
    setOperationMode(NORMAL);
    return;
  }

  if (isCurrentlyOperating(WAITING_FOR_NEW_CARD)) {
    addAllowedCard(cardUID);
    publishDebugLogs(printCardUID(cardUID).c_str());
    tripleShortBeep();
    setOperationMode(NORMAL);
    return;
  }

  if (!isCurrentlyOperating(NORMAL)) {
    publishDebugLogs("Unexpected state... stop processing.");
    return;
  }

  if (isCardAllowed(cardUID)) {
    publishDebugLogs("Allowed card detected. Access granted.");
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

  activateSolenoid(5000); // leaving the closed relay for 5 seconds
  publishMqttStatus(LOCKED);
}

void forceCloseLocker() {
  singleLongBeep();
  activateSolenoid(50); // check the solenoid closing the relay for 50ms
  publishMqttStatus(LOCKED);
}

void accessGranted() {
  openLocker();
}

void accessDenied() {
  publishDebugLogs("Unknown card detected. Access denied.");
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
  publishDebugLogs("Master card saved.");
}

void readMasterCard(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < CARD_SIZE; i++) {
    uid[i] = EEPROM.read(MASTER_CARD_ADDR + i);
  }
}

void addAllowedCard(unsigned char uid[CARD_SIZE]) {
  for (int i = 0; i < MAX_CARDS; i++) {
    int addr = ALLOWED_CARDS_ADDR + i * CARD_SIZE;
    if (EEPROM.read(addr) == 0xFF) { // empty mem slot
      for (int j = 0; j < CARD_SIZE; j++) {
        EEPROM.write(addr + j, uid[j]);
      }
      EEPROM.commit();
      publishDebugLogs("Allowed card added.");
      return;
    }
  }
  publishDebugLogs("No space for additional cards.");
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

String printCardUID(unsigned char uid[CARD_SIZE]) {
  String msg = "Card UID: ";
  for (int i = 0; i < CARD_SIZE; i++) {
    msg += String(uid[i]);
    if (i < CARD_SIZE - 1) {
      msg += ":";
    }
  }

  return msg;
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

void publishDebugLogs(const char* message) {
  Serial.println(message);
  mqttClient.publish(mqtt_topic_lock_logs, message);
}
