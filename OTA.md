# OTA — procédure pour les datalogger M5Stack

Mémo opérationnel pour pousser une mise à jour firmware over-the-air sur un M5Stack
CoreS3 datalogger déjà déployé. Toutes les URL sont en dur et collent à mon setup
actuel — adapter le username GitHub et le deviceId pour d'autres devices.

**Firmware de référence aujourd'hui** : `datalogger-qos1` (env PIO du même nom,
source `src/main_datalogger_qos1.cpp`). Voir `CLAUDE.md` pour les autres
variantes (`m5stack-cores3`, `datalogger-ble`, `datalogger-buffered`) qui
suivent la même procédure OTA — seul le path du `.bin` change.

## Références

- **Repo source** : https://github.com/9gb66frptt-cmyk/m5datalogger
- **Page Releases** : https://github.com/9gb66frptt-cmyk/m5datalogger/releases
- **Page Settings (visibilité)** : https://github.com/9gb66frptt-cmyk/m5datalogger/settings
- **Broker MQTT** : HiveMQ Cloud (TLS 8883), web client dans la console HiveMQ
- **Device de test** : `datalogger-B93824`
- **Topic commandes** : `datalogger/command/datalogger-B93824`
- **Topic monitoring** : `datalogger/#` (heartbeats + status)
- **Pattern URL release** : `https://github.com/9gb66frptt-cmyk/m5datalogger/releases/download/<TAG>/firmware.bin`
- **Path .bin local** (firmware de réf) : `/Users/Thibaut/vs code/m5datalogger/.pio/build/datalogger-qos1/firmware.bin`
- **Path .bin local** (BLE pour info) : `/Users/Thibaut/vs code/m5datalogger/.pio/build/datalogger-ble/firmware.bin`

## Procédure complète, version courte

1. Modifier le code (au minimum bumper `FW_VERSION` dans `main_datalogger_qos1.cpp`)
2. Build dans VS Code (icône ✓ PlatformIO, env `datalogger-qos1`)
3. **Calculer le SHA-256 juste avant l'upload** (pas en avance — PIO peut rebuild en arrière-plan) :
   ```
   shasum -a 256 .pio/build/datalogger-qos1/firmware.bin
   ```
4. Sur GitHub, créer une nouvelle Release :
   https://github.com/9gb66frptt-cmyk/m5datalogger/releases/new
   - Tag : `v1.X.Y-qos1` (ou `-ble` / `-buffered` selon l'env utilisé)
   - Title : `Datalogger 1.X.Y — <changement visible>`
   - Description : coller le SHA-256 + `FW_VERSION` exact attendu dans le heartbeat
   - Attach binaries → drag-drop `firmware.bin` depuis le Finder
   - **Publish release** (pas Save draft)
5. Si le repo est privé, **flipper en public** : Settings → Danger Zone → Change visibility → Make public
6. **Vérifier le SHA après upload** (le `.bin` peut avoir bougé entre étape 3 et 4) :
   ```
   curl -sL "https://github.com/9gb66frptt-cmyk/m5datalogger/releases/download/v1.X.Y-qos1/firmware.bin" | shasum -a 256
   ```
   Ce SHA est celui à mettre dans le payload OTA.
7. Dans la console web HiveMQ, section **Send Message** :
   - Topic : `datalogger/command/datalogger-B93824`
   - QoS : 0 (la cmd est livrée en QoS 1 si abonné en QoS 1, le min des deux)
   - Message :
     ```json
     {"cmd":"ota","url":"https://github.com/9gb66frptt-cmyk/m5datalogger/releases/download/v1.X.Y-qos1/firmware.bin","sha256":"<sha-de-l-etape-6>"}
     ```
8. Surveiller dans la zone Subscriptions (abonné à `datalogger/#`) :
   - `datalogger/status` → `{"status":"ota-started",...,"shaCheck":true}`
   - ~30-60s sans message
   - `datalogger/heartbeat` → avec la nouvelle `fwVersion`
9. Re-flipper le repo en **privé** une fois tous les devices à jour.

## Pièges déjà rencontrés et leur cause

### `OTA KO http-404` alors que l'URL marche dans Safari
Le repo est privé. GitHub renvoie 404 (et pas 403) aux requêtes anonymes sur un
repo privé. Safari fonctionne parce que les cookies de session sont envoyés ; l'ESP32
fait une requête anonyme.

→ **Fix** : passer le repo en public le temps de l'OTA (ou utiliser un PAT, voir
plus bas).

### `OTA KO sha-mismatch`
Le SHA calculé en local diffère de ce que le device a téléchargé. Cause habituelle :
PIO a rebuild silencieusement entre le moment du `shasum` et le moment de l'upload
sur GitHub. Du coup le `.bin` sur GitHub n'est pas celui qu'on a hashé.

→ **Fix** : recalculer le SHA depuis le `.bin` *téléchargé depuis GitHub* après
l'upload (étape 6 ci-dessus). C'est la source de vérité.

### `OTA KO http-302` ou erreur après redirect
Pas vu en pratique avec le code actuel (`runOta` suit les redirects à la main),
mais si ça revient un jour, vérifier que le User-Agent est bien envoyé et que la
release est publique.

### Pas de réaction du M5 après publish MQTT
Vérifier dans la zone Subscriptions HiveMQ que des heartbeats arrivent bien. Si oui,
relire le topic exact (deviceId case-sensitive). Si non, le M5 n'est pas connecté à
MQTT — touch screen pour réveiller, ou cycle d'alimentation.

## Vérifications utiles depuis le Mac

```bash
# Le repo est-il accessible anonymement (= public) ?
curl -sI "https://github.com/9gb66frptt-cmyk/m5datalogger" | head -1
# HTTP/2 200 = public ; HTTP/2 404 = privé

# La release est-elle bien publiée et téléchargeable ?
curl -sLI "https://github.com/9gb66frptt-cmyk/m5datalogger/releases/download/v1.X.Y-ble/firmware.bin" | grep -E "^(HTTP|content-length)"

# SHA du .bin réellement servi par GitHub
curl -sL "https://github.com/9gb66frptt-cmyk/m5datalogger/releases/download/v1.X.Y-ble/firmware.bin" | shasum -a 256
```

## Quand sortir du flip-flop public/privé

À envisager le jour où :
- Plusieurs dataloggers à mettre à jour en parallèle, supervision pénible
- OTA déclenché depuis une CI, donc impossible de surveiller manuellement
- App de monitoring qui pousse les OTAs auto

Trois options propres :

1. **Repo dédié firmware public** (ex: `m5datalogger-firmware-releases`) qui ne
   contient que les `.bin` taggés. Le repo source reste privé.
2. **Personal Access Token** avec scope `Contents: Read`, stocké en NVS au
   provisioning, envoyé en header `Authorization: Bearer <token>` dans `runOta`.
   Demande une modif firmware (nouveau champ NVS + header dans la requête HTTP).
3. **CDN tiers** (Cloudflare R2, S3, Vercel) public dédié au firmware. Le code
   reste sur GitHub privé.

## Format du payload de commande MQTT

```json
{
  "cmd": "ota",
  "url": "https://github.com/<user>/<repo>/releases/download/<tag>/firmware.bin",
  "sha256": "<64 hex chars>"
}
```

Champs obligatoires : `cmd`, `url`. `sha256` est optionnel mais **fortement
recommandé** — sans lui, n'importe quel binaire à l'URL est accepté, ce qui
ouvre la porte à tout attaquant ayant accès à HiveMQ.

Le device répond sur `datalogger/status` :
- `{"status":"ota-started","device":"...","fwVersion":"<ancienne>","shaCheck":true|false}` au début
- Reboot et nouveau heartbeat avec la nouvelle `fwVersion` en cas de succès
- Ou erreur affichée à l'écran (en parallèle des logs Serial USB) en cas d'échec
