/* 
********** CONNECTION ***********
VCC -> + , GND -> -
DHT22: DAT -> D27
Door sensor: One of them go to D14, another go to -
RFID: SDA -> D5, SCK -> D18, MOSI -> D23, MISO -> D19, RST -> D4
Buzzer: + -> D13
OLED: SCK -> D22, SDA -> D21
**********************************
*/


#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <MFRC522.h>

// ================= WIFI =================
#define WIFI_SSID "YOUR_WIFI_NAME" //for safety purpose I took the orignal
#define WIFI_PASSWORD "YOUR_WIFI_PASS"

// ================= FIREBASE =================
#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL YOUR_DATABASE_URL"

FirebaseData fbdo;
FirebaseData fbdo_auth;
FirebaseData fbdo_settings;
FirebaseAuth auth;
FirebaseConfig config;

// ================= PINS =================
#define DHTPIN 27
#define DHTTYPE DHT22

#define DOOR_PIN 14
#define BUZZER_PIN 13
#define FAN_PIN 26

#define OLED_SDA 21
#define OLED_SCL 22

#define RFID_SS 5
#define RFID_RST 4

// ================= OBJECTS =================
DHT dht(DHTPIN, DHTTYPE);
MFRC522 rfid(RFID_SS, RFID_RST);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= VARIABLES =================
float temperature = 0;
float humidity = 0;

bool doorOpen = false; // Detect if the door is currently open
bool accessApproved = false;
bool warningStatus = false;
bool buzzerStatus = false;
bool fanStatus = false;
bool doorWasOpened = false; // Detect if the door was opened recently
bool sessionActive = false;
bool justReset = false; // Check if Firebase is recently updated

String warningType = "NONE";
String accessStatus = "NO_CARD";
String tempMessage = "";

float TEMP_LIMIT = 30;
float TEMP_WARN = TEMP_LIMIT - 3;

unsigned long lastRoutineUpdate = 0;
bool prevWarningStatus = false;

unsigned long lastSettingsUpdate = 0;

// RFID ID
String authorizedUID = "F4 54 5B D3";

// ================= SERIAL DISPLAY =================
void showSerial(String doorText, String tempText, String humText, String warnText, String rfidText){
  Serial.println("\n----------------------");
  Serial.println(doorText);
  Serial.println(tempText);
  Serial.println(humText);
  Serial.println(warnText);
  Serial.println(rfidText);
}

// ================= OLED DISPLAY =================
void showOLED(String line1, String line2, String line3, String line4, String line5) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(line1);

  display.setCursor(0, 13);
  display.println(line2);

  display.setCursor(0, 26);
  display.println(line3);

  display.setCursor(0, 39);
  display.println(line4);

  display.setCursor(0, 52);
  display.println(line5);

  display.display();
}

// ================= RFID UID READ =================
String getRFIDUID() {
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += " ";
  }

  uid.toUpperCase();
  return uid;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT); // Change to Relay.

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);
  //digitalWrite(FAN_PIN, LOW); (For using relay module)

  dht.begin();

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  showOLED("Starting...", "Connecting WiFi", "", "", "");

  SPI.begin();
  rfid.PCD_Init();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println("\nWiFi Connected");
  showOLED("WiFi Connected", WiFi.localIP().toString(), "", "", "");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.print("Waiting for Firebase...");
  unsigned long startTime = millis();
  while (!Firebase.ready()) {
    if (millis() - startTime > 10000) {
      Serial.println("Firebase not ready! Check API key or Anonymous Auth.");
      break;
    }
    Serial.print(".");

    delay(500);
  }
  Serial.println("Firebase ready!");

  // Read temp limits from Firebase settings
  if (Firebase.RTDB.getFloat(&fbdo_settings, "/medicineCabinet/settings/tempLimit")) {
    TEMP_LIMIT = fbdo_settings.floatData();
    TEMP_WARN = TEMP_LIMIT - 3;
  }
  

  delay(1000);
}

// ================= LOOP =================
void loop() {
  /* DEBUGGING
  Serial.println("--- LOOP ---");
  Serial.println("sessionActive: " + String(sessionActive));
  Serial.println("accessApproved: " + String(accessApproved));
  Serial.println("doorOpen: " + String(doorOpen));
  Serial.println("doorWasOpened: " + String(doorWasOpened));
  */

  temperature = dht.readTemperature();
  humidity = dht.readHumidity();
  doorOpen = digitalRead(DOOR_PIN) == HIGH;

  // Remote approval - only when no session is active
  if (!sessionActive && !justReset) { // 👈 skip read if just reset
    if (Firebase.RTDB.getBool(&fbdo_auth, "/medicineCabinet/system/accessApproved")) {
        accessApproved = fbdo_auth.boolData();
        if (accessApproved) sessionActive = true;
    }
  } else {
    justReset = false; // 👈 clear flag after one skipped cycle
  }

  // RFID scan
  accessStatus = sessionActive ? "AUTHORIZED" : "NO_TAG";
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String cardUID = getRFIDUID();
    Serial.print("\nCard UID: ");
    Serial.println(cardUID);

    if (cardUID == authorizedUID) {
      accessApproved = true;
      sessionActive = true;
      Firebase.RTDB.setBool(&fbdo_auth, "/medicineCabinet/system/accessApproved", true);
    } else {
      accessApproved = false;
      sessionActive = false;
      Firebase.RTDB.setBool(&fbdo_auth, "/medicineCabinet/system/accessApproved", false);
    }
    rfid.PICC_HaltA();
  }

  // Temperature warning
  if (isnan(temperature) || isnan(humidity)) {
    warningStatus = true;
    warningType = "DHT_ERROR";
    tempMessage = "Temperature sensor error!";
  } else if (temperature >= TEMP_LIMIT) {
    warningStatus = true;
    warningType = "HIGH_TEMP";
    tempMessage = "High temp detected!";
  } else {
    warningStatus = false;
    warningType = "NONE";
    tempMessage = "";
  }

  // Track door open during session
  if (sessionActive && doorOpen) {
    doorWasOpened = true;
  }

  // Reset session only when door closes after having been opened
  if (doorWasOpened && !doorOpen) {
    accessApproved = false;
    sessionActive = false;
    doorWasOpened = false;
    justReset = true;
    Firebase.RTDB.setBool(&fbdo_auth, "/medicineCabinet/system/accessApproved", false);
  }

  // Door warning logic
  if (doorOpen && !accessApproved) {
    accessStatus = "UNAUTHORIZED";
  } else if (doorOpen && accessApproved) {
    accessStatus = "AUTHORIZED";
  }
  if (!warningStatus) {
    if (doorOpen && !accessApproved) {
      warningStatus = true;
      warningType = "UNAUTHORIZED_ACCESS";
    } else if (doorOpen && accessApproved) {
      warningStatus = false;
      warningType = "NONE";
    }
  }

  // Fan control
  fanStatus = (temperature >= TEMP_WARN);
  buzzerStatus = warningStatus;

  digitalWrite(FAN_PIN, fanStatus ? HIGH : LOW);
  //digitalWrite(FAN_PIN, fanStatus ? LOW : HIGH); (For using relay module)
  digitalWrite(BUZZER_PIN, buzzerStatus ? HIGH : LOW);

  String doorText = doorOpen ? "Door: OPEN" : "Door: CLOSED";
  String tempText = "Temp: " + String(temperature) + " C";
  String humText = "Hum: " + String(humidity) + " %";
  String warnText = "Warning: " + String(warningStatus ? "ACTIVE" : "NORMAL");
  String accessText = "Access: " + accessStatus;

  showSerial(doorText, tempText, humText, warnText, accessText);
  showOLED(doorText, tempText, humText, accessText, tempMessage);

  FirebaseJson json;
  json.set("/deviceA/doorStatus", doorOpen ? "OPEN" : "CLOSED");
  json.set("/deviceA/temperature", temperature);
  json.set("/deviceA/humidity", humidity);
  json.set("/deviceA/buzzerStatus", buzzerStatus);
  json.set("/system/warningStatus", warningStatus);
  json.set("/system/warningType", warningType);
  //json.set("/system/accessApproved", accessApproved);

  //Serial.println("Firebase ready: " + String(Firebase.ready()));
  //Serial.println("Time since last update: " + String(millis() - lastRoutineUpdate));

  if (Firebase.ready()) {
    bool sent = false;
    if (warningStatus != prevWarningStatus) {
      prevWarningStatus = warningStatus;
      Firebase.RTDB.updateNodeAsync(&fbdo, "/medicineCabinet", &json);
      sent = true;
    }
    if (millis() - lastRoutineUpdate >= 5000) {
      lastRoutineUpdate = millis();
      Firebase.RTDB.updateNodeAsync(&fbdo, "/medicineCabinet", &json);
      sent = true;
    }
    if (sent) Serial.print("\nFirebase update queued!");
  } else {
    delay(500);
  }

  
  if (Firebase.RTDB.getFloat(&fbdo_settings, "/medicineCabinet/settings/tempLimit")){
    TEMP_LIMIT = fbdo_settings.floatData();
    TEMP_WARN = TEMP_LIMIT - 3;
  }

  delay(300);
}
