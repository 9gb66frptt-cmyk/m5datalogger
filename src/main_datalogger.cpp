/*
 * ═══════════════════════════════════════════════════════════════════
 *  Stérig — Firmware Datalogger RS-232 pour M5Stack S3
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Écoute le port série RS-232 d'un autoclave (via MAX3232) et
 *  transmet les données de cycle via MQTT (HiveMQ Cloud, TLS 8883).
 *
 *  Provisioning identique au scanner QR (QR code depuis iPad Stérig),
 *  avec champs supplémentaires : autoclaveName, autoclaveSerial, baudRate.
 *
 *  Câblage M5Stack CoreS3 + module RS-232 empilable :
 *    // UART1 pour la communication RS-232 via MAX3232
// Module13.2 RS232F — Core S3 : TX=GPIO6, RX=GPIO7, bouton STRAIGHT
// DIP switches : RXD=13 (ON), TXD=12 (ON) → mapping Core→CoreS3 : RX=GPIO7, TX=GPIO6
// Confirmé par test firmware main_rs232_test.cpp (24/04/2026)
 *
 *  Dépendances PlatformIO :
 *    - m5stack/M5Unified
 *    - bblanchon/ArduinoJson
 *    - knolleary/PubSubClient
 *
 *  NVS namespace : "stery-dl" (distinct du scanner "stery")
 * ═══════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// Module QR pour provisioning
#include <M5ModuleQRCode.h>

Preferences prefs;

// ─── Configuration (chargée depuis NVS ou QR provisioning) ───
// Réseau WiFi
String cfgWifiSSID;
String cfgWifiPassword;
// MQTT (même broker HiveMQ que le scanner)
String cfgMqttHost;
int    cfgMqttPort = 8883;
String cfgMqttUser;
String cfgMqttPass;
// Identité
String cfgCabinetId;
String cfgAutoclaveName;       // ex: "STATCLAVE G4 - Salle 1"
String cfgAutoclaveSerial;     // numéro de série autoclave
int    cfgBaudRate = 9600;     // baud rate RS-232 autoclave (défaut 9600)

// ─── Identité auto (MAC) ───
String deviceId;  // "datalogger-A1B2C3" généré depuis la MAC

// ─── Topics MQTT (préfixe datalogger/ au lieu de scanner/) ───
const char* TOPIC_HEARTBEAT = "datalogger/heartbeat";
const char* TOPIC_STATUS    = "datalogger/status";
const char* TOPIC_CYCLE     = "datalogger/data";      // données de cycle complètes
const char* TOPIC_LINE      = "datalogger/line";       // lignes RS-232 en temps réel (optionnel)
String      TOPIC_COMMAND;  // "datalogger/command/<deviceId>" — construit dans setup()

// ─── LWT / status ───
String lwtMessage;
String onlineMessage;

// ─── Hardware MQTT ───
WiFiClientSecure net;
PubSubClient mqtt(net);

// ─── Hardware RS-232 ───
// UART1 pour la communication RS-232 via MAX3232
// Module13.2 RS232F — Core S3 : TX=GPIO6, RX=GPIO7, bouton STRAIGHT
// Confirmé par test firmware main_rs232_test.cpp (24/04/2026)
#define RS232_RX_PIN  7   // GPIO7 — RS232 RX (confirmé)
#define RS232_TX_PIN  6   // GPIO6 — RS232 TX (confirmé)
HardwareSerial RS232Serial(1);  // UART1

// ─── Module QR pour provisioning ───
M5ModuleQRCode module_qrcode;

// ─── Buffer réception série ───
#define SERIAL_BUFFER_SIZE 4096
char serialBuffer[SERIAL_BUFFER_SIZE];
int  serialBufferPos = 0;

// ─── Cycle data accumulator ───
String currentCycleData = "";
bool   cycleInProgress = false;
unsigned long cycleStartTime = 0;
unsigned long lastSerialActivity = 0;

// ─── Timers ───
const unsigned long HEARTBEAT_INTERVAL   = 30000;   // 30s (identique au scanner)
const unsigned long CYCLE_END_TIMEOUT    = 3000;    // 3s sans données = fin de cycle
const unsigned long SCREEN_TIMEOUT       = 30000;   // 30s → veille écran
const unsigned long WIFI_RECONNECT_DELAY = 30000;   // 30s entre tentatives WiFi

bool screenOn = true;
unsigned long lastActivity = 0;
bool provisioned = false;

// ─── Stats ───
unsigned long totalLinesReceived = 0;
unsigned long totalCyclesSent = 0;
unsigned long lastHeartbeat = 0;

// ─── Reprovisioning QR (scan à la demande) ───
bool qrAvailable = false;          // module QR détecté au boot
bool provScanning = false;         // scan QR en cours
unsigned long provScanStart = 0;
const unsigned long PROV_SCAN_TIMEOUT = 15000;  // 15s timeout (plus long que le scanner)

// ─── Réassemblage des fragments QR ───
// La lib M5Module-QRCode (waitResponse timeout_ms=0) livre les gros QR
// (>~250 octets) en plusieurs paquets UART successifs. On accumule jusqu'à
// avoir un JSON complet (accolade fermante finale).
String qrBuffer = "";
unsigned long qrBufferFirstAt = 0;
const unsigned long QR_BUFFER_TIMEOUT = 1500;

enum QRFragResult { QR_PARSED_OK, QR_PARTIAL, QR_INVALID_FORMAT, QR_NOT_PROVISIONING };

// ─── Dernier cycle transmis (volatile, "Aucun" au boot) ───
String lastCycleDisplay = "Aucun";

// ═══════════════════════════════════════════════════════════
// NVS : sauvegarde / chargement de la configuration
// ═══════════════════════════════════════════════════════════

bool loadConfigFromNVS() {
    prefs.begin("stery-dl", true);  // read-only
    bool valid = prefs.getBool("provisioned", false);
    if (valid) {
        cfgWifiSSID        = prefs.getString("wifiSSID", "");
        cfgWifiPassword    = prefs.getString("wifiPass", "");
        cfgMqttHost        = prefs.getString("mqttHost", "");
        cfgMqttPort        = prefs.getInt("mqttPort", 8883);
        cfgMqttUser        = prefs.getString("mqttUser", "");
        cfgMqttPass        = prefs.getString("mqttPass", "");
        cfgCabinetId       = prefs.getString("cabinetId", "");
        cfgAutoclaveName   = prefs.getString("aclName", "");
        cfgAutoclaveSerial = prefs.getString("aclSerial", "");
        cfgBaudRate        = prefs.getInt("baudRate", 9600);
    }
    prefs.end();
    return valid && cfgWifiSSID.length() > 0 && cfgMqttHost.length() > 0;
}

void saveConfigToNVS() {
    prefs.begin("stery-dl", false);  // read-write
    prefs.putBool("provisioned", true);
    prefs.putString("wifiSSID",   cfgWifiSSID);
    prefs.putString("wifiPass",   cfgWifiPassword);
    prefs.putString("mqttHost",   cfgMqttHost);
    prefs.putInt("mqttPort",      cfgMqttPort);
    prefs.putString("mqttUser",   cfgMqttUser);
    prefs.putString("mqttPass",   cfgMqttPass);
    prefs.putString("cabinetId",  cfgCabinetId);
    prefs.putString("aclName",    cfgAutoclaveName);
    prefs.putString("aclSerial",  cfgAutoclaveSerial);
    prefs.putInt("baudRate",      cfgBaudRate);
    prefs.end();
}

void clearNVS() {
    prefs.begin("stery-dl", false);
    prefs.clear();
    prefs.end();
}

// Triple-reset detection (identique au scanner)
bool checkTripleReset() {
    prefs.begin("boot-dl", false);
    uint32_t cnt = prefs.getUInt("cnt", 0) + 1;
    prefs.putUInt("cnt", cnt);
    prefs.end();
    Serial.printf("[BOOT] reset count = %u\n", cnt);
    return cnt >= 3;
}

void clearResetCounter() {
    prefs.begin("boot-dl", false);
    prefs.putUInt("cnt", 0);
    prefs.end();
}

// ═══════════════════════════════════════════════════════════
// QR Provisioning : parse le JSON du QR iPad (format datalogger)
// ═══════════════════════════════════════════════════════════

bool parseProvisioningQR(const String& qrContent) {
    // Accepte deux formats :
    //   1. JSON brut : {"t":"datalogger", "w":"...", ...}
    //   2. Préfixé   : PROV:{"t":"datalogger", "w":"...", ...}
    //
    // Clés courtes :
    //   "t": "datalogger",          // type (pour distinguer du scanner)
    //   "w": "wifiSSID",
    //   "wp": "wifiPassword",
    //   "h": "mqttHost",            // même clé que le scanner
    //   "p": 8883,                  // même clé que le scanner
    //   "u": "mqttUser",            // même clé que le scanner
    //   "mp": "mqttPass",           // même clé que le scanner
    //   "c": "cabinetId",
    //   "an": "autoclaveName",
    //   "as": "autoclaveSerial",
    //   "br": 9600                  // baud rate

    // Strip "PROV:" prefix if present
    String jsonStr = qrContent;
    if (jsonStr.startsWith("PROV:")) {
        jsonStr = jsonStr.substring(5);
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err) {
        Serial.printf("[PROV] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Vérifier le type
    String type = doc["t"] | "";
    if (type != "datalogger") {
        Serial.println("[PROV] Type invalide (attendu: datalogger)");
        return false;
    }

    // Champs obligatoires
    if (!doc["w"].is<const char*>() ||
        !doc["h"].is<const char*>() ||
        !doc["u"].is<const char*>() ||
        !doc["mp"].is<const char*>()) {
        Serial.println("[PROV] Champs obligatoires manquants (w, h, u, mp)");
        return false;
    }

    cfgWifiSSID        = doc["w"].as<String>();
    cfgWifiPassword    = doc["wp"] | "";
    cfgMqttHost        = doc["h"].as<String>();
    cfgMqttPort        = doc["p"] | 8883;
    cfgMqttUser        = doc["u"].as<String>();
    cfgMqttPass        = doc["mp"].as<String>();
    cfgCabinetId       = doc["c"] | "";
    cfgAutoclaveName   = doc["an"] | "";
    cfgAutoclaveSerial = doc["as"] | "";
    cfgBaudRate        = doc["br"] | 9600;

    return true;
}

// Accumule un fragment QR reçu du module et tente de parser quand le JSON
// est complet. Gère le cas où la lib livre un gros QR en plusieurs paquets UART.
QRFragResult consumeQRFragment(const String& fragment) {
    if (qrBuffer.length() > 0 && millis() - qrBufferFirstAt > QR_BUFFER_TIMEOUT) {
        Serial.println("[PROV] Buffer QR timeout, purge");
        qrBuffer = "";
    }
    if (qrBuffer.length() == 0) qrBufferFirstAt = millis();
    qrBuffer += fragment;

    String s = qrBuffer;
    s.trim();

    bool looksLikeProv = s.startsWith("{") || s.startsWith("PROV:{");
    if (!looksLikeProv) {
        qrBuffer = "";
        return QR_NOT_PROVISIONING;
    }

    if (!s.endsWith("}")) {
        Serial.printf("[PROV] Fragment partiel (%d octets), attente suite\n", qrBuffer.length());
        return QR_PARTIAL;
    }

    Serial.printf("[PROV] JSON reassemble (%d octets), parsing...\n", qrBuffer.length());
    String full = qrBuffer;
    qrBuffer = "";
    return parseProvisioningQR(full) ? QR_PARSED_OK : QR_INVALID_FORMAT;
}

// ═══════════════════════════════════════════════════════════
// Mode provisioning : attend un QR depuis le module scanner
// ═══════════════════════════════════════════════════════════

void enterProvisioningMode() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setCursor(5, 10);
    M5.Display.println("DATALOGGER");
    M5.Display.println("PROVISIONING");
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.println("");
    M5.Display.println("Scannez le QR code");
    M5.Display.println("depuis l'iPad Stery");
    M5.Display.println("");
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.printf("ID: %s\n", deviceId.c_str());

    Serial.println("[PROV] En attente de QR provisioning datalogger...");

    module_qrcode.startDecode();

    while (true) {
        M5.update();
        module_qrcode.update();

        if (module_qrcode.available()) {
            auto result = module_qrcode.getScanResult();
            String qrContent = result.c_str();
            Serial.printf("[PROV] Fragment recu (%d octets)\n", qrContent.length());

            QRFragResult r = consumeQRFragment(qrContent);
            if (r == QR_PARSED_OK) {
                module_qrcode.stopDecode();

                M5.Speaker.setVolume(50);
                M5.Speaker.tone(1000, 100);
                delay(100);
                M5.Speaker.tone(1500, 100);

                M5.Display.fillScreen(TFT_BLACK);
                M5.Display.setCursor(5, 10);
                M5.Display.setTextColor(TFT_GREEN);
                M5.Display.println("PROVISIONING OK !");
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.println("");
                M5.Display.printf("WiFi: %s\n", cfgWifiSSID.c_str());
                M5.Display.printf("MQTT: %s\n", cfgMqttHost.c_str());
                M5.Display.printf("Autoclave: %s\n", cfgAutoclaveName.c_str());
                M5.Display.printf("Baud: %d\n", cfgBaudRate);

                saveConfigToNVS();
                Serial.println("[PROV] Config datalogger sauvegardee dans NVS");

                delay(2000);
                M5.Display.setTextColor(TFT_YELLOW);
                M5.Display.println("");
                M5.Display.println("Redemarrage...");
                delay(1000);
                ESP.restart();
            } else if (r == QR_INVALID_FORMAT || r == QR_NOT_PROVISIONING) {
                M5.Speaker.setVolume(50);
                M5.Speaker.tone(300, 300);
                M5.Display.setTextColor(TFT_RED);
                M5.Display.println("QR invalide !");
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.println("Reessayez...");
                delay(1000);
            }
            // QR_PARTIAL : on attend silencieusement le fragment suivant
        }
        delay(50);
    }
}

// ═══════════════════════════════════════════════════════════
// Helpers écran
// ═══════════════════════════════════════════════════════════

void wakeScreen() {
    if (!screenOn) {
        M5.Display.wakeup();
        M5.Display.setBrightness(200);
        screenOn = true;
    }
    lastActivity = millis();
}

void sleepScreen() {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
    screenOn = false;
}

void showStatus(const char* line1, uint16_t color1,
                const char* line2 = nullptr, uint16_t color2 = TFT_WHITE) {
    wakeScreen();
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(5, 10);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextColor(color1);
    M5.Display.println(line1);
    if (line2) {
        M5.Display.setTextColor(color2);
        M5.Display.println(line2);
    }
}

// Indicateurs WiFi (icône) + MQTT (cercle plein) en bas-droite.
// Zone réservée : x=255..320, y=205..240 (65×35).
void drawStatusIndicators() {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool mqttOk = mqtt.connected();
    uint16_t wcol = wifiOk ? TFT_GREEN : TFT_DARKGREY;
    uint16_t mcol = mqttOk ? TFT_GREEN : TFT_DARKGREY;

    M5.Display.fillRect(255, 205, 65, 35, TFT_BLACK);

    // Icône WiFi : point + 2 arcs concentriques (moitié haute), centre-bas (275, 230)
    M5.Display.fillCircle(275, 230, 2, wcol);
    M5.Display.drawCircleHelper(275, 230, 6,  0x3, wcol);
    M5.Display.drawCircleHelper(275, 230, 7,  0x3, wcol);
    M5.Display.drawCircleHelper(275, 230, 11, 0x3, wcol);
    M5.Display.drawCircleHelper(275, 230, 12, 0x3, wcol);
    // Pastille MQTT
    M5.Display.fillCircle(308, 225, 7, mcol);
}

void showIdleScreen() {
    M5.Display.setTextScroll(false);
    M5.Display.fillScreen(TFT_BLACK);

    // Nom autoclave (gros, en haut)
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(10, 20);
    M5.Display.println(cfgAutoclaveName.length() > 0 ? cfgAutoclaveName.c_str() : "(autoclave)");

    // Numéro de série
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(10, 70);
    M5.Display.printf("Serie : %s", cfgAutoclaveSerial.length() > 0 ? cfgAutoclaveSerial.c_str() : "-");

    // Dernier cycle (label + valeur)
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setCursor(10, 120);
    M5.Display.print("Dernier cycle transmis:");

    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(10, 145);
    M5.Display.println(lastCycleDisplay.c_str());

    drawStatusIndicators();

    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextScroll(true);   // réactiver le scroll pour les données RS-232
    lastActivity = millis();
}

void stopProvScan() {
    module_qrcode.stopDecode();
    provScanning = false;
    showIdleScreen();
}

// ═══════════════════════════════════════════════════════════
// Réseau
// ═══════════════════════════════════════════════════════════

void sendHeartbeat();  // forward decl (défini plus bas)

void connectWifi() {
    Serial.printf("[WIFI] Connexion a '%s'...\n", cfgWifiSSID.c_str());
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.printf("WiFi: %s\n", cfgWifiSSID.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgWifiSSID.c_str(), cfgWifiPassword.c_str());

    unsigned long t0 = millis();
    unsigned long lastLog = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
        Serial.print(".");
        M5.Display.print(".");
        if (millis() - lastLog > 1500) {
            lastLog = millis();
            Serial.printf(" [status=%d]", WiFi.status());
        }
    }
    Serial.printf("\n[WIFI] final status=%d\n", WiFi.status());

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK %s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.printf("\nOK %s\n", WiFi.localIP().toString().c_str());

        // Synchro NTP. TZ Europe/Paris pour l'affichage local ;
        // isoNow() reste en UTC via gmtime_r pour les payloads MQTT.
        configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
                     "pool.ntp.org", "time.nist.gov");
    } else {
        // Diagnostic WiFi (identique au scanner)
        Serial.println("[WIFI] KO");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(0, 0);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.printf("WiFi KO status=%d\n", WiFi.status());
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.printf("SSID voulu: '%s' (%d)\n",
                          cfgWifiSSID.c_str(), cfgWifiSSID.length());
        M5.Display.printf("Pass len=%d\n\n", cfgWifiPassword.length());

        Serial.printf("[WIFI] SSID attendu: '%s' (len=%d)\n",
                      cfgWifiSSID.c_str(), cfgWifiSSID.length());
        Serial.printf("[WIFI] Pass len=%d\n", cfgWifiPassword.length());

        M5.Display.setTextColor(TFT_CYAN);
        M5.Display.println("Reseaux visibles:");
        M5.Display.setTextColor(TFT_WHITE);
        Serial.println("[WIFI] Scan des reseaux visibles:");

        WiFi.disconnect(true, true);
        delay(300);
        WiFi.mode(WIFI_STA);
        delay(100);
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("[WIFI] scanNetworks() = %d\n", n);
        if (n <= 0) {
            M5.Display.setTextColor(TFT_RED);
            M5.Display.printf("Scan echoue (n=%d)\n", n);
            M5.Display.setTextColor(TFT_WHITE);
        }
        for (int i = 0; i < n && i < 10; i++) {
            char band = (WiFi.channel(i) > 14) ? '5' : 'G';
            bool match = (WiFi.SSID(i) == cfgWifiSSID);
            M5.Display.setTextColor(match ? TFT_GREEN : TFT_WHITE);
            M5.Display.printf("%c%ddB %s\n",
                              band, WiFi.RSSI(i), WiFi.SSID(i).c_str());
            Serial.printf("  %2d: %-32s  rssi=%d  ch=%d  enc=%d\n",
                          i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), WiFi.encryptionType(i));
        }
        M5.Display.setFont(&fonts::Font4);
    }
}

// ═══════════════════════════════════════════════════════════
// MQTT — réception des commandes (datalogger/command/<deviceId>)
// ═══════════════════════════════════════════════════════════
//
// Format attendu : { "cmd": "deprovision" }
// Effet "deprovision" : efface NVS + reboot → mode provisioning QR
// (équivalent au triple-RESET physique).

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char buf[512];
    if (length >= sizeof(buf)) length = sizeof(buf) - 1;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    Serial.printf("[CMD] recu sur '%s': %s\n", topic, buf);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        Serial.printf("[CMD] JSON parse error: %s\n", err.c_str());
        return;
    }

    String cmd = doc["cmd"] | "";
    if (cmd == "deprovision") {
        Serial.println("[CMD] deprovision -> efface NVS et reboot");

        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(5, 40);
        M5.Display.setFont(&fonts::Font4);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.println("DISSOCIATION");
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.println("Effacement config...");

        clearNVS();
        clearResetCounter();
        delay(1500);
        ESP.restart();
    } else {
        Serial.printf("[CMD] inconnue: '%s'\n", cmd.c_str());
    }
}

void connectMqtt() {
    net.setInsecure();  // TLS sans vérification de cert (HiveMQ Cloud)
    mqtt.setServer(cfgMqttHost.c_str(), cfgMqttPort);
    mqtt.setCallback(mqttCallback);
    // PubSubClient buffer par défaut = 256 bytes, trop petit pour les cycles
    // On augmente pour supporter les payloads de données RS-232
    mqtt.setBufferSize(4096);

    unsigned long t0 = millis();
    while (!mqtt.connected() && millis() - t0 < 10000) {
        Serial.print("MQTT...");
        if (mqtt.connect(deviceId.c_str(), cfgMqttUser.c_str(), cfgMqttPass.c_str(),
                         TOPIC_STATUS, 1, true, lwtMessage.c_str())) {
            Serial.println(" OK");
            mqtt.publish(TOPIC_STATUS, onlineMessage.c_str(), true);
            mqtt.subscribe(TOPIC_COMMAND.c_str(), 1);
            Serial.printf("[MQTT] Subscribed to %s\n", TOPIC_COMMAND.c_str());
            // Annonce immédiate de l'identité complète (nom autoclave, série,
            // cabinet) — sans ça l'iPad doit attendre 30s le premier heartbeat.
            sendHeartbeat();
            lastHeartbeat = millis();
            M5.Display.setTextColor(TFT_GREEN);
            M5.Display.println("MQTT OK");
        } else {
            Serial.printf(" fail rc=%d\n", mqtt.state());
            delay(1000);
        }
    }

    if (!mqtt.connected()) {
        M5.Display.setTextColor(TFT_RED);
        M5.Display.printf("MQTT KO rc=%d\n", mqtt.state());
    }
}

// Timestamp ISO 8601 UTC : "2026-04-19T14:32:00Z"
String isoNow() {
    time_t now = time(nullptr);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return String(buf);
}

// Timestamp local pour affichage : "27/04/2026 14:32"
// Renvoie "" si l'horloge n'est pas encore synchronisée.
String localNow() {
    time_t now = time(nullptr);
    if (now < 1700000000) return "";  // NTP pas encore synchro
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char buf[24];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tm_local);
    return String(buf);
}

// ═══════════════════════════════════════════════════════════
// MQTT — envoi des données
// ═══════════════════════════════════════════════════════════

// Envoi d'un heartbeat datalogger
void sendHeartbeat() {
    if (!mqtt.connected()) return;

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"type\":\"datalogger\","
             "\"autoclaveName\":\"%s\",\"autoclaveSerial\":\"%s\","
             "\"cabinetId\":\"%s\",\"uptime\":%lu,\"rssi\":%d,"
             "\"linesReceived\":%lu,\"cyclesSent\":%lu}",
             deviceId.c_str(), cfgAutoclaveName.c_str(),
             cfgAutoclaveSerial.c_str(), cfgCabinetId.c_str(),
             millis() / 1000, WiFi.RSSI(),
             totalLinesReceived, totalCyclesSent);

    mqtt.publish(TOPIC_HEARTBEAT, payload);
    Serial.printf("[HB] %s\n", payload);
}

// Envoi des données brutes d'un cycle terminé
void sendCycleData(const String& rawData) {
    if (!mqtt.connected()) {
        Serial.println("[MQTT] Non connecte, tentative reconnexion...");
        connectMqtt();
    }

    // Construire le payload JSON avec ArduinoJson (pour gérer l'escaping)
    JsonDocument doc;
    doc["device"]          = deviceId;
    doc["type"]            = "rs232-cycle";
    doc["autoclaveName"]   = cfgAutoclaveName;
    doc["autoclaveSerial"] = cfgAutoclaveSerial;
    doc["cabinetId"]       = cfgCabinetId;
    doc["timestamp"]       = isoNow();
    doc["data"]            = rawData;
    doc["durationSeconds"] = (millis() - cycleStartTime) / 1000;
    doc["lineCount"]       = totalLinesReceived;

    // Sérialiser
    String payload;
    serializeJson(doc, payload);

    Serial.printf("[MQTT] Envoi cycle (%d bytes)...\n", payload.length());

    // PubSubClient.publish() retourne false si le message dépasse le buffer
    // ou si la connexion est perdue
    if (mqtt.publish(TOPIC_CYCLE, payload.c_str())) {
        totalCyclesSent++;
        String ts = localNow();
        if (ts.length() > 0) lastCycleDisplay = ts;
        Serial.println("[MQTT] Cycle publie OK");
        showStatus("CYCLE ENVOYE", TFT_GREEN, "MQTT OK", TFT_WHITE);
    } else {
        Serial.printf("[MQTT] Echec publication (len=%d, connected=%d)\n",
                      payload.length(), mqtt.connected());
        showStatus("ERREUR ENVOI", TFT_RED, "Retry au prochain cycle", TFT_YELLOW);
        // TODO: stocker en SPIFFS/LittleFS pour retry ultérieur
    }
}

// Envoi d'une ligne RS-232 en temps réel (optionnel, pour debug/monitoring)
void sendLine(const String& line) {
    if (!mqtt.connected()) return;

    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"autoclaveName\":\"%s\","
             "\"autoclaveSerial\":\"%s\",\"cabinetId\":\"%s\","
             "\"line\":\"%s\",\"timestamp\":\"%s\"}",
             deviceId.c_str(), cfgAutoclaveName.c_str(),
             cfgAutoclaveSerial.c_str(), cfgCabinetId.c_str(),
             line.c_str(), isoNow().c_str());

    mqtt.publish(TOPIC_LINE, payload);
}

// ═══════════════════════════════════════════════════════════
// Parsing RS-232 autoclave
// ═══════════════════════════════════════════════════════════
//
// Le format exact dépend de l'autoclave. Pour le STATCLAVE G4
// (SciCan), le port série envoie typiquement un "ticket" texte
// à la fin de chaque cycle, contenant :
//   - Numéro de cycle
//   - Date/heure
//   - Températures (consigne / max atteinte)
//   - Pression
//   - Durée phases (chauffage, stérilisation, séchage)
//   - Statut final (PASS/FAIL)
//
// Stratégie : on accumule toutes les lignes reçues. Quand on
// détecte un silence de CYCLE_END_TIMEOUT ms, on considère que
// le ticket est complet et on l'envoie via MQTT.
//
// Le parsing fin (extraction température, pression, statut) sera
// fait côté iPad/serveur, pas sur l'ESP32.

void processSerialLine(const String& line) {
    totalLinesReceived++;
    Serial.printf("[RS232] %s\n", line.c_str());

    // Si c'est la première ligne après un silence, on démarre un nouveau cycle
    if (!cycleInProgress) {
        cycleInProgress = true;
        cycleStartTime = millis();
        currentCycleData = "";

        wakeScreen();
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(5, 10);
        M5.Display.setFont(&fonts::Font4);
        M5.Display.setTextColor(TFT_ORANGE);
        M5.Display.println("RECEPTION CYCLE");
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setFont(&fonts::Font2);

        Serial.println("[CYCLE] Debut reception donnees");
    }

    // Accumuler la ligne
    if (currentCycleData.length() > 0) {
        currentCycleData += "\n";
    }
    currentCycleData += line;

    // Affichage écran (dernières lignes, scroll automatique)
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.println(line.c_str());

    // Optionnel : publier chaque ligne en temps réel pour monitoring
    // Décommenter si nécessaire (attention au débit MQTT) :
    // sendLine(line);

    lastSerialActivity = millis();
}

// ═══════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════

void setup()
{
    M5.begin();
    M5.update();
    Serial.begin(115200);

    // Laisse le host rattacher le CDC (ré-énumération au reset)
    delay(1500);
    Serial.println("\n========== BOOT DATALOGGER ==========");

    M5.Display.setRotation(3);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextSize(1);
    M5.Display.setTextScroll(true);

    // ─── Reset NVS : triple-reset pattern ───
    if (checkTripleReset()) {
        Serial.println("[RESET] Triple reset detecte -> effacement NVS");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(5, 40);
        M5.Display.println("RESET CONFIG");
        M5.Display.println("DATALOGGER");
        clearNVS();
        clearResetCounter();
        delay(1500);
        ESP.restart();
    }

    // Générer deviceId unique depuis la MAC (3 derniers octets)
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char idBuf[24];
    snprintf(idBuf, sizeof(idBuf), "datalogger-%02X%02X%02X", mac[3], mac[4], mac[5]);
    deviceId = idBuf;
    TOPIC_COMMAND = String("datalogger/command/") + deviceId;
    Serial.printf("[BOOT] Device ID: %s\n", deviceId.c_str());
    Serial.printf("[BOOT] cmd topic: %s\n", TOPIC_COMMAND.c_str());

    // Messages LWT / status : construits après loadConfigFromNVS() pour inclure autoclaveSerial
    // (placeholder vide ici, rempli plus bas)
    lwtMessage    = "";
    onlineMessage = "";

    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.println(deviceId.c_str());

    // Affichage compteur triple-reset
    prefs.begin("boot-dl", true);
    uint32_t bootCnt = prefs.getUInt("cnt", 0);
    prefs.end();
    if (bootCnt > 0) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(bootCnt >= 2 ? TFT_ORANGE : TFT_DARKGREY);
        M5.Display.printf("reset %u/3\n", bootCnt);
        M5.Display.setFont(&fonts::Font4);
    }

    // Init module QR (pour provisioning uniquement)
    auto cfg     = module_qrcode.getConfig();
    cfg.pin_tx   = 17;
    cfg.pin_rx   = 18;
    cfg.baudrate = 115200;
    cfg.serial   = &Serial2;
    module_qrcode.setConfig(cfg);

    // Buffer RX agrandi avant le Serial2.begin() interne de la lib : les QR de
    // provisioning dépassent les 256 octets par défaut et arriveraient tronqués.
    Serial2.setRxBufferSize(1024);

    qrAvailable = module_qrcode.begin();
    if (qrAvailable) {
        module_qrcode.setFillLightMode(QRCodeM14::FILL_LIGHT_ON_DECODE);
        module_qrcode.setPosLightMode(QRCodeM14::POS_LIGHT_ON_DECODE);
        module_qrcode.setTriggerMode(QRCodeM14::TRIGGER_MODE_CONTINUOUS);
        module_qrcode.setDecodeSuccessBeep(0);
        module_qrcode.stopDecode();
    } else {
        Serial.println("[QR] Module QR non detecte (provisioning par QR indisponible)");
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.println("Module QR absent");
        M5.Display.setFont(&fonts::Font4);
    }

    // ─── Vérifier si provisionné (NVS) ───
    provisioned = loadConfigFromNVS();

    // Construire les messages LWT / online avec autoclaveSerial (disponible après NVS)
    lwtMessage    = "{\"status\":\"offline\",\"device\":\"" + deviceId + "\",\"type\":\"datalogger\""
                  + (cfgAutoclaveSerial.length() > 0
                     ? ",\"autoclaveSerial\":\"" + cfgAutoclaveSerial + "\"}"
                     : "}");
    onlineMessage = "{\"status\":\"online\",\"device\":\"" + deviceId + "\",\"type\":\"datalogger\""
                  + (cfgAutoclaveSerial.length() > 0
                     ? ",\"autoclaveSerial\":\"" + cfgAutoclaveSerial + "\"}"
                     : "}");

    if (!provisioned) {
        if (qrAvailable) {
            enterProvisioningMode();
            // enterProvisioningMode() fait un ESP.restart()
            return;
        } else {
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setTextColor(TFT_RED);
            M5.Display.setCursor(5, 40);
            M5.Display.println("NON PROVISIONNE");
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.println("Module QR requis");
            M5.Display.println("pour provisioning");
            Serial.println("[BOOT] Non provisionne, module QR absent. Bloque.");
            while (true) { M5.update(); delay(200); }
        }
    }

    Serial.printf("[BOOT] Config NVS chargee — WiFi: %s, MQTT: %s, Autoclave: %s, Baud: %d\n",
                  cfgWifiSSID.c_str(), cfgMqttHost.c_str(),
                  cfgAutoclaveName.c_str(), cfgBaudRate);

    // ─── Mode normal : connexion réseau ───
    connectWifi();

    // Si le WiFi n'est pas connecté, on continue quand même (mode dégradé).
    // Le loop() tentera la reconnexion périodiquement, et le bouton REPROV
    // permettra de changer les credentials WiFi sans triple-reset.
    if (WiFi.status() != WL_CONNECTED) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.println("\nWiFi KO — reconnexion auto");
        if (qrAvailable) {
            M5.Display.println("Bouton REPROV pour reconfigurer");
        } else {
            M5.Display.println("Triple RESET = reprov");
        }
        M5.Display.setFont(&fonts::Font4);
        Serial.println("[BOOT] WiFi KO — mode degrade, loop quand meme");
    }

    // ─── Connexion MQTT (HiveMQ Cloud TLS) ───
    connectMqtt();

    // ─── Init RS-232 (UART1 via MAX3232) ───
    RS232Serial.begin(cfgBaudRate, SERIAL_8N1, RS232_RX_PIN, RS232_TX_PIN);
    Serial.printf("[RS232] UART1 init — baud=%d, RX=GPIO%d, TX=GPIO%d\n",
                  cfgBaudRate, RS232_RX_PIN, RS232_TX_PIN);
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.printf("RS-232: %d baud\n", cfgBaudRate);

    // Écran d'attente
    delay(1000);
    showIdleScreen();

    Serial.println("[BOOT] Datalogger pret — en ecoute RS-232...");
}

// ═══════════════════════════════════════════════════════════
// Loop
// ═══════════════════════════════════════════════════════════

void loop()
{
    M5.update();

    // Compteur triple-reset : effacé après 10s d'utilisation stable
    static unsigned long loopStartedAt = millis();
    static bool resetCounterCleared = false;
    if (!resetCounterCleared && millis() - loopStartedAt > 10000) {
        clearResetCounter();
        resetCounterCleared = true;
        Serial.println("[BOOT] Reset counter cleared");
    }

    // ─── Maintien connexion MQTT ───
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();

    // ─── Reconnexion WiFi si perdu ───
    static unsigned long lastWifiCheck = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > WIFI_RECONNECT_DELAY) {
        lastWifiCheck = millis();
        Serial.println("[WIFI] Reconnexion...");
        WiFi.reconnect();
    }

    // ─── Lecture RS-232 ───
    while (RS232Serial.available()) {
        char c = RS232Serial.read();

        if (c == '\n' || c == '\r') {
            if (serialBufferPos > 0) {
                serialBuffer[serialBufferPos] = '\0';
                processSerialLine(String(serialBuffer));
                serialBufferPos = 0;
            }
        } else if (serialBufferPos < SERIAL_BUFFER_SIZE - 1) {
            serialBuffer[serialBufferPos++] = c;
        }
    }

    // ─── Détection fin de cycle (silence RS-232) ───
    if (cycleInProgress && millis() - lastSerialActivity > CYCLE_END_TIMEOUT) {
        Serial.printf("[CYCLE] Fin detectee (silence %lu ms). %d octets accumules.\n",
                      CYCLE_END_TIMEOUT, currentCycleData.length());

        // Envoyer les données accumulées via MQTT
        if (currentCycleData.length() > 0) {
            sendCycleData(currentCycleData);
        }

        cycleInProgress = false;
        currentCycleData = "";

        delay(2000);
        showIdleScreen();
    }

    // ─── Heartbeat périodique (avec infos datalogger) ───
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    // ─── Gestion QR reprovisioning (scan en cours) ───
    if (provScanning) {
        module_qrcode.update();

        // Timeout
        if (millis() - provScanStart > PROV_SCAN_TIMEOUT) {
            Serial.println("[REPROV] Timeout scan QR");
            M5.Display.setTextColor(TFT_RED);
            M5.Display.println("Timeout");
            delay(800);
            stopProvScan();
        }

        // Résultat QR
        if (module_qrcode.available()) {
            auto result = module_qrcode.getScanResult();
            String qrContent = result.c_str();
            Serial.printf("[REPROV] Fragment recu (%d octets)\n", qrContent.length());

            QRFragResult r = consumeQRFragment(qrContent);
            if (r == QR_PARSED_OK) {
                module_qrcode.stopDecode();
                provScanning = false;

                M5.Speaker.setVolume(50);
                M5.Speaker.tone(1000, 100);
                delay(100);
                M5.Speaker.tone(1500, 100);

                M5.Display.fillScreen(TFT_BLACK);
                M5.Display.setCursor(5, 10);
                M5.Display.setTextColor(TFT_GREEN);
                M5.Display.setFont(&fonts::Font4);
                M5.Display.println("REPROV OK !");
                M5.Display.setTextColor(TFT_WHITE);
                M5.Display.println("");
                M5.Display.printf("WiFi: %s\n", cfgWifiSSID.c_str());
                M5.Display.printf("MQTT: %s\n", cfgMqttHost.c_str());
                M5.Display.printf("Autoclave: %s\n", cfgAutoclaveName.c_str());
                M5.Display.printf("Baud: %d\n", cfgBaudRate);

                saveConfigToNVS();
                Serial.println("[REPROV] Config mise a jour dans NVS — reboot");

                delay(2000);
                M5.Display.setTextColor(TFT_YELLOW);
                M5.Display.println("");
                M5.Display.println("Redemarrage...");
                delay(1000);
                ESP.restart();
            } else if (r == QR_INVALID_FORMAT || r == QR_NOT_PROVISIONING) {
                M5.Speaker.setVolume(50);
                M5.Speaker.tone(300, 200);

                M5.Display.setTextColor(TFT_RED);
                M5.Display.setFont(&fonts::Font2);
                M5.Display.println(r == QR_NOT_PROVISIONING
                                   ? "QR non-provisioning"
                                   : "QR invalide");
                M5.Display.setTextColor(TFT_DARKGREY);
                M5.Display.println("Attendu: PROV:{...}");
                M5.Display.setFont(&fonts::Font4);
            }
            // QR_PARTIAL : on attend le fragment suivant, pas de feedback
        }
    }

    // ─── Extinction écran après inactivité ───
    if (screenOn && !cycleInProgress && !provScanning &&
        millis() - lastActivity > SCREEN_TIMEOUT) {
        sleepScreen();
    }

    // ─── Toucher écran ───
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
        wakeScreen();
        if (provScanning) {
            Serial.println("[REPROV] Scan annule par toucher");
            stopProvScan();
        } else {
            showIdleScreen();
        }
    }

    // ─── Mise à jour des indicateurs si le statut WiFi/MQTT change ───
    static bool lastWifiState = false;
    static bool lastMqttState = false;
    bool wifiNow = (WiFi.status() == WL_CONNECTED);
    bool mqttNow = mqtt.connected();
    if (screenOn && !cycleInProgress && !provScanning &&
        (wifiNow != lastWifiState || mqttNow != lastMqttState)) {
        drawStatusIndicators();
        lastWifiState = wifiNow;
        lastMqttState = mqttNow;
    }

    delay(10);  // yield
}
