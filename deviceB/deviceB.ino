/* 
********** CONNECTION ***********
VCC -> + , GND -> -
LDR: Data -> a0
Button: One of them go to D5, another go to -
Buzzer: + -> D7
LED: Long -> D6, Short -> 220 ohm resistor -> -
OLED: SCK -> D1, SDA -> D2
**********************************
*/


#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================= WIFI =================
#define WIFI_SSID YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASS"

// ================= FIREBASE =================
#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "YOUR_DATABASE_URL"

// ================= FIREBASE OBJECTS =================
FirebaseData fbdo;        // for deviceB JSON updates
FirebaseData fbdo_auth;   // for accessApproved write only
FirebaseData fbdo_read;   // for reading warningStatus/warningType from Device A
FirebaseAuth auth;
FirebaseConfig config;


#define BUTTON_PIN    14   // D5
#define LDR_PIN       A0   // Only analog pin on ESP8266

#define WHITE_LED_PIN 12   // D6
#define BUZZER_PIN    13   // D7

#define OLED_SDA      4    // D2
#define OLED_SCL      5    // D1

// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= VARIABLES =================
bool warningStatus = false;
String warningType = "NONE";

bool buttonPressed = false;
bool roomDark = false;
bool whiteLedStatus = false;
bool buzzerStatus = false;

bool buttonHandled = false;

unsigned long lastFirebaseRead = 0;

// Variable for firebase's values
String remoteDoorStatus = "UNKNOWN";
float remoteTemperature = 0;
float remoteHumidity = 0;

int lightValue = 0;
// ESP8266 ADC is 10-bit (0-1023), so threshold scaled down from ESP32's 12-bit (0-4095)
int DARK_THRESHOLD = 375;  // ~equivalent to 1500 on ESP32's 12-bit scale

//String oledMessage = "Remote Ready";
String oledMessage = "";

// ================= TIMING FOR BUZZER =================
unsigned long previousBuzzerMillis = 0;
bool buzzerOutput = false;

// ================= OLED FUNCTION =================
void showOLED(String line1, String line2, String line3, String line4, String line5) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  /*
  display.setCursor(0, 0);
  display.println("Caregiver Remote");

  display.setCursor(0, 18);
  display.println(line1);

  display.setCursor(0, 34);
  display.println(line2);

  display.setCursor(0, 50);
  display.println(line3);
  */

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

// ================= BUZZER PATTERN =================
void updateBuzzerPattern() {
  unsigned long currentMillis = millis();

  if (!warningStatus) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerOutput = false;
    buzzerStatus = false;
    return;
  }

  buzzerStatus = true;

  int interval = 500;

  if (warningType == "UNAUTHORIZED_ACCESS") {
    interval = 150;   // fast beep
  }
  else if (warningType == "HIGH_TEMP") {
    interval = 800;   // slow beep
  }
  else {
    interval = 400;
  }

  if (currentMillis - previousBuzzerMillis >= interval) {
    previousBuzzerMillis = currentMillis;
    buzzerOutput = !buzzerOutput;
    digitalWrite(BUZZER_PIN, buzzerOutput ? HIGH : LOW);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(WHITE_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(WHITE_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  showOLED("Starting...", "Connecting WiFi", "", "", "");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("WiFi Connected");
  Serial.println(WiFi.localIP());

  showOLED("WiFi Connected", WiFi.localIP().toString(), "", "", "");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  delay(1000);
}

// ================= LOOP =================
void loop() {
  // Light sensor — A0 is the only ADC pin on ESP8266, range 0-1023
  lightValue = analogRead(LDR_PIN);

  if (lightValue < DARK_THRESHOLD) {
    roomDark = false;
    whiteLedStatus = false;
  } else {
    roomDark = true;
    whiteLedStatus = true;
  }

  digitalWrite(WHITE_LED_PIN, whiteLedStatus ? HIGH : LOW);

  // Read warning from Firebase - uses dedicated read object
  if (millis() - lastFirebaseRead >= 3000) {
    lastFirebaseRead = millis();
    if (Firebase.RTDB.getBool(&fbdo_read, "/medicineCabinet/system/warningStatus")) {
        warningStatus = fbdo_read.boolData();
    }
    if (Firebase.RTDB.getString(&fbdo_read, "/medicineCabinet/system/warningType")) {
        warningType = fbdo_read.stringData();
    }
    
    if (Firebase.RTDB.getString(&fbdo_read, "/medicineCabinet/deviceA/doorStatus")) {
        remoteDoorStatus = fbdo_read.stringData();
    }
    if (Firebase.RTDB.getFloat(&fbdo_read, "/medicineCabinet/deviceA/temperature")) {
        remoteTemperature = fbdo_read.floatData();
    }
    if (Firebase.RTDB.getFloat(&fbdo_read, "/medicineCabinet/deviceA/humidity")) {
        remoteHumidity = fbdo_read.floatData();
    }
  }

  // Button approval
  buttonPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (buttonPressed && !buttonHandled) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      buttonHandled = true;

      // Uses fbdo_auth exclusively for accessApproved — same as Device A
      Firebase.RTDB.setBool(&fbdo_auth, "/medicineCabinet/system/accessApproved", true);

      oledMessage = "Access Approved";
    }
  }

  // Reset flag only when button is released
  if (digitalRead(BUTTON_PIN) == HIGH) {
    buttonHandled = false;
    buttonPressed = false;
  }

  // OLED message
  if (warningStatus) {
    if (warningType == "HIGH_TEMP") {
      oledMessage = "High Temp Alert";
    }
    else if (warningType == "UNAUTHORIZED_ACCESS") {
      oledMessage = "Unauthorized Alert";
    }
    else {
      oledMessage = "Warning Active";
    }
  }
  else if (roomDark) {
    oledMessage = "Room Dark LED ON";
  }
  else {
    oledMessage = "Remote Ready";
  }

  // Buzzer different pattern
  updateBuzzerPattern();

  // Firebase update — uses updateNodeAsync to avoid overwriting other nodes
  if (Firebase.ready()) {
    FirebaseJson json;
    json.set("/deviceB/buttonStatus", buttonPressed);
    json.set("/deviceB/lightValue", lightValue);
    json.set("/deviceB/roomDark", roomDark);
    json.set("/deviceB/whiteLedStatus", whiteLedStatus);
    json.set("/deviceB/buzzerStatus", buzzerStatus);
    json.set("/deviceB/oledMessage", oledMessage);

    Firebase.RTDB.updateNodeAsync(&fbdo, "/medicineCabinet", &json);
  }

  String lightText = "Light: " + String(lightValue);
  String warningText = warningStatus ? warningType : "Normal";

  /*
  String doorText = ; To see if the door is open
  String tempText = ; Display value of temperature
  String humText = ; Display value of humidity
  String doorMessage = ; For detecting unauthorized access through the door
  String tempMessage = ; For detecting if temp is higher than threshold
  */

  //showOLED(lightText, warningText, oledMessage); 
  // We don'd need light level display on the oled.
  // Change oledMessage.
  //showOLED(doorText, tempText, humText, accessText, tempMessage); (FROM DEVICE A)
  showOLED(
    "Door: " + remoteDoorStatus,
    "Temp: " + String(remoteTemperature) + " C",
    "Hum: " + String(remoteHumidity) + " %",
    "Warn: " + warningType,
    oledMessage
  );


  Serial.println("---------------------");
  Serial.println("Light: " + String(lightValue));
  Serial.println("Room Dark: " + String(roomDark));
  Serial.println("Warning: " + warningType);
  Serial.println("Buzzer: " + String(buzzerStatus));
  Serial.println("OLED: " + oledMessage);

  delay(200);
}
