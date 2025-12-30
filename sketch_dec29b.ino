#include <WiFiS3.h>
#include <DHT.h>
#include <ArduinoBLE.h>
#include <Air_Quality_Monitor_inferencing.h>

#define EDGE_AI_ENABLED true  

// CONFIG

const char* ssid = "INTERNET NAME";
const char* password = "INTERNET PASSWORD";
const char* server = "api.thingspeak.com";
String apiKey = "3L0VR3BII73LWNS9";

#define DHTPIN 2
#define DHTTYPE DHT11
#define MQ2PIN A0

const long uploadInterval = 20000;

DHT dht(DHTPIN, DHTTYPE);
WiFiClient client;
unsigned long lastUploadTime = 0;

// BLUETOOTH

BLEService airQualityService("19B10000-E8F2-537E-4F6C-D104768A1214");
BLEStringCharacteristic gasLevelChar("19B10001-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify, 20);
BLEStringCharacteristic temperatureChar("19B10002-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify, 20);
BLEStringCharacteristic humidityChar("19B10003-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify, 20);
BLEStringCharacteristic statusChar("19B10004-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify, 50);
BLEStringCharacteristic alertChar("19B10005-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify, 100);

BLEDescriptor gasDescriptor("2901", "Gas Level");
BLEDescriptor tempDescriptor("2901", "Temperature");
BLEDescriptor humidDescriptor("2901", "Humidity");
BLEDescriptor statusDescriptor("2901", "Air Quality Status");
BLEDescriptor alertDescriptor("2901", "Alert Message");

// EDGE AI

#if EDGE_AI_ENABLED
// Buffer to hold sensor readings for ML inference
float features[3];  // gas_level, temperature, humidity

// Run ML inference
String runEdgeAIClassification(int gasValue, float temperature, float humidity) {
  // Prepare features for inference
  features[0] = (float)gasValue;
  features[1] = temperature;
  features[2] = humidity;
  
  // Run classifier
  ei_impulse_result_t result = {0};
  
  // Create signal from features
  signal_t signal;
  signal.total_length = 3;
  signal.get_data = [](size_t offset, size_t length, float *out_ptr) -> int {
    for (size_t i = 0; i < length; i++) {
      out_ptr[i] = features[offset + i];
    }
    return 0;
  };
  
  // Run inference
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  
  if (res != EI_IMPULSE_OK) {
    return "ERROR";
  }
  
  // Find the classification with highest confidence
  float max_confidence = 0;
  String predicted_class = "unknown";
  
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    if (result.classification[i].value > max_confidence) {
      max_confidence = result.classification[i].value;
      predicted_class = result.classification[i].label;
    }
  }
  
  return predicted_class;
}
#endif

// Simple threshold-based classification (fallback)
String runThresholdClassification(int gasValue) {
  if (gasValue < 300) {
    return "good";
  } else if (gasValue < 500) {
    return "moderate";
  } else if (gasValue < 700) {
    return "poor";
  } else {
    return "dangerous";
  }
}

// SETUP

void setup() {
  Serial.begin(9600);
  
  while (!Serial) {
    delay(100);
  }
  
  Serial.println("========================================");
  Serial.println("  Smart Air Safety Monitor");
  Serial.println("  IoT + Edge AI + M2M");
  Serial.println("========================================");
  Serial.println();
  
  dht.begin();
  Serial.println("✓ DHT11 sensor initialized");
  Serial.println("✓ MQ-2 sensor initialized");
  
#if EDGE_AI_ENABLED
  Serial.println("✓ Edge AI model loaded");
  Serial.print("  Model: ");
  Serial.println(EI_CLASSIFIER_PROJECT_NAME);
  Serial.print("  Inference time: ~");
  Serial.print(EI_CLASSIFIER_INTERVAL_MS);
  Serial.println("ms");
#else
  Serial.println("⚠ Edge AI not enabled (using threshold classification)");
  Serial.println("  To enable: Add Edge Impulse library include at top of code");
#endif
  
  Serial.println();
  
  // Initialize Bluetooth
  if (!BLE.begin()) {
    Serial.println("✗ Bluetooth failed!");
    while (1);
  }
  
  BLE.setLocalName("AirQualityMonitor");
  BLE.setAdvertisedService(airQualityService);
  
  gasLevelChar.addDescriptor(gasDescriptor);
  temperatureChar.addDescriptor(tempDescriptor);
  humidityChar.addDescriptor(humidDescriptor);
  statusChar.addDescriptor(statusDescriptor);
  alertChar.addDescriptor(alertDescriptor);
  
  airQualityService.addCharacteristic(gasLevelChar);
  airQualityService.addCharacteristic(temperatureChar);
  airQualityService.addCharacteristic(humidityChar);
  airQualityService.addCharacteristic(statusChar);
  airQualityService.addCharacteristic(alertChar);
  
  BLE.addService(airQualityService);
  
  gasLevelChar.writeValue("Initializing...");
  temperatureChar.writeValue("Initializing...");
  humidityChar.writeValue("Initializing...");
  statusChar.writeValue("STARTING");
  alertChar.writeValue("System starting up...");
  
  BLE.advertise();
  
  Serial.println("✓ Bluetooth M2M initialized");
  Serial.println();
  
  connectToWiFi();
  
  Serial.println();
  Serial.println("System ready! Starting monitoring...");
  Serial.println("========================================");
  Serial.println();
}

// MAIN LOOP

void loop() {
  BLEDevice central = BLE.central();
  
  if (central) {
    Serial.print("📱 BLE Connected: ");
    Serial.println(central.address());
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected! Reconnecting...");
    connectToWiFi();
  }
  
  // Read sensors
  int gasValue = analogRead(MQ2PIN);
  float gasVoltage = gasValue * (5.0 / 1023.0);
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();
  
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("⚠ DHT11 read error, skipping");
    delay(5000);
    return;
  }
  
  // EDGE AI CLASSIFICATION
  
  String aiPrediction;
  String classificationMethod;
  
#if EDGE_AI_ENABLED
  // Use Edge AI model
  aiPrediction = runEdgeAIClassification(gasValue, temperature, humidity);
  classificationMethod = "Edge AI";
#else
  // Use simple threshold
  aiPrediction = runThresholdClassification(gasValue);
  classificationMethod = "Threshold";
#endif
  
  // DETERMINE STATUS
  
  int airQualityStatus;
  String airQualityText;
  String statusText;
  String alertMessage;
  
  // Convert AI prediction to status
  if (aiPrediction == "good") {
    airQualityStatus = 0;
    airQualityText = "GOOD ✓";
    statusText = "GOOD";
    alertMessage = "Air quality is GOOD. No action needed.";
  } else if (aiPrediction == "moderate") {
    airQualityStatus = 1;
    airQualityText = "MODERATE ⚠";
    statusText = "MODERATE";
    alertMessage = "Air quality MODERATE. Monitor the situation.";
  } else if (aiPrediction == "poor") {
    airQualityStatus = 2;
    airQualityText = "POOR ⚠⚠";
    statusText = "POOR";
    alertMessage = "WARNING: Poor air quality! Consider ventilation.";
  } else {  // dangerous or unknown
    airQualityStatus = 3;
    airQualityText = "DANGEROUS ✗✗✗";
    statusText = "DANGEROUS";
    alertMessage = "DANGER! High gas levels! Ventilate immediately!";
  }
  
  // UPDATE BLUETOOTH
  
  String gasString = String(gasValue) + " ppm";
  String tempString = String(temperature, 1) + " C";
  String humidString = String(humidity, 1) + " %";
  
  gasLevelChar.writeValue(gasString);
  temperatureChar.writeValue(tempString);
  humidityChar.writeValue(humidString);
  statusChar.writeValue(statusText);
  alertChar.writeValue(alertMessage);
  
  // DISPLAY STATUS SERIAL
  
  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│       SENSOR READINGS               │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.print("│ Gas Level:   ");
  Serial.print(gasValue);
  Serial.print(" (");
  Serial.print(gasVoltage, 2);
  Serial.println("V)       │");
  Serial.print("│ Temperature: ");
  Serial.print(temperature, 1);
  Serial.println(" °C            │");
  Serial.print("│ Humidity:    ");
  Serial.print(humidity, 1);
  Serial.println(" %              │");
  Serial.println("├─────────────────────────────────────┤");
  
  // Show classification method and result
  Serial.print("│ Method: ");
  Serial.print(classificationMethod);
  for(int i = classificationMethod.length(); i < 26; i++) {
    Serial.print(" ");
  }
  Serial.println("│");
  
  Serial.print("│ AI Prediction: ");
  Serial.print(aiPrediction);
  for(int i = aiPrediction.length(); i < 18; i++) {
    Serial.print(" ");
  }
  Serial.println("│");
  
  Serial.print("│ Air Quality: ");
  Serial.print(airQualityText);
  for(int i = airQualityText.length(); i < 20; i++) {
    Serial.print(" ");
  }
  Serial.println("│");
  Serial.println("└─────────────────────────────────────┘");
  
  if (central && central.connected()) {
    Serial.println("📱 M2M: BLE device connected");
  } else {
    Serial.println("📡 M2M: BLE advertising");
  }
  
  // Upload to ThingSpeak
  unsigned long currentTime = millis();
  if (currentTime - lastUploadTime >= uploadInterval) {
    Serial.println();
    Serial.print("📤 Uploading to ThingSpeak... ");
    
    if (uploadToThingSpeak(gasValue, temperature, humidity, airQualityStatus)) {
      Serial.println("SUCCESS ✓");
    } else {
      Serial.println("FAILED ✗");
    }
    
    lastUploadTime = currentTime;
  } else {
    unsigned long timeRemaining = (uploadInterval - (currentTime - lastUploadTime)) / 1000;
    Serial.print("⏱ Next upload in: ");
    Serial.print(timeRemaining);
    Serial.println(" seconds");
  }
  
  Serial.println();
  delay(5000);
  
  if (central && !central.connected()) {
    Serial.println("📱 BLE Disconnected");
  }
}

void connectToWiFi() {
  Serial.print("🌐 Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("✓ WiFi connected!");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("  Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println();
    Serial.println("✗ WiFi failed!");
    delay(10000);
    connectToWiFi();
  }
}

bool uploadToThingSpeak(int gasValue, float temperature, float humidity, int airQualityStatus) {
  if (client.connect(server, 80)) {
    String postStr = apiKey;
    postStr += "&field1=";
    postStr += String(gasValue);
    postStr += "&field2=";
    postStr += String(temperature, 1);
    postStr += "&field3=";
    postStr += String(humidity, 1);
    postStr += "&field4=";
    postStr += String(airQualityStatus);
    postStr += "\r\n\r\n";
    
    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + apiKey + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(postStr.length());
    client.print("\n\n");
    client.print(postStr);
    
    delay(1000);
    
    while (client.available()) {
      char c = client.read();
    }
    
    client.stop();
    return true;
  } else {
    Serial.println("ThingSpeak connection failed!");
    client.stop();
    return false;
  }
}
