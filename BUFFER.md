# Buffer LittleFS — résilience des cycles RS-232

Mécanisme de persistance flash partagé par les variantes
**`datalogger-buffered`** et **`datalogger-qos1`** (les deux contiennent le
même code de buffer, seule la couche MQTT diffère — voir `CLAUDE.md` pour
la matrice des variantes). Identique fonctionnellement à `m5stack-cores3`
(RS-232 + provisioning QR), mais avec persistance des cycles d'autoclave
en flash quand MQTT est down.

> **Recommandé en prod** : `datalogger-qos1` (buffer LittleFS + MQTT QoS 1
> + session persistante). `datalogger-buffered` reste utile comme fallback
> stable si arduino-mqtt pose problème un jour.

## Pourquoi cette variante existe

`main_datalogger.cpp` envoie chaque cycle directement via `mqtt.publish()`. Si
le réseau est défaillant au moment précis du publish, **le cycle est perdu** :
les données ne sont pas stockées, pas retentées. C'est inacceptable pour une
trace de cycle de stérilisation qui doit être archivée.

`main_datalogger_buffered.cpp` ajoute une couche de persistance LittleFS :
quand le publish échoue, le payload JSON est écrit dans `/pending/c_<seq>.json`
en flash. Quand MQTT redevient OK, le firmware republie un fichier toutes les
5 secondes jusqu'à vider la queue.

## Architecture

```
┌───────────────────────┐
│ Cycle terminé         │
│ (3s silence RS-232)   │
└──────────┬────────────┘
           ▼
   ┌───────────────┐    OK     ┌────────────────┐
   │ mqtt.publish  ├──────────►│ totalCyclesSent│
   │ TOPIC_CYCLE   │           │ ++ ; affichage │
   └──────┬────────┘           └────────────────┘
          │ KO
          ▼
   ┌─────────────────┐
   │ bufferCycle()   │
   │ /pending/c_N.json│
   └─────────────────┘

   loop() toutes les 5s, si MQTT OK :
   ┌──────────────────┐    OK    ┌────────────────┐
   │ flushOnePending  ├─────────►│ remove fichier │
   │ lit + publie     │          │ totalCyclesSent│
   └──────────────────┘          └────────────────┘
```

### Constantes (à ajuster si besoin)

- `MAX_PENDING = 100` — au-delà, les nouveaux cycles sont **jetés** (FIFO drop :
  on garde les anciens, on perd les récents). À ~1.5 KB par cycle, ça plafonne
  à ~150 KB de flash, large dans la partition LittleFS par défaut.
- `FLUSH_INTERVAL = 5000 ms` — intervalle entre tentatives de flush. Trop court
  = MQTT spam au reconnect ; trop long = vidage lent. 5 s est un bon compromis.
- `BUFFER_DIR = "/pending"` — dossier des cycles en attente.

### Format du fichier en queue

Chaque fichier `/pending/c_<seq>.json` contient le **payload exact** qui devait
être publié sur `datalogger/data` — un JSON sérialisé via ArduinoJson, avec
`device`, `timestamp`, `data`, etc. Au flush, on relit le fichier tel quel et
on le passe à `mqtt.publish()`. Donc même les cycles bufferisés conservent leur
timestamp d'origine et leur durée mesurée.

## Limites connues (volontaires pour le pilote)

1. **QoS 0 reste** — un publish "réussi" côté client n'est PAS garanti reçu par
   le broker. Si le paquet UDP/TCP est dropped après envoi, on a un faux
   succès et le fichier est supprimé. Pour vraiment garantir, il faut migrer
   vers AsyncMqttClient avec QoS 1 + ACK broker. Mémoire projet :
   `MQTT QoS 0 — dette acceptée`.

2. **Pas d'atomic-write** — si l'alim est coupée au milieu de l'écriture du
   `c_N.json`, le fichier peut être tronqué. Au flush, mqtt.publish() le
   verra comme un payload corrompu et le broker rejettera (ou pire, accepter
   un JSON invalide). Improvement futur : écrire en `.tmp` puis rename atomic.

3. **Pas de checksum côté broker** — l'iPad reçoit le payload, le décode, et
   doit dédupliquer si jamais un même cycle est envoyé deux fois (peut arriver
   si publish "réussit" côté client mais la suppression du fichier échoue).
   Solution : l'iPad dédupe sur `device + timestamp + lineCount` (clé naturelle).

4. **MAX_PENDING = 100 strict** — pas de rotation circulaire. Si la flotte
   est offline pendant des heures, les nouveaux cycles sont perdus pendant que
   les anciens occupent la flash. Si on veut une politique différente
   (jeter les vieux pour garder les nouveaux), changer la branche `n >= MAX_PENDING`.

## Tests à faire

### Test 1 — fonctionnement nominal
Cycle reçu en RS-232 avec WiFi/MQTT OK → cycle envoyé direct, écran "CYCLE
ENVOYE", `pendingCycles=0` dans le heartbeat.

### Test 2 — bufferisation
1. Couper le WiFi du M5 (désactiver le Wi-Fi de la box ou éteindre le routeur)
2. Faire passer un cycle d'autoclave (ou simuler avec un envoi manuel sur l'UART
   à 9600 baud + 3s silence)
3. Écran doit afficher "CYCLE BUFFERISE" en orange
4. Le heartbeat suivant (s'il arrive — il faut WiFi minimum pour qu'il sorte)
   ne marchera pas non plus, mais à la reconnexion : `pendingCycles=1`

### Test 3 — flush automatique
1. État précédent : 1 cycle en queue
2. Rétablir le WiFi
3. Dans les 5 s, flushOnePending() détecte la connexion et republie. Logs
   Serial : `[BUF] flush /pending/c_0000000000.json (...) [BUF] flush OK`
4. `pendingCycles=0` dans le heartbeat suivant

### Test 4 — persistance au reboot
1. Bufferiser 3 cycles (couper réseau, faire 3 cycles)
2. Couper l'alim du M5 (débrancher USB ou éteindre via PMU long-press)
3. Rebrancher : au boot, log `[BUF] 3 cycles en attente au boot (next seq=3)`
4. À la reconnexion MQTT, les 3 cycles sont republiés un par un toutes les 5 s

### Test 5 — overflow
1. Désactiver le réseau
2. Bufferiser 100 cycles (long, à automatiser via UART Mac→M5)
3. 101e cycle : écran "CYCLE PERDU" en rouge, log `[BUF] Buffer plein`
4. Rétablir réseau, vérifier que les 100 partent correctement

## Champs ajoutés au heartbeat

```json
{
  "device": "datalogger-XXXXXX",
  "type": "datalogger",
  ...
  "pendingCycles": 3,            ← NEW : taille de la queue locale
  "fwVersion": "1.0.0-buffered"  ← NEW : version firmware
}
```

L'app iPad / outil de monitoring doit consommer `pendingCycles` :
- = 0 → tout va bien
- 1-10 → coupure récente, en train de rattraper
- > 10 → coupure prolongée ou problème de connectivité, alerter
- = MAX_PENDING (100) → buffer saturé, pertes en cours, **alerter en URGENT**

## États écran ajoutés

| Écran | Couleur | Sens |
|---|---|---|
| `CYCLE ENVOYE` | vert | publish direct OK |
| `CYCLE BUFFERISE` | orange | publish KO, persisté en flash, retry auto |
| `CYCLE PERDU` | rouge | buffer plein OU FS HS, cycle non sauvé |

## Build et déploiement

```bash
# Build
pio run -e datalogger-buffered

# Flash USB (1ère fois ou re-flash)
pio run -e datalogger-buffered -t upload

# Path du .bin
.pio/build/datalogger-buffered/firmware.bin
```

L'OTA fonctionne identiquement à la variante BLE : Release GitHub +
commande MQTT sur `datalogger/command/datalogger-XXXXXX`. Voir `OTA.md`
pour la procédure complète.

## Migration QoS 1 — faite

Cette section listait initialement les conditions de migration vers QoS 1.
**C'est fait** dans la variante `datalogger-qos1` (lib `256dpi/arduino-mqtt`,
publish cycles en QoS 1, session persistante côté broker, keep-alive 15 s
pour détection LWT rapide). Le buffer LittleFS reste en place comme filet
de sécurité.

Tentative initiale avec `esp_mqtt_client` (ESP-IDF natif) abandonnée :
arduino-esp32 v2.0.14 exige un trust anchor TLS strict, le bundle Mozilla
n'est pas embarqué par défaut, et après ajout d'un cert hardcodé on tombait
sur des conflits de session persistante (`connect_return_code=5`).
`arduino-mqtt` ré-utilise `WiFiClientSecure setInsecure()` exactement comme
PubSubClient → TLS marche, et la lib supporte vraiment QoS 1.

## Coexistence avec les autres variantes

Quatre envs cohabitent dans `platformio.ini` :

| Env | Source | Provisioning | Buffer | MQTT |
|---|---|---|---|---|
| `m5stack-cores3` | `main_datalogger.cpp` | QR | ❌ | PubSubClient (QoS 0) |
| `datalogger-ble` | `main_datalogger_ble.cpp` | BLE | n/a | PubSubClient (QoS 0) |
| `datalogger-buffered` | `main_datalogger_buffered.cpp` | QR | ✅ | PubSubClient (QoS 0) |
| **`datalogger-qos1`** | **`main_datalogger_qos1.cpp`** | **QR** | **✅** | **arduino-mqtt (QoS 1)** |

`datalogger-qos1` est le firmware de référence pour la prod. Garder les autres
envs permet un rollback rapide par re-flash USB si nécessaire.

## Référence rapide

- **Partition** : LittleFS par défaut sur ESP32 Arduino, ~1.5 MB libres
- **Topic d'output** : `datalogger/data` (identique entre toutes les variantes)
- **Format payload cycle** : JSON avec `device`, `type:"rs232-cycle"`, `cabinetId`,
  `autoclaveName/Serial`, `timestamp`, `data` (ticket RS-232 brut),
  `durationSeconds`, `lineCount`, `cycleNumber` (si parsé)
- **SHA `.bin`** : à recalculer à chaque build (voir `OTA.md` étape 6)
- **NVS namespace** : `stery-dl` (partagé avec `m5stack-cores3`, donc le
  reprov fonctionne entre les deux)
