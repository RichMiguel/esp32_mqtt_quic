#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// ===================== KONFIGURASI WIFI & MQTT =====================
// Opsi 1 (Utama - Bebas AP Isolation HP): Hotspot Laptop Windows (2.4 GHz)
const char* WIFI_SSID_LAPTOP = "raspi";
const char* WIFI_PASS_LAPTOP = "raspberry";
const char* BROKER_LAPTOP_AP = "192.168.137.1"; // IP tetap Hotspot Windows

// Opsi 2 (Cadangan): Hotspot HP langsung
const char* WIFI_SSID_HP     = "Negus";
const char* WIFI_PASS_HP     = "1sampai4";
const char* BROKER_HP_WIFI   = "10.174.108.221"; // IP Laptop di Hotspot HP

const char* activeBroker    = BROKER_LAPTOP_AP;
const int   MQTT_PORT       = 1884;                     // Port relay Windows (tcp_relay.py) -> NanoMQ WSL 1885
const char* MQTT_TOPIC_PUB  = "sensor/air_quality";     // Topik publish ke NanoMQ
const char* MQTT_CLIENT_ID  = "ESP32_AirQuality_01";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ===================== KONFIGURASI HARDWARE =====================
#define SDA_PIN             21
#define SCL_PIN             22
#define GP2Y_LED_PIN        27      // Pin IRED (LED) GP2Y1010AU0F (Active LOW)

// Pilihan ADC untuk GP2Y1010AU0F:
// Rekomendasi: Gunakan pin ADC internal ESP32 (GPIO 34) karena timing pulsa 280 us GP2Y
// terlalu cepat untuk ADS1115 (I2C overhead 1-8 ms).
#define USE_INTERNAL_ADC_GP2Y  false
#define GP2Y_ANALOG_PIN        34      // Pin analog Vo GP2Y -> GPIO 34 (ADC1, aman saat WiFi aktif)

Adafruit_ADS1115 ads;
#define CH_MQ135            0       // A0 pada ADS1115 -> MQ-135
#define CH_GP2Y             1       // A1 pada ADS1115 -> Alternatif jika USE_INTERNAL_ADC_GP2Y = false

// ===================== KALIBRASI SENSOR =====================
// MQ-135 (Sesuaikan R0 setelah warm-up di udara bersih)
const float MQ_RL           = 10.0;        // Resistor beban RL (kOhm)
const float MQ_R0_CLEAN     = 9.6;         // Nilai R0 di udara bersih (kOhm)
const float MQ_CURVE_A      = 116.6020682; // Karakteristik gas CO2
const float MQ_CURVE_B      = -2.769034857;

// GP2Y1010AU0F
const float GP2Y_VCC        = 5.0;         // Tegangan kerja sensor
const float GP2Y_SENS       = 0.005;       // Sensitivitas ~5 mV per ug/m3 (0.5V per 100 ug/m3)
const float GP2Y_V_CLEAN    = 0.60;        // Tegangan baseline saat udara bersih (~0.6V - 0.9V)

// ADS1115 Gain ±6.144V -> 1 LSB = 0.1875 mV
const float ADS_LSB_MV      = 0.1875;

// ===================== VARIABEL GLOBAL =====================
float mqVoltage   = 0.0;
float mqRs        = 0.0;
float mqRatio     = 0.0;
float mqPPM       = 0.0;

float gp2yVoltage = 0.0;
float gp2yDensity = 0.0;
int16_t gp2yRaw   = 0;
int16_t mqRaw     = 0;

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 2000; // Kirim data setiap 2 detik
unsigned long lastMqttRetry = 0;

// ===================== DEKLARASI FUNGSI =====================
void setupWiFi();
void reconnectMQTT();
float readGP2YSensor();
float readMQ135Sensor();
const char* kategoriDebu(float d);
const char* kategoriGas(float ppm);
void publishSensorPayload();

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=========================================");
  Serial.println("  IoT Air Quality Monitor (ESP32)");
  Serial.println("  MQ-135 + GP2Y1010AU0F -> NanoMQ (QUIC)");
  Serial.println("=========================================");

  // Inisialisasi Pin GP2Y LED (Active LOW)
  pinMode(GP2Y_LED_PIN, OUTPUT);
  digitalWrite(GP2Y_LED_PIN, HIGH); // Matikan LED di awal

#if USE_INTERNAL_ADC_GP2Y
  pinMode(GP2Y_ANALOG_PIN, INPUT);
  analogReadResolution(12); // Resolusi 12-bit (0-4095)
  Serial.println("[OK] GP2Y1010AU0F menggunakan ADC Internal ESP32 (GPIO 34).");
#else
  Serial.println("[WARN] GP2Y1010AU0F menggunakan ADS1115 A1 (Perhatikan limitasi I2C timing!).");
#endif

  // Inisialisasi I2C & ADS1115
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); // Fast I2C 400 kHz
  if (!ads.begin()) {
    Serial.println("[ERROR] ADS1115 tidak terdeteksi! Periksa wiring I2C (SDA=21, SCL=22).");
    while (1) { delay(500); }
  }
  ads.setGain(GAIN_TWOTHIRDS);              // Rentang ±6.144V
  ads.setDataRate(RATE_ADS1115_860SPS);     // Kecepatan konversi maksimum (860 SPS)
  Serial.println("[OK] ADS1115 terdeteksi (Gain ±6.144V, 860 SPS).");

  // Inisialisasi WiFi & MQTT
  setupWiFi();
  mqttClient.setServer(activeBroker, MQTT_PORT);
  mqttClient.setBufferSize(512); // Buffer cukup untuk payload JSON

  // Warm-up MQ-135 (Singkat 10 detik untuk testing, tingkatkan jika diperlukan)
  Serial.println("[INFO] Warm-up MQ-135 sensor...");
  for (int i = 10; i > 0; i--) {
    Serial.printf("  Warm-up: %d detik...\r", i);
    delay(1000);
  }
  Serial.println("\n[OK] Sensor siap digunakan.\n");
}

// ===================== SETUP WIFI =====================
bool connectToAP(const char* ssid, const char* pass, const char* brokerIp) {
  Serial.printf("[WiFi] Mencoba SSID: %s ", ssid);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(ssid, pass);

  for (int i = 0; i < 16 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    activeBroker = brokerIp;
    mqttClient.setServer(activeBroker, MQTT_PORT);
    Serial.printf("\n[WiFi] Terhubung ke %s!\n", ssid);
    Serial.printf("[WiFi] IP ESP32 : %s | Gateway: %s | Target Broker: %s:%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  activeBroker, MQTT_PORT);
    return true;
  }
  Serial.println(" gagal.");
  return false;
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  // 1. Prioritas utama: Hotspot Laptop Windows (2.4 GHz, bebas AP Isolation HP)
  if (connectToAP(WIFI_SSID_LAPTOP, WIFI_PASS_LAPTOP, BROKER_LAPTOP_AP)) return;
  // 2. Fallback: Hotspot HP langsung
  if (connectToAP(WIFI_SSID_HP, WIFI_PASS_HP, BROKER_HP_WIFI)) return;
  Serial.println("[WiFi] Belum terhubung. Akan dicoba ulang di loop.");
}

// ===================== RECONNECT MQTT =====================
void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry >= 5000) { // Retry setiap 5 detik
      lastMqttRetry = now;
      Serial.printf("[MQTT] Menghubungkan ke NanoMQ (%s:%d)... ", activeBroker, MQTT_PORT);

      if (mqttClient.connect(MQTT_CLIENT_ID)) {
        Serial.println("BERHASIL!");
      } else {
        Serial.printf("GAGAL (rc=%d). Coba lagi 5 detik.\n", mqttClient.state());
      }
    }
  }
}

// ===================== BACA GP2Y1010AU0F =====================
float readGP2YSensor() {
  // Pulsa LED GP2Y:
  // 1. LED ON (LOW)
  // 2. Tunggu 280 us hingga pantulan partikel stabil
  // 3. Baca sampling ADC
  // 4. Tunggu 40 us (total ON = 320 us)
  // 5. LED OFF (HIGH)
  // 6. Tunggu sisa siklus ~9680 us (total siklus = 10 ms)
  
  digitalWrite(GP2Y_LED_PIN, LOW); // LED ON
  delayMicroseconds(280);

#if USE_INTERNAL_ADC_GP2Y
  gp2yRaw = analogRead(GP2Y_ANALOG_PIN);
  delayMicroseconds(40);
  digitalWrite(GP2Y_LED_PIN, HIGH); // LED OFF
  delayMicroseconds(9680);

  // ESP32 ADC: 0-4095 -> 0-3.3V
  float volt = (gp2yRaw / 4095.0) * 3.3;
#else
  gp2yRaw = ads.readADC_SingleEnded(CH_GP2Y);
  delayMicroseconds(40);
  digitalWrite(GP2Y_LED_PIN, HIGH); // LED OFF
  delayMicroseconds(9680);

  float volt = (gp2yRaw * ADS_LSB_MV) / 1000.0;
#endif

  return volt;
}

// ===================== BACA MQ-135 =====================
float readMQ135Sensor() {
  mqRaw = ads.readADC_SingleEnded(CH_MQ135);
  float volt = (mqRaw * ADS_LSB_MV) / 1000.0;
  return volt;
}

// ===================== PUBLISH PAYLOAD KE MQTT =====================
void publishSensorPayload() {
  char jsonPayload[320];

  snprintf(jsonPayload, sizeof(jsonPayload),
    "{\"device\":\"%s\",\"dust\":{\"ug_m3\":%.2f,\"volt\":%.3f,\"status\":\"%s\"},"
    "\"mq135\":{\"co2_ppm\":%.1f,\"rs_kohm\":%.2f,\"status\":\"%s\"},\"uptime\":%lu}",
    MQTT_CLIENT_ID,
    gp2yDensity,
    gp2yVoltage,
    kategoriDebu(gp2yDensity),
    mqPPM,
    mqRs,
    kategoriGas(mqPPM),
    millis() / 1000
  );

  if (mqttClient.connected()) {
    bool published = mqttClient.publish(MQTT_TOPIC_PUB, jsonPayload);
    if (published) {
      Serial.printf("[MQTT PUB] -> %s: %s\n", MQTT_TOPIC_PUB, jsonPayload);
    } else {
      Serial.println("[MQTT PUB] Gagal mengirim data.");
    }
  } else {
    Serial.println("[MQTT] Belum terhubung ke broker NanoMQ.");
  }
}

// ===================== LOOP UTAMA =====================
void loop() {
  // Jaga koneksi WiFi & MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    } else {
      mqttClient.loop();
    }
  } else {
    static unsigned long lastWifiRetry = 0;
    if (millis() - lastWifiRetry >= 8000) {
      lastWifiRetry = millis();
      Serial.println("[WiFi] Terputus! Mencoba menyambung kembali...");
      setupWiFi();
    }
  }

  // Pengambilan data dan pengiriman berkala
  if (millis() - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = millis();

    // 1. Baca Sensor GP2Y1010AU0F (Rata-rata 5 sampel)
    float gp2ySum = 0;
    for (int i = 0; i < 5; i++) {
      gp2ySum += readGP2YSensor();
    }
    gp2yVoltage = gp2ySum / 5.0;

    // Hitung densitas debu (ug/m3)
    // Rumus linear: (Vout - Vclean) / SENS
    if (gp2yVoltage > GP2Y_V_CLEAN) {
      gp2yDensity = (gp2yVoltage - GP2Y_V_CLEAN) / GP2Y_SENS;
    } else {
      gp2yDensity = 0.0;
    }

    // 2. Baca Sensor MQ-135
    mqVoltage = readMQ135Sensor();
    if (mqVoltage > 0.001 && mqVoltage < 5.0) {
      mqRs    = ((5.0 - mqVoltage) / mqVoltage) * MQ_RL;
      mqRatio = mqRs / MQ_R0_CLEAN;
      mqPPM   = MQ_CURVE_A * pow(mqRatio, MQ_CURVE_B);
    } else {
      mqRs = mqRatio = mqPPM = 0;
    }

    // 3. Cetak ke Serial Monitor
    Serial.println("-----------------------------------------");
    Serial.printf("[GP2Y] Volt: %.3f V | Debu: %.2f ug/m3 (%s)\n",
                  gp2yVoltage, gp2yDensity, kategoriDebu(gp2yDensity));
    Serial.printf("[MQ135] Volt: %.3f V | Rs: %.2f kOhm | CO2: %.1f ppm (%s)\n",
                  mqVoltage, mqRs, mqPPM, kategoriGas(mqPPM));

    // 4. Kirim ke NanoMQ via MQTT
    publishSensorPayload();
  }
}

// ===================== KATEGORI =====================
const char* kategoriDebu(float d) {
  if (d < 35.0)   return "BAIK";
  if (d < 75.0)   return "SEDANG";
  if (d < 150.0)  return "TIDAK SEHAT";
  if (d < 250.0)  return "SANGAT TIDAK SEHAT";
  return "BERBAHAYA";
}

const char* kategoriGas(float ppm) {
  if (ppm < 400.0)  return "BAIK";
  if (ppm < 1000.0) return "SEDANG";
  if (ppm < 2000.0) return "BURUK";
  return "SANGAT BURUK";
}
