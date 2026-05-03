# m5datalogger — notes projet

Data logger basé sur **M5Stack CoreS3** avec module scanner QR pour le provisioning. Partage l'écosystème du projet frère `m5scanner` (../m5scanner/).

## Variantes firmware

Quatre envs PlatformIO cohabitent. Le firmware de référence pour la prod
est **`datalogger-qos1`** (RS-232 + QR + buffer LittleFS + MQTT QoS 1).

| Env PIO | Source | MQTT | Buffer cycles | Provisioning |
|---|---|---|---|---|
| `m5stack-cores3` | `main_datalogger.cpp` | PubSubClient (QoS 0) | ❌ | QR |
| `datalogger-ble` | `main_datalogger_ble.cpp` | PubSubClient (QoS 0) | n/a (pas de RS-232) | BLE GATT |
| `datalogger-buffered` | `main_datalogger_buffered.cpp` | PubSubClient (QoS 0) | LittleFS | QR |
| **`datalogger-qos1`** | **`main_datalogger_qos1.cpp`** | **arduino-mqtt (QoS 1)** | **LittleFS** | QR |

`datalogger-qos1` ajoute :
- Publish des cycles en **QoS 1** (broker ACK chaque message)
- **Session persistante** côté broker (`clean_session=false`) → broker bufferise
  les commandes vers le device pendant ses déconnexions
- Keep-alive **15 s** → détection offline ~22s côté broker (LWT) au lieu de 90s
- **Parser cycle number** depuis le ticket RS-232 (15 patterns : `CYCLE NUMBER`,
  `CYCLE NUMERO`, `N° de cycle`, etc.) — ajoute `cycleNumber` au heartbeat et
  affiche `#XXX` à l'écran idle

Voir `BUFFER.md` (mécanisme buffer LittleFS) et `OTA.md` (procédure OTA).

## Hardware

- **Board:** M5Stack CoreS3 (ESP32-S3, PMU AXP2101, écran 320×240 tactile, USB-C)
- **Module QR:** M5Module-QRCode (M14, UART sur TX=17 / RX=18, baudrate 115200)
- **Boutons physiques:** RESET (côté SD) + POWER (côté USB-C)
  - RESET = hardware pur, non lisible en code — déclenche un reboot
  - POWER = passe par le PMU AXP2101, IRQ consommée au wake → **`M5.BtnPWR` n'est pas fiable** pour détecter un appui au boot ou pendant l'opération

## Gotchas ESP32-S3 / CoreS3

- **WiFi 2.4 GHz only** — aucun support 5 GHz côté chip. En cas de `WL_NO_SSID_AVAIL` (status=1), vérifier la bande du SSID sur la box.
- **USB composite HID+CDC fragile:**
  - Avec `ARDUINO_USB_CDC_ON_BOOT=1`, `Serial` est démarré automatiquement. **Ne PAS appeler `USB.begin()` manuellement** — ça casse le CDC déjà attaché au host et le monitor devient muet.
  - Après un flash, le device ré-énumère → il faut souvent débrancher/rebrancher ou attendre 10-15 s pour que `pio device monitor` récupère.
  - Prévoir un `delay(1500)` en début de `setup()` avant les premiers `Serial.print` pour laisser le host rattacher le CDC.
- **`WiFi.scanNetworks()` échoue après un `WiFi.begin()` timeout** — le radio reste dans un état bizarre. Pattern correct :
  ```cpp
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.mode(WIFI_STA);
  delay(100);
  int n = WiFi.scanNetworks(false, true);  // blocking, show hidden
  ```

## Provisioning (partagé avec m5scanner)

QR JSON clés courtes généré par l'app iPad Stery :
```json
{ "t":"datalogger", "w":"ssid", "wp":"pass", "h":"mqttHost", "p":8883,
  "u":"mqttUser", "mp":"mqttPass", "c":"cabinetId",
  "an":"autoclaveName", "as":"autoclaveSerial", "br":9600 }
```
Obligatoires : `w`, `h`, `u`, `mp`. `t` doit valoir `"datalogger"` (filtre côté
firmware pour distinguer du QR scanner). Stocké en NVS namespace **`"stery-dl"`**
(distinct du scanner qui utilise `"stery"`).

**Reprovisioning** : triple-RESET rapide (3 appuis RESET dans une fenêtre de ~10 s après le boot) → `clearNVS()` + reboot. Pattern bien plus fiable que `M5.BtnPWR.pressedFor()` sur CoreS3.

## Debug

Le Serial monitor sur CoreS3 composite USB peut muter sans prévenir. **Toujours prévoir un fallback de diagnostic à l'écran** (320×240, ~15 lignes en Font2) pour les erreurs critiques (WiFi KO, MQTT KO, init hardware raté). Exemples dans `../m5scanner/src/main.cpp` (`connectWifi()` affiche SSID attendu, longueur password, liste des réseaux visibles).

## Architecture flotte Stery

```
M5 devices (scanner + datalogger) --MQTT/TLS--> HiveMQ Cloud --MQTT--> iPadOS app --> DB
```

Pas de bridge serveur (Firebase/Cloud Functions abandonnés). L'app iPad est le seul client MQTT côté consommation, elle écrit en DB elle-même. Toute évolution du payload côté firmware impacte directement le code Swift.

## Libs PlatformIO

Communes à toutes les variantes :
- `m5stack/M5Unified @ ^0.2.0`
- `bblanchon/ArduinoJson @ ^7.0.0` (syntaxe v7 : `JsonDocument`, pas `StaticJsonDocument`)

Selon la variante :
- `https://github.com/m5stack/M5Module-QRCode` — toutes sauf `datalogger-ble`
- `knolleary/PubSubClient @ ^2.8` (QoS 0) — variantes `m5stack-cores3`, `datalogger-ble`, `datalogger-buffered`
- `256dpi/MQTT @ ^2.5.2` (QoS 1) — **`datalogger-qos1` uniquement**
- `h2zero/NimBLE-Arduino @ ^1.4.1` — `datalogger-ble` uniquement
- LittleFS (built-in arduino-esp32) — `datalogger-buffered` et `datalogger-qos1`
  (env doit déclarer `board_build.filesystem = littlefs`)

**Tentative ratée à éviter** : `esp_mqtt_client` (ESP-IDF natif) en arduino-esp32 v2.0.14
demande un trust anchor TLS strict (`MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED 0x7680`),
le bundle Mozilla n'est pas embarqué par défaut, et fournir un cert hardcodé
mène à `connect_return_code=5 NOT_AUTHORIZED` à cause d'un conflit de session
persistante. **Utiliser `arduino-mqtt`** qui ré-utilise `WiFiClientSecure setInsecure()`
exactement comme PubSubClient — TLS marche, et la lib supporte vraiment QoS 1.

## Projet frère

`../m5scanner/` : scanner HID+MQTT terminé, sert de référence pour le provisioning, le pattern triple-reset, la gestion NVS, et le diagnostic WiFi à l'écran.
