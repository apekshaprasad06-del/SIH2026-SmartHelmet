#include <Wire.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include "BluetoothSerial.h"

const int BUTTON_PIN = 13;
const int BUZZER_PIN = 25;
const int MQ9_PIN = 34; 

const String PHONE_NUMBER = "+919819640668"; 
const unsigned long CANCEL_WINDOW_MS = 10000; 
const float CRASH_THRESHOLD = 2.5; 
const int GAS_THRESHOLD = 1500; 
const unsigned long GAS_ALERT_TIMEOUT_MS = 10000; 

TinyGPSPlus gps;
HardwareSerial SerialGPS(1); 
HardwareSerial SerialGSM(2); 
BluetoothSerial SerialBT; 

volatile bool cancelAlarm = false; 
unsigned long lastCrashTime = 0;
bool gasAlertArmed = true; 

float lastAX = 0, lastAY = 0, lastAZ = 0;
float lastGX = 0, lastGY = 0, lastGZ = 0;
float lastForce = 0;

float crashAX = 0, crashAY = 0, crashAZ = 0;
float crashGX = 0, crashGY = 0, crashGZ = 0;
float crashForce = 0;

const float FALLBACK_LAT = -77.8419;
const float FALLBACK_LON = 166.6863;

String buildUnifiedMessage(bool alcohol, bool crash, bool falseAlarm, bool useCrashSnapshot = false);

void IRAM_ATTR buttonISR() {
  cancelAlarm = true; 
}

void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(MQ9_PIN, INPUT); 
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  SerialGPS.begin(9600, SERIAL_8N1, 32, 33); 
  SerialGSM.begin(9600, SERIAL_8N1, 16, 17); 

  SerialBT.begin("Crash_Detector_ESP32");
  Serial.println("Bluetooth started. You can pair your phone now!");

  Wire.begin(21, 22);
  Wire.beginTransmission(0x68);
  Wire.write(0x6B); 
  Wire.write(0x00); 
  if (Wire.endTransmission() != 0) {
    Serial.println("MPU6050 not responding to wake command! Check wires.");
    while(1) delay(10);
  }

  Wire.beginTransmission(0x68);
  Wire.write(0x1C); 
  Wire.write(0x08); 
  Wire.endTransmission();

  Wire.beginTransmission(0x68);
  Wire.write(0x1B);
  Wire.write(0x08);
  Wire.endTransmission();

  Serial.println("System Ready. Monitoring for crashes...");
}

void loop() {
  if (checkForCrash()) { 
    handleCrashSequence();
    waitForReset(); 
    lastCrashTime = millis() - 5000; 
  }

  checkGasAlert(); 

  printGasDebug(); 

  sendLiveUpdate(); 

  delay(20); 
}

unsigned long lastLiveUpdate = 0;
void sendLiveUpdate() {
  if (millis() - lastLiveUpdate >= 1000) {
    lastLiveUpdate = millis();
    String liveMsg = buildUnifiedMessage(false, false, false);
    SerialBT.println(liveMsg);
  }
}

unsigned long lastGasDebugPrint = 0;
void printGasDebug() {
  if (millis() - lastGasDebugPrint >= 1000) {
    lastGasDebugPrint = millis();
    int gasRaw = analogRead(MQ9_PIN);
    String debugLine = "MQ9 raw: " + String(gasRaw) + " (threshold: " + String(GAS_THRESHOLD) + ")";
    Serial.println(debugLine);
  }
}

bool readIMU() {
  bool accelOk = false;
  bool gyroOk = false;

  Wire.beginTransmission(0x68);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom((uint16_t)0x68, (uint8_t)6, true); 

  if (Wire.available() == 6) {
    int16_t ax = (Wire.read() << 8 | Wire.read());
    int16_t ay = (Wire.read() << 8 | Wire.read());
    int16_t az = (Wire.read() << 8 | Wire.read());

    lastAX = ax / 8192.0;
    lastAY = ay / 8192.0;
    lastAZ = az / 8192.0;
    lastForce = sqrt(sq(lastAX) + sq(lastAY) + sq(lastAZ));
    accelOk = true;
  } else {
    Wire.flush(); 
  }

  Wire.beginTransmission(0x68);
  Wire.write(0x43);
  Wire.endTransmission(false);
  Wire.requestFrom((uint16_t)0x68, (uint8_t)6, true);

  if (Wire.available() == 6) {
    int16_t gx = (Wire.read() << 8 | Wire.read());
    int16_t gy = (Wire.read() << 8 | Wire.read());
    int16_t gz = (Wire.read() << 8 | Wire.read());

    lastGX = gx / 65.5;
    lastGY = gy / 65.5;
    lastGZ = gz / 65.5;
    gyroOk = true;
  } else {
    Wire.flush();
  }

  if (!accelOk || !gyroOk) {
    Serial.println("WARNING: IMU read failed this cycle, reusing last known values.");
  }

  return accelOk && gyroOk;
}

bool checkForCrash() {
  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  readIMU();
  float totalGs = lastForce;

  if (totalGs > CRASH_THRESHOLD && millis() - lastCrashTime > 5000) {
    Serial.print("\nCRASH DETECTED! Force: ");
    Serial.print(totalGs);
    Serial.println(" Gs.");

    crashAX = lastAX;
    crashAY = lastAY;
    crashAZ = lastAZ;
    crashGX = lastGX;
    crashGY = lastGY;
    crashGZ = lastGZ;
    crashForce = lastForce;

    return true;
  }
  return false;
}

void checkGasAlert() {
  int gasRaw = analogRead(MQ9_PIN);

  if (gasRaw > GAS_THRESHOLD && gasAlertArmed) {
    gasAlertArmed = false;

    Serial.print("\nALCOHOL/GAS DETECTED! Raw: ");
    Serial.println(gasRaw);

    sendGasSMS(gasRaw);

    cancelAlarm = false;
    unsigned long startTime = millis();
    int tapCount = 0;
    unsigned long firstTapTime = 0;
    bool buttonWasReleased = true;

    Serial.println("Sounding gas alarm. Press button 5 times within 3 seconds to silence.");

    while (millis() - startTime < GAS_ALERT_TIMEOUT_MS) {
      if (checkForCrash()) {
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("Crash detected during gas alert! Handing off to crash sequence.");
        handleCrashSequence();
        waitForReset();
        lastCrashTime = millis() - 5000;
        return;
      }

      digitalWrite(BUZZER_PIN, HIGH); delay(80);
      digitalWrite(BUZZER_PIN, LOW); delay(80);
      digitalWrite(BUZZER_PIN, HIGH); delay(80);
      digitalWrite(BUZZER_PIN, LOW); delay(260);

      if (digitalRead(BUTTON_PIN) == LOW) {
        if (buttonWasReleased) {
          if (tapCount == 0) {
            firstTapTime = millis();
          }
          tapCount++;
          buttonWasReleased = false;
        }
      } else {
        buttonWasReleased = true;
      }

      if (tapCount > 0 && (millis() - firstTapTime > 3000)) {
        tapCount = 0; 
      }

      if (tapCount >= 5 && (millis() - firstTapTime <= 3000)) {
        Serial.println("Button pressed 5x! Gas alarm silenced.");
        break;
      }
    }

    digitalWrite(BUZZER_PIN, LOW);
  }

  if (gasRaw <= GAS_THRESHOLD) {
    gasAlertArmed = true;
  }
}

String buildUnifiedMessage(bool alcohol, bool crash, bool falseAlarm, bool useCrashSnapshot) {
  if (!useCrashSnapshot) {
    readIMU();
  }

  while (SerialGPS.available() > 0) {
    gps.encode(SerialGPS.read());
  }

  float ax = useCrashSnapshot ? crashAX : lastAX;
  float ay = useCrashSnapshot ? crashAY : lastAY;
  float az = useCrashSnapshot ? crashAZ : lastAZ;
  float gx = useCrashSnapshot ? crashGX : lastGX;
  float gy = useCrashSnapshot ? crashGY : lastGY;
  float gz = useCrashSnapshot ? crashGZ : lastGZ;
  float force = useCrashSnapshot ? crashForce : lastForce;

  float lat, lon, speed;

  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    speed = gps.speed.isValid() ? gps.speed.kmph() : 0;
  } else {
    lat = FALLBACK_LAT;
    lon = FALLBACK_LON;
    speed = 0;
  }

  String msg = "ALCOHOL=" + String(alcohol ? "Yes" : "No");
  msg += ",LAT=" + String(lat, 4);
  msg += ",LON=" + String(lon, 4);
  msg += ",SPEED=" + String((int)speed);
  msg += ",AX=" + String(ax, 1);
  msg += ",AY=" + String(ay, 1);
  msg += ",AZ=" + String(az, 1);
  msg += ",GX=" + String(gx, 1);
  msg += ",GY=" + String(gy, 1);
  msg += ",GZ=" + String(gz, 1);
  msg += ",CRASH=" + String(crash ? "1" : "0");
  msg += ",FALSE_ALARM=" + String(falseAlarm ? "1" : "0");
  msg += ",FORCE=" + String(force, 1) + " g";

  return msg;
}

void sendGasSMS(int gasRaw) {
  String finalMessage = buildUnifiedMessage(true, false, false);

  Serial.println("\n--- MESSAGE PREVIEW ---");
  Serial.println(finalMessage);

  SerialBT.println(finalMessage);

  SerialGSM.println("AT+CMGF=1");
  delay(500);
  SerialGSM.print("AT+CMGS=\"");
  SerialGSM.print(PHONE_NUMBER);
  SerialGSM.println("\"");
  delay(500);
  SerialGSM.print(finalMessage);
  delay(500);
  SerialGSM.write(26);
  delay(3000);

  Serial.println("Alcohol Alert SMS Process Completed.");
}

void handleCrashSequence() {
  cancelAlarm = false; 
  unsigned long startTime = millis();
  
  Serial.println("Starting cancellation countdown...");
  
  while (millis() - startTime < CANCEL_WINDOW_MS) {
    if (cancelAlarm) break;
    playSOS();
  }

  digitalWrite(BUZZER_PIN, LOW); 

  if (cancelAlarm) {
    Serial.println("Button pressed! Sending False Alarm SMS...");
    sendSMS(false, true);
  } else {
    Serial.println("Timer expired! Sending Emergency SMS...");
    sendSMS(true, false);
  }
}

void waitForReset() {
  Serial.println("\n--- SYSTEM HALTED ---");
  Serial.println("Press button 3 times within 2 seconds to manually reset.");
  Serial.println("Otherwise, system will auto-reset in 30 seconds...");
  
  int tapCount = 0;
  unsigned long firstTapTime = 0;
  bool buttonWasReleased = true;
  unsigned long haltStartTime = millis(); 

  while(true) {
    while (SerialGPS.available() > 0) {
      gps.encode(SerialGPS.read());
    }

    if (millis() - haltStartTime >= 30000) {
      Serial.println("\nResetting...");
      cancelAlarm = false; 
      digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW); 
      Serial.println("System now reset and able to detect crash");
      return; 
    }

    if (digitalRead(BUTTON_PIN) == LOW) {
      if (buttonWasReleased) {
        if (tapCount == 0) {
          firstTapTime = millis();
        }
        tapCount++;
        buttonWasReleased = false;
        
        digitalWrite(BUZZER_PIN, HIGH); delay(50); digitalWrite(BUZZER_PIN, LOW);
        delay(150); 
      }
    } else {
      buttonWasReleased = true;
    }

    if (tapCount >= 3 && (millis() - firstTapTime <= 2000)) {
      Serial.println("\nResetting...");
      cancelAlarm = false; 
      digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW); 
      Serial.println("System now reset and able to detect crash");
      return; 
    }

    if (tapCount > 0 && (millis() - firstTapTime > 2000)) {
      tapCount = 0; 
    }
  }
}

void playSOS() {
  for (int i = 0; i < 3; i++) {
    if (cancelAlarm) return;
    digitalWrite(BUZZER_PIN, HIGH); delay(100);
    digitalWrite(BUZZER_PIN, LOW); delay(100);
  }
  for (int i = 0; i < 3; i++) {
    if (cancelAlarm) return;
    digitalWrite(BUZZER_PIN, HIGH); delay(300);
    digitalWrite(BUZZER_PIN, LOW); delay(100);
  }
  for (int i = 0; i < 3; i++) {
    if (cancelAlarm) return;
    digitalWrite(BUZZER_PIN, HIGH); delay(100);
    digitalWrite(BUZZER_PIN, LOW); delay(100);
  }
  delay(400); 
}

void sendSMS(bool crash, bool falseAlarm) {
  String finalMessage = buildUnifiedMessage(false, crash, falseAlarm, true); // true = use frozen crash-moment snapshot
  
  Serial.println("\n--- MESSAGE PREVIEW ---");
  Serial.println(finalMessage);
  
  SerialBT.println(finalMessage);

  SerialGSM.println("AT+CMGF=1"); 
  delay(500);
  SerialGSM.print("AT+CMGS=\"");
  SerialGSM.print(PHONE_NUMBER);
  SerialGSM.println("\"");
  delay(500);
  SerialGSM.print(finalMessage); 
  delay(500);
  SerialGSM.write(26); 
  delay(3000); 
  
  Serial.println("SMS Process Completed.");
  digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW); 
}
