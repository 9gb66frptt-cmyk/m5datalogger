/*
 * ═══════════════════════════════════════════════════════════════════
 *  Stérig — Firmware Datalogger BLE pour M5Stack CoreS3
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Variant sans accessoire : provisioning via Bluetooth Low Energy
 *  depuis l'app iPad Stery, puis MQTT heartbeat (pas de RS-232 pour
 *  cette première itération — sera ajouté ensuite).
 *
 *  Provisioning BLE (GATT) :
 *    Service  : CAFE0001-CAFE-CAFE-CAFE-CFEECFEECFEE
 *    Write    : CAFE0002-CAFE-CAFE-CAFE-CFEECFEECFEE  (iPad → M5)
 *    Notify   : CAFE0003-CAFE-CAFE-CAFE-CFEECFEECFEE  (M5 → iPad)
 *
 *  Advertising name : STERY-DL-XXXXXX (3 derniers octets MAC)
 *  Device ID MQTT   : datalogger-XXXXXX (mêmes 3 octets)
 *
 *  Format JSON (clés courtes, identique au QR) :
 *    { "t":"datalogger", "w":"...", "wp":"...",
 *      "h":"mqttHost", "p":8883, "u":"...", "mp":"...",
 *      "c":"cabinetId", "an":"autoclaveName", "as":"autoclaveSerial",
 *      "br":9600 }
 *
 *  Réponse notify : "OK" si parse + sauvegarde NVS OK, "ERR:xxx" sinon.
 *
 *  NVS namespace : "stery-dl" (partagé avec main_datalogger.cpp)
 *  Reprovisioning : triple-RESET (efface NVS + reboot)
 * ═══════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#define FW_VERSION "1.0.5-ble"

Preferences prefs;

// ─── Config (chargée depuis NVS ou prov BLE) ───
String cfgWifiSSID;
String cfgWifiPassword;
String cfgMqttHost;
int    cfgMqttPort = 8883;
String cfgMqttUser;
String cfgMqttPass;
String cfgCabinetId;
String cfgAutoclaveName;
String cfgAutoclaveSerial;
int    cfgBaudRate = 9600;

// ─── Identité ───
String deviceId;     // "datalogger-A1B2C3"
String bleAdvName;   // "STERY-DL-A1B2C3"

// ─── Topics MQTT ───
const char* TOPIC_HEARTBEAT = "datalogger/heartbeat";
const char* TOPIC_STATUS    = "datalogger/status";
String      TOPIC_COMMAND;  // "datalogger/command/<deviceId>" — construit dans setup()

String lwtMessage;
String onlineMessage;

WiFiClientSecure net;
PubSubClient mqtt(net);

// ─── BLE GATT ───
#define BLE_SVC_UUID    "CAFE0001-CAFE-CAFE-CAFE-CFEECFEECFEE"
#define BLE_CHAR_WRITE  "CAFE0002-CAFE-CAFE-CAFE-CFEECFEECFEE"
#define BLE_CHAR_NOTIFY "CAFE0003-CAFE-CAFE-CAFE-CFEECFEECFEE"

NimBLECharacteristic* pNotifyChar = nullptr;

// Réassemblage des writes BLE (si JSON > MTU)
String bleRxBuffer = "";
unsigned long bleRxFirstAt = 0;
const unsigned long BLE_RX_TIMEOUT = 3000;

volatile bool blePendingReboot = false;

// ─── Timers ───
const unsigned long HEARTBEAT_INTERVAL   = 30000;
const unsigned long WIFI_RECONNECT_DELAY = 30000;
const unsigned long SCREEN_TIMEOUT       = 30000;

unsigned long lastHeartbeat = 0;
unsigned long lastActivity  = 0;
unsigned long totalHeartbeats = 0;

bool screenOn = true;
bool provisioned = false;

// ═══════════════════════════════════════════════════════════
// NVS
// ═══════════════════════════════════════════════════════════

bool loadConfigFromNVS() {
    prefs.begin("stery-dl", true);
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
    prefs.begin("stery-dl", false);
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
// Parse du JSON de provisioning
// ═══════════════════════════════════════════════════════════

bool parseProvisioningJson(const String& jsonStr, String& errOut) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonStr);
    if (err) {
        errOut = String("ERR:json:") + err.c_str();
        Serial.printf("[PROV] JSON parse error: %s\n", err.c_str());
        return false;
    }

    String type = doc["t"] | "";
    if (type != "datalogger") {
        errOut = "ERR:type";
        Serial.printf("[PROV] Type invalide: '%s'\n", type.c_str());
        return false;
    }

    if (!doc["w"].is<const char*>() ||
        !doc["h"].is<const char*>() ||
        !doc["u"].is<const char*>() ||
        !doc["mp"].is<const char*>()) {
        errOut = "ERR:fields";
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

// Forward declarations (helpers écran définis plus bas mais utilisés dès l'OTA).
void wakeScreen();

// ═══════════════════════════════════════════════════════════
// OTA — download streamé avec vérif SHA-256 optionnelle
// ═══════════════════════════════════════════════════════════
//
// On streame le binaire dans Update.write() en hashant à la volée. Si un
// SHA-256 attendu est fourni et ne matche pas, on abort avant Update.end()
// → la partition reste sur l'ancien firmware, pas de reboot, pas de risque.
//
// Suivi manuel des redirects : `HTTPClient::setFollowRedirects` est instable
// sur ESP32 quand le redirect croise deux hosts HTTPS différents (cas de
// GitHub Releases : github.com → objects.githubusercontent.com).
//
// Renvoie "" si succès (caller doit reboot), sinon un libellé d'erreur court.

static String runOta(const String& urlIn, const String& expectedSha) {
    String url = urlIn;
    HTTPClient http;
    WiFiClient        plainClient;
    WiFiClientSecure  tlsClient;
    WiFiClient*       transport = nullptr;

    int code = 0;
    const int MAX_HOPS = 5;
    int hop = 0;
    for (; hop < MAX_HOPS; hop++) {
        if (url.startsWith("https://")) {
            tlsClient.setInsecure();
            transport = &tlsClient;
        } else {
            transport = &plainClient;
        }

        if (!http.begin(*transport, url)) return "http-begin";
        const char* keys[] = {"Location"};
        http.collectHeaders(keys, 1);
        http.setUserAgent("ESP32-Datalogger/" FW_VERSION);

        code = http.GET();
        Serial.printf("[OTA] hop %d -> %d (%s)\n", hop, code, url.c_str());

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            String loc = http.header("Location");
            http.end();
            if (loc.length() == 0) return "no-location";
            url = loc;
            continue;  // nouvelle iter avec une URL fraiche
        }
        break;  // 200 OK ou erreur définitive
    }
    if (hop >= MAX_HOPS) return "too-many-redirects";
    if (code != HTTP_CODE_OK) {
        http.end();
        return String("http-") + code;
    }

    int total = http.getSize();
    if (total <= 0) {
        http.end();
        return "no-length";
    }

    if (!Update.begin(total)) {
        http.end();
        return String("upd-begin:") + Update.errorString();
    }

    bool checkSha = (expectedSha.length() == 64);
    mbedtls_sha256_context shaCtx;
    if (checkSha) {
        mbedtls_sha256_init(&shaCtx);
        mbedtls_sha256_starts(&shaCtx, 0);  // 0 = SHA-256
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int remaining = total;
    unsigned long lastLog = 0;
    int lastPctShown = -1;

    while (http.connected() && remaining > 0) {
        size_t avail = stream->available();
        if (avail) {
            int n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
            if (checkSha) mbedtls_sha256_update(&shaCtx, buf, n);
            if (Update.write(buf, n) != (size_t)n) {
                if (checkSha) mbedtls_sha256_free(&shaCtx);
                Update.abort();
                http.end();
                return "upd-write";
            }
            remaining -= n;

            if (millis() - lastLog > 500) {
                lastLog = millis();
                int pct = 100 - (remaining * 100 / total);
                Serial.printf("[OTA] %d%% (%d/%d)\n", pct, total - remaining, total);
                if (pct != lastPctShown) {
                    lastPctShown = pct;
                    M5.Display.fillRect(5, 100, 310, 20, TFT_BLACK);
                    M5.Display.setCursor(5, 100);
                    M5.Display.setTextColor(TFT_WHITE);
                    M5.Display.setFont(&fonts::Font2);
                    M5.Display.printf("Download: %d%%", pct);
                }
            }
        } else {
            delay(1);
        }
    }

    if (checkSha) {
        uint8_t hash[32];
        mbedtls_sha256_finish(&shaCtx, hash);
        mbedtls_sha256_free(&shaCtx);
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);
        hex[64] = 0;
        if (!String(hex).equalsIgnoreCase(expectedSha)) {
            Serial.printf("[OTA] SHA mismatch\n  got = %s\n  exp = %s\n",
                          hex, expectedSha.c_str());
            Update.abort();
            http.end();
            return "sha-mismatch";
        }
        Serial.printf("[OTA] SHA-256 OK: %s\n", hex);
    }

    if (!Update.end(true)) {  // true = commit
        http.end();
        return String("upd-end:") + Update.errorString();
    }
    http.end();
    return "";
}

// ═══════════════════════════════════════════════════════════
// MQTT — réception des commandes (datalogger/command/<deviceId>)
// ═══════════════════════════════════════════════════════════
//
// Format attendu : { "cmd": "deprovision" }
// Effet "deprovision" : efface NVS + reboot → mode BLE provisioning
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
    } else if (cmd == "ota") {
        // OTA déclenché par MQTT.
        // Payload attendu :
        //   { "cmd":"ota", "url":"https://.../firmware.bin",
        //     "sha256":"<hex 64 chars optionnel>" }
        // Sans sha256 : intégrité non vérifiée (OK pour dev local, à éviter en prod).
        String url = doc["url"] | "";
        String sha = doc["sha256"] | "";
        if (url.length() == 0) {
            Serial.println("[OTA] URL manquante");
            String err = "{\"status\":\"ota-error\",\"device\":\"" + deviceId + "\",\"error\":\"no-url\"}";
            mqtt.publish(TOPIC_STATUS, err.c_str());
            return;
        }
        Serial.printf("[OTA] Demarrage update depuis %s (sha=%s)\n",
                      url.c_str(), sha.length() ? "yes" : "no");

        wakeScreen();
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(5, 20);
        M5.Display.setFont(&fonts::Font4);
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.println("OTA UPDATE");
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.println("");
        M5.Display.println("Telechargement...");

        String startMsg = "{\"status\":\"ota-started\",\"device\":\"" + deviceId
                        + "\",\"fwVersion\":\"" FW_VERSION "\""
                        + ",\"shaCheck\":" + (sha.length() == 64 ? "true" : "false")
                        + "}";
        mqtt.publish(TOPIC_STATUS, startMsg.c_str());
        mqtt.disconnect();  // libère le client TLS pour le download

        String err = runOta(url, sha);
        if (err.length() == 0) {
            Serial.println("[OTA] OK -> reboot");
            M5.Display.setTextColor(TFT_GREEN);
            M5.Display.setFont(&fonts::Font4);
            M5.Display.fillRect(0, 80, 320, 80, TFT_BLACK);
            M5.Display.setCursor(5, 100);
            M5.Display.println("OTA OK !");
            M5.Display.setFont(&fonts::Font2);
            M5.Display.println("Reboot...");
            delay(1500);
            ESP.restart();
        } else {
            Serial.printf("[OTA] Echec: %s\n", err.c_str());
            M5.Display.setTextColor(TFT_RED);
            M5.Display.setFont(&fonts::Font2);
            M5.Display.fillRect(0, 100, 320, 60, TFT_BLACK);
            M5.Display.setCursor(5, 105);
            M5.Display.printf("OTA KO\n%s\n", err.c_str());
            // Le loop reconnectera MQTT, on publiera l'erreur au prochain heartbeat.
            // L'ancien firmware reste actif (Update.end pas appelé en commit).
        }
    } else {
        Serial.printf("[CMD] inconnue: '%s'\n", cmd.c_str());
    }
}

// ═══════════════════════════════════════════════════════════
// Notifications BLE
// ═══════════════════════════════════════════════════════════

void bleNotify(const char* msg) {
    if (!pNotifyChar) return;
    pNotifyChar->setValue((uint8_t*)msg, strlen(msg));
    pNotifyChar->notify();
    Serial.printf("[BLE] notify -> %s\n", msg);
}

// ═══════════════════════════════════════════════════════════
// Callbacks BLE
// ═══════════════════════════════════════════════════════════

class WriteCB : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* pChar) override {
        std::string raw = pChar->getValue();
        Serial.printf("[BLE] Write recu (%u octets)\n", (unsigned)raw.size());

        if (bleRxBuffer.length() == 0) bleRxFirstAt = millis();
        bleRxBuffer += String(raw.c_str());

        String s = bleRxBuffer;
        s.trim();

        if (!s.startsWith("{")) {
            Serial.println("[BLE] Format invalide (doit commencer par '{')");
            bleNotify("ERR:fmt");
            bleRxBuffer = "";
            return;
        }

        if (!s.endsWith("}")) {
            // JSON incomplet, on attend le fragment suivant
            Serial.printf("[BLE] Fragment partiel (%d octets), attente suite\n",
                          bleRxBuffer.length());
            return;
        }

        Serial.printf("[BLE] JSON reassemble (%d octets), parsing...\n", s.length());

        String errMsg;
        if (parseProvisioningJson(s, errMsg)) {
            saveConfigToNVS();
            Serial.println("[PROV] Config sauvegardee dans NVS");

            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(5, 10);
            M5.Display.setFont(&fonts::Font4);
            M5.Display.setTextColor(TFT_GREEN);
            M5.Display.println("PROV OK !");
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.setFont(&fonts::Font2);
            M5.Display.printf("WiFi: %s\n", cfgWifiSSID.c_str());
            M5.Display.printf("MQTT: %s\n", cfgMqttHost.c_str());
            M5.Display.printf("Cabinet: %s\n", cfgCabinetId.c_str());
            M5.Display.println("");
            M5.Display.setTextColor(TFT_YELLOW);
            M5.Display.println("Reboot dans 1s...");

            bleNotify("OK");
            bleRxBuffer = "";
            blePendingReboot = true;
        } else {
            bleNotify(errMsg.c_str());
            bleRxBuffer = "";

            M5.Display.setTextColor(TFT_RED);
            M5.Display.setFont(&fonts::Font2);
            M5.Display.println(errMsg.c_str());
        }
    }
};

class ServerCB : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer) override {
        Serial.println("[BLE] Client connecte");
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.println("iPad connecte");
    }
    void onDisconnect(NimBLEServer* pServer) override {
        Serial.println("[BLE] Client deconnecte — reprise advertising");
        bleRxBuffer = "";
        NimBLEDevice::startAdvertising();
    }
};

WriteCB  writeCb;
ServerCB serverCb;

// ═══════════════════════════════════════════════════════════
// Démarrage du provisioning BLE
// ═══════════════════════════════════════════════════════════

void startBleProvisioning() {
    Serial.printf("[BLE] Init advertising '%s'\n", bleAdvName.c_str());

    NimBLEDevice::init(bleAdvName.c_str());
    NimBLEDevice::setMTU(247);

    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(&serverCb);

    NimBLEService* pSvc = pServer->createService(BLE_SVC_UUID);

    NimBLECharacteristic* pWrite = pSvc->createCharacteristic(
        BLE_CHAR_WRITE,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pWrite->setCallbacks(&writeCb);

    pNotifyChar = pSvc->createCharacteristic(
        BLE_CHAR_NOTIFY,
        NIMBLE_PROPERTY::NOTIFY
    );

    pSvc->start();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(BLE_SVC_UUID);
    pAdv->setName(bleAdvName.c_str());
    pAdv->setScanResponse(true);
    pAdv->start();

    Serial.println("[BLE] Advertising actif — en attente de l'iPad");
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

void showIdleScreen() {
    M5.Display.setTextScroll(false);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(5, 10);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.println("DATALOGGER BLE");
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.println("");
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.printf("ID: %s\n", deviceId.c_str());
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.printf("WiFi: %s\n",
                      WiFi.status() == WL_CONNECTED ? "OK" : "---");
    M5.Display.printf("MQTT: %s\n", mqtt.connected() ? "OK" : "---");
    if (WiFi.status() == WL_CONNECTED) {
        M5.Display.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        M5.Display.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.printf("Heartbeats: %lu\n", totalHeartbeats);
    M5.Display.println("");
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.println("Triple RESET = reprov");
    lastActivity = millis();
}

// ═══════════════════════════════════════════════════════════
// Réseau
// ═══════════════════════════════════════════════════════════

void connectWifi() {
    Serial.printf("[WIFI] Connexion a '%s'...\n", cfgWifiSSID.c_str());
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.printf("WiFi: %s\n", cfgWifiSSID.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgWifiSSID.c_str(), cfgWifiPassword.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
        Serial.print(".");
        M5.Display.print(".");
    }
    Serial.printf("\n[WIFI] final status=%d\n", WiFi.status());

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK %s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        M5.Display.setTextColor(TFT_GREEN);
        M5.Display.printf("\nOK %s\n", WiFi.localIP().toString().c_str());
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    } else {
        Serial.println("[WIFI] KO — diagnostic scan");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(0, 0);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.printf("WiFi KO status=%d\n", WiFi.status());
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.printf("SSID: '%s' (%d)\n",
                          cfgWifiSSID.c_str(), cfgWifiSSID.length());
        M5.Display.printf("Pass len=%d\n\n", cfgWifiPassword.length());

        WiFi.disconnect(true, true);
        delay(300);
        WiFi.mode(WIFI_STA);
        delay(100);
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("[WIFI] scanNetworks = %d\n", n);
        for (int i = 0; i < n && i < 10; i++) {
            char band = (WiFi.channel(i) > 14) ? '5' : 'G';
            bool match = (WiFi.SSID(i) == cfgWifiSSID);
            M5.Display.setTextColor(match ? TFT_GREEN : TFT_WHITE);
            M5.Display.printf("%c%ddB %s\n",
                              band, WiFi.RSSI(i), WiFi.SSID(i).c_str());
        }
        M5.Display.setFont(&fonts::Font4);
    }
}

String isoNow() {
    time_t now = time(nullptr);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return String(buf);
}

void sendHeartbeat() {
    if (!mqtt.connected()) return;

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"type\":\"datalogger\","
             "\"autoclaveName\":\"%s\",\"autoclaveSerial\":\"%s\","
             "\"cabinetId\":\"%s\",\"uptime\":%lu,\"rssi\":%d,"
             "\"heartbeats\":%lu,\"fwVersion\":\"%s\",\"prov\":\"ble\"}",
             deviceId.c_str(), cfgAutoclaveName.c_str(),
             cfgAutoclaveSerial.c_str(), cfgCabinetId.c_str(),
             millis() / 1000, WiFi.RSSI(),
             totalHeartbeats, FW_VERSION);

    mqtt.publish(TOPIC_HEARTBEAT, payload);
    totalHeartbeats++;
    Serial.printf("[HB] %s\n", payload);
}

void connectMqtt() {
    if (WiFi.status() != WL_CONNECTED) return;

    net.setInsecure();
    mqtt.setServer(cfgMqttHost.c_str(), cfgMqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);

    unsigned long t0 = millis();
    while (!mqtt.connected() && millis() - t0 < 10000) {
        Serial.print("MQTT...");
        if (mqtt.connect(deviceId.c_str(), cfgMqttUser.c_str(), cfgMqttPass.c_str(),
                         TOPIC_STATUS, 1, true, lwtMessage.c_str())) {
            Serial.println(" OK");
            mqtt.publish(TOPIC_STATUS, onlineMessage.c_str(), true);
            mqtt.subscribe(TOPIC_COMMAND.c_str(), 1);
            Serial.printf("[MQTT] Subscribed to %s\n", TOPIC_COMMAND.c_str());
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

// ═══════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════

void setup()
{
    M5.begin();
    M5.update();
    Serial.begin(115200);

    delay(1500);
    Serial.println("\n========== BOOT DATALOGGER BLE ==========");

    M5.Display.setRotation(3);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextSize(1);
    M5.Display.setTextScroll(true);

    // Triple-reset
    if (checkTripleReset()) {
        Serial.println("[RESET] Triple reset detecte -> effacement NVS");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED);
        M5.Display.setCursor(5, 40);
        M5.Display.println("RESET CONFIG");
        M5.Display.println("BLE");
        clearNVS();
        clearResetCounter();
        delay(1500);
        ESP.restart();
    }

    // Identité
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char idBuf[24], bleBuf[24];
    snprintf(idBuf,  sizeof(idBuf),  "datalogger-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(bleBuf, sizeof(bleBuf), "STERY-DL-%02X%02X%02X",   mac[3], mac[4], mac[5]);
    deviceId     = idBuf;
    bleAdvName   = bleBuf;
    TOPIC_COMMAND = String("datalogger/command/") + deviceId;
    Serial.printf("[BOOT] %s  (BLE: %s)\n", deviceId.c_str(), bleAdvName.c_str());
    Serial.printf("[BOOT] cmd topic: %s\n", TOPIC_COMMAND.c_str());

    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.println(deviceId.c_str());

    // Compteur reset
    prefs.begin("boot-dl", true);
    uint32_t bootCnt = prefs.getUInt("cnt", 0);
    prefs.end();
    if (bootCnt > 0) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(bootCnt >= 2 ? TFT_ORANGE : TFT_DARKGREY);
        M5.Display.printf("reset %u/3\n", bootCnt);
        M5.Display.setFont(&fonts::Font4);
    }

    provisioned = loadConfigFromNVS();

    lwtMessage    = "{\"status\":\"offline\",\"device\":\"" + deviceId + "\",\"type\":\"datalogger\""
                  + (cfgAutoclaveSerial.length() > 0
                     ? ",\"autoclaveSerial\":\"" + cfgAutoclaveSerial + "\"}"
                     : "}");
    onlineMessage = "{\"status\":\"online\",\"device\":\"" + deviceId + "\",\"type\":\"datalogger\""
                  + (cfgAutoclaveSerial.length() > 0
                     ? ",\"autoclaveSerial\":\"" + cfgAutoclaveSerial + "\"}"
                     : "}");

    if (!provisioned) {
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setCursor(5, 10);
        M5.Display.setFont(&fonts::Font4);
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.println("PROVISIONING");
        M5.Display.println("BLE");
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.println("");
        M5.Display.println("Connecter depuis l'iPad:");
        M5.Display.setTextColor(TFT_CYAN);
        M5.Display.println(bleAdvName.c_str());
        M5.Display.setFont(&fonts::Font4);

        startBleProvisioning();
        return;  // loop() s'occupe de l'attente du write + reboot
    }

    Serial.printf("[BOOT] Config NVS — WiFi: %s, MQTT: %s, Cabinet: %s\n",
                  cfgWifiSSID.c_str(), cfgMqttHost.c_str(), cfgCabinetId.c_str());

    connectWifi();
    if (WiFi.status() != WL_CONNECTED) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.println("\nWiFi KO — reconnexion auto");
        M5.Display.setFont(&fonts::Font4);
    }

    connectMqtt();

    delay(1000);
    showIdleScreen();

    Serial.println("[BOOT] Datalogger BLE pret — heartbeats only");
}

// ═══════════════════════════════════════════════════════════
// Loop
// ═══════════════════════════════════════════════════════════

void loop()
{
    M5.update();

    static unsigned long loopStartedAt = millis();
    static bool resetCounterCleared = false;
    if (!resetCounterCleared && millis() - loopStartedAt > 10000) {
        clearResetCounter();
        resetCounterCleared = true;
        Serial.println("[BOOT] Reset counter cleared");
    }

    // ─── Mode provisioning : attente du write BLE ───
    if (!provisioned) {
        // Reboot après ACK envoyé (laisser à l'iPad le temps de recevoir le notify)
        if (blePendingReboot) {
            delay(1000);
            ESP.restart();
        }

        // Timeout buffer si l'iPad envoie un fragment partiel puis disparait
        if (bleRxBuffer.length() > 0 && millis() - bleRxFirstAt > BLE_RX_TIMEOUT) {
            Serial.println("[BLE] Buffer timeout — purge");
            bleRxBuffer = "";
        }

        delay(50);
        return;
    }

    // ─── Mode normal : MQTT heartbeats ───
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();

    static unsigned long lastWifiCheck = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiCheck > WIFI_RECONNECT_DELAY) {
        lastWifiCheck = millis();
        Serial.println("[WIFI] Reconnexion...");
        WiFi.reconnect();
    }

    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    // ─── Extinction ecran apres inactivite ───
    if (screenOn && millis() - lastActivity > SCREEN_TIMEOUT) {
        sleepScreen();
    }

    // Toucher ecran = reveil + refresh statut
    auto t = M5.Touch.getDetail();
    if (t.wasPressed()) {
        wakeScreen();
        showIdleScreen();
    }

    delay(10);
}
