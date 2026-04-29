# m5datalogger — notes projet

Data logger basé sur **M5Stack CoreS3** avec module scanner QR pour le provisioning. Partage l'écosystème du projet frère `m5scanner` (../m5scanner/).

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
{ "w":"ssid", "wp":"pass", "h":"mqttHost", "p":8883,
  "u":"mqttUser", "mp":"mqttPass", "s":"id", "c":"cabinetId" }
```
Obligatoires : `w`, `h`, `u`, `mp`, `s`. Stocké en NVS namespace `"stery"`.

**Reprovisioning** : triple-RESET rapide (3 appuis RESET dans une fenêtre de ~10 s après le boot) → `clearNVS()` + reboot. Pattern bien plus fiable que `M5.BtnPWR.pressedFor()` sur CoreS3.

## Debug

Le Serial monitor sur CoreS3 composite USB peut muter sans prévenir. **Toujours prévoir un fallback de diagnostic à l'écran** (320×240, ~15 lignes en Font2) pour les erreurs critiques (WiFi KO, MQTT KO, init hardware raté). Exemples dans `../m5scanner/src/main.cpp` (`connectWifi()` affiche SSID attendu, longueur password, liste des réseaux visibles).

## Architecture flotte Stery

```
M5 devices (scanner + datalogger) --MQTT/TLS--> HiveMQ Cloud --MQTT--> iPadOS app --> DB
```

Pas de bridge serveur (Firebase/Cloud Functions abandonnés). L'app iPad est le seul client MQTT côté consommation, elle écrit en DB elle-même. Toute évolution du payload côté firmware impacte directement le code Swift.

## Libs PlatformIO

Dans `platformio.ini` :
- `m5stack/M5Unified @ ^0.2.0`
- `https://github.com/m5stack/M5Module-QRCode`
- `knolleary/PubSubClient @ ^2.8` (MQTT)
- `bblanchon/ArduinoJson @ ^7.0.0` (syntaxe v7 : `JsonDocument`, pas `StaticJsonDocument`)

## Projet frère

`../m5scanner/` : scanner HID+MQTT terminé, sert de référence pour le provisioning, le pattern triple-reset, la gestion NVS, et le diagnostic WiFi à l'écran.
