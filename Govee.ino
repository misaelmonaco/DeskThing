// ===========================================================================
// GOVEE OPENAPI v2 - controllo striscia LED (https://openapi.api.govee.com)
// ---------------------------------------------------------------------------
// Integrazione con le API Govee v2 tramite tre endpoint REST:
//   - GET  /router/api/v1/user/devices   scoperta automatica di SKU e MAC
//   - POST /router/api/v1/device/state   lettura stato (on/off, lum., colore)
//   - POST /router/api/v1/device/control invio comandi (powerSwitch, brightness,
//                                        colorRgb, colorTemperatureK)
//
// Tutte le chiamate passano da goveeHttpGet() e goveeHttpPost(), autenticate
// con l'header Govee-API-Key letto da Secrets.h. Come per Spotify, il modulo
// interroga la rete solo quando la pagina Govee e' visibile a schermo, cosi'
// l'handshake TLS non rallenta i tick dell'orologio o del Pomodoro.
// ===========================================================================

// Buffer di ricezione compatto per le risposte Govee v2 (risparmia RAM statica).
static char goveeBody[2048];

static const char *NVS_GOVEE_SKU    = "sku";
static const char *NVS_GOVEE_DEVICE = "device";
static const char *NVS_GOVEE_NAME   = "name";

static const uint8_t GOVEE_BRIGHT_STEPS[5] = {20, 40, 60, 80, 100};

static const GoveePreset GOVEE_PRESETS[] = {
  {"BIANCO CALDO",  "2700 K - RELAX",   255, 184, 108, 2700},
  {"LUCE GIORNO",   "5500 K - STUDIO",  232, 244, 255, 5500},
  {"AMBRA SOFFUSA", "2000 K - SERA",    224, 162,  74, 2000},
  {"BLU OCEANO",    "RGB - AMBIENTE",     0, 120, 255,    0},
  {"VIOLA CYBER",   "RGB - GAMING",     168,  85, 247,    0},
  {"VERDE FORESTA", "RGB - NATURA",      34, 197,  94,    0},
  {"ROSSO CORALLO", "RGB - CALDO",      244,  63,  94,    0},
};

constexpr uint8_t GOVEE_PRESET_COUNT = sizeof(GOVEE_PRESETS) / sizeof(GOVEE_PRESETS[0]);

uint8_t goveePresetCount() {
  return GOVEE_PRESET_COUNT;
}

const GoveePreset &goveeGetPreset(uint8_t index) {
  if (index >= GOVEE_PRESET_COUNT) {
    index = 0;
  }
  return GOVEE_PRESETS[index];
}

// Ricava l'indice (0..4) dello step di luminosita' piu' vicino al valore 1..100.
static uint8_t goveeNearestBriStep(uint8_t bri) {
  uint8_t bestIdx = 0;
  int16_t bestDiff = 1000;
  for (uint8_t i = 0; i < 5; ++i) {
    int16_t diff = static_cast<int16_t>(bri) - static_cast<int16_t>(GOVEE_BRIGHT_STEPS[i]);
    if (diff < 0) {
      diff = -diff;
    }
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIdx = i;
    }
  }
  return bestIdx;
}

// Converte una temperatura Kelvin approssimata in una terna RGB per l'anteprima
// nel riquadro colore quando il colore proviene dall'app Govee.
static void goveeKelvinToRgb(uint16_t k, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (k <= 2200) {
    r = 224; g = 162; b = 74;
  } else if (k <= 3200) {
    r = 255; g = 184; b = 108;
  } else if (k <= 4500) {
    r = 255; g = 220; b = 180;
  } else {
    r = 232; g = 244; b = 255;
  }
}

// Genera un requestId univoco per le richieste POST Govee v2.
static void goveeNewRequestId(char *out, size_t size) {
  snprintf(out, size, "esp32-%08lx%04x",
           static_cast<unsigned long>(millis()),
           static_cast<unsigned>(esp_random() & 0xFFFF));
}

// ---------------------------------------------------------------------------
// PERSISTENZA NVS DEL DISPOSITIVO SCOPERTO
// ---------------------------------------------------------------------------
static void storeGoveeDevice() {
  Preferences prefs;
  if (!prefs.begin(GOVEE_NVS_NAMESPACE, false)) {
    return;
  }
  prefs.putString(NVS_GOVEE_SKU, govee.sku);
  prefs.putString(NVS_GOVEE_DEVICE, govee.device);
  prefs.putString(NVS_GOVEE_NAME, govee.deviceName);
  prefs.end();
}

void initGovee() {
  govee.apiKeyPresent = (GOVEE_API_KEY != nullptr && GOVEE_API_KEY[0] != '\0');
  if (!govee.apiKeyPresent) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage),
             "Imposta GOVEE_API_KEY in Secrets.h");
    return;
  }

  Preferences prefs;
  if (prefs.begin(GOVEE_NVS_NAMESPACE, true)) {
    readNvsText(prefs, NVS_GOVEE_SKU, govee.sku, sizeof(govee.sku));
    readNvsText(prefs, NVS_GOVEE_DEVICE, govee.device, sizeof(govee.device));
    readNvsText(prefs, NVS_GOVEE_NAME, govee.deviceName, sizeof(govee.deviceName));
    prefs.end();
  }

  if (govee.sku[0] != '\0' && govee.device[0] != '\0') {
    govee.deviceFound = true;
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Pronto");
  } else {
    govee.deviceFound = false;
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Ricerca dispositivo...");
  }
}

// ---------------------------------------------------------------------------
// TRASPORTO HTTP / HTTPS VERSO GOVEE OPENAPI v2
// ---------------------------------------------------------------------------
static bool goveeReadResponseBody(HTTPClient &http, int status, const char *path) {
  int declared = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long deadline = millis() + GOVEE_HTTP_TIMEOUT_MS;

  while (got + 1 < sizeof(goveeBody)) {
    if (millis() > deadline) {
      break;
    }

    int available = stream->available();
    if (available <= 0) {
      if (!http.connected() && available <= 0) {
        break;
      }
      delay(5);
      continue;
    }

    size_t room = sizeof(goveeBody) - 1 - got;
    size_t take = available < static_cast<int>(room) ? static_cast<size_t>(available) : room;
    int read = stream->readBytes(goveeBody + got, take);
    if (read > 0) {
      got += static_cast<size_t>(read);
      deadline = millis() + GOVEE_HTTP_TIMEOUT_MS;
    }

    if (declared > 0 && got >= static_cast<size_t>(declared)) {
      break;
    }
  }

  goveeBody[got] = '\0';
  Serial.printf("[govee] %s -> HTTP %d, %u byte\n", path, status, static_cast<unsigned>(got));
  return got > 0;
}

bool goveeHttpGet(const char *path, JsonDocument &doc, JsonDocument &filter) {
  if (WiFi.status() != WL_CONNECTED) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "WiFi assente");
    return false;
  }

  if (!govee.apiKeyPresent) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "API Key mancante");
    return false;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s%s", GOVEE_BASE_URL, path);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(GOVEE_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(GOVEE_HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "URL Govee non valido");
    return false;
  }

  http.addHeader("Govee-API-Key", GOVEE_API_KEY);
  http.addHeader("Content-Type", "application/json");

  int status = http.GET();
  govee.lastStatusCode = status;

  if (status != HTTP_CODE_OK) {
    if (status == 429) {
      snprintf(govee.statusMessage, sizeof(govee.statusMessage), "Rate limit Govee (429)");
      govee.lastPollMs = millis() + 45000UL; // Pausa 45s prima di riprovare
    } else {
      snprintf(govee.statusMessage, sizeof(govee.statusMessage), "Errore HTTP %d", status);
    }
    Serial.printf("[govee] GET %s fallita -> HTTP %d\n", path, status);
    http.end();
    return false;
  }

  bool hasBody = goveeReadResponseBody(http, status, path);
  http.end();

  if (!hasBody) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Risposta vuota");
    return false;
  }

  DeserializationError error = deserializeJson(doc, goveeBody,
                                               DeserializationOption::Filter(filter));
  if (error || !doc.is<JsonObject>()) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "JSON Govee illeggibile");
    Serial.printf("[govee] errore JSON: %s\n", error.c_str());
    return false;
  }

  return true;
}

bool goveeHttpPost(const char *path, const char *payloadJson,
                   JsonDocument &doc, JsonDocument &filter) {
  if (WiFi.status() != WL_CONNECTED) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "WiFi assente");
    return false;
  }

  if (!govee.apiKeyPresent) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "API Key mancante");
    return false;
  }

  char url[128];
  snprintf(url, sizeof(url), "%s%s", GOVEE_BASE_URL, path);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(GOVEE_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(GOVEE_HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "URL Govee non valido");
    return false;
  }

  http.addHeader("Govee-API-Key", GOVEE_API_KEY);
  http.addHeader("Content-Type", "application/json");

  int status = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(payloadJson)),
                         strlen(payloadJson));
  govee.lastStatusCode = status;

  if (status != HTTP_CODE_OK) {
    if (status == 429) {
      snprintf(govee.statusMessage, sizeof(govee.statusMessage), "Rate limit Govee (429)");
      govee.lastPollMs = millis() + 45000UL;
    } else {
      snprintf(govee.statusMessage, sizeof(govee.statusMessage), "Errore HTTP %d", status);
    }
    Serial.printf("[govee] POST %s fallita -> HTTP %d\n", path, status);
    http.end();
    return false;
  }

  bool hasBody = goveeReadResponseBody(http, status, path);
  http.end();

  if (!hasBody) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Risposta vuota");
    return false;
  }

  DeserializationError error = deserializeJson(doc, goveeBody,
                                               DeserializationOption::Filter(filter));
  if (error || !doc.is<JsonObject>()) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "JSON Govee illeggibile");
    Serial.printf("[govee] errore JSON: %s\n", error.c_str());
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// ENDPOINT 1: LISTA DISPOSITIVI (GET /router/api/v1/user/devices)
// ---------------------------------------------------------------------------
bool goveeDiscoverDevice() {
  JsonDocument filter;
  filter["code"] = true;
  filter["message"] = true;
  filter["data"][0]["sku"] = true;
  filter["data"][0]["device"] = true;
  filter["data"][0]["deviceName"] = true;
  filter["data"][0]["type"] = true;

  JsonDocument doc;
  if (!goveeHttpGet("/router/api/v1/user/devices", doc, filter)) {
    return false;
  }

  JsonArray list = doc["data"].as<JsonArray>();
  if (list.isNull() || list.size() == 0) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Nessun dispositivo trovato");
    return false;
  }

  // Sceglie il primo dispositivo luce (o il primo in assoluto).
  JsonVariant chosen = list[0];
  for (JsonVariant item : list) {
    const char *type = item["type"] | "";
    if (strstr(type, "light") != nullptr) {
      chosen = item;
      break;
    }
  }

  const char *sku  = chosen["sku"] | "";
  const char *dev  = chosen["device"] | "";
  const char *name = chosen["deviceName"] | "STRISCIA GOVEE";

  if (sku[0] == '\0' || dev[0] == '\0') {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Dati dispositivo incompleti");
    return false;
  }

  copyText(govee.sku, sizeof(govee.sku), sku);
  copyText(govee.device, sizeof(govee.device), dev);
  copyText(govee.deviceName, sizeof(govee.deviceName), name);
  govee.deviceFound = true;

  storeGoveeDevice();

  Serial.printf("[govee] trovato %s (SKU: %s, MAC: %s)\n",
                govee.deviceName, govee.sku, govee.device);
  return true;
}

// ---------------------------------------------------------------------------
// ENDPOINT 2: LETTURA STATO (POST /router/api/v1/device/state)
// ---------------------------------------------------------------------------
bool goveeFetchState() {
  if (!govee.deviceFound && !goveeDiscoverDevice()) {
    return false;
  }

  char reqId[32];
  goveeNewRequestId(reqId, sizeof(reqId));

  char payload[192];
  snprintf(payload, sizeof(payload),
           "{\"requestId\":\"%s\",\"payload\":{\"sku\":\"%s\",\"device\":\"%s\"}}",
           reqId, govee.sku, govee.device);

  JsonDocument filter;
  filter["code"] = true;
  filter["payload"]["capabilities"][0]["instance"] = true;
  filter["payload"]["capabilities"][0]["state"]["value"] = true;

  JsonDocument doc;
  if (!goveeHttpPost("/router/api/v1/device/state", payload, doc, filter)) {
    return false;
  }

  JsonArray caps = doc["payload"]["capabilities"].as<JsonArray>();
  if (caps.isNull()) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), "Stato non disponibile");
    return false;
  }

  uint32_t fetchedRgb = 0;
  bool hasRgb = false;
  uint16_t fetchedKelvin = 0;

  for (JsonVariant cap : caps) {
    const char *inst = cap["instance"] | "";
    JsonVariant val = cap["state"]["value"];

    if (strcmp(inst, "online") == 0) {
      if (val.is<bool>()) {
        govee.online = val.as<bool>();
      }
    } else if (strcmp(inst, "powerSwitch") == 0) {
      govee.powerOn = (val.as<int>() != 0);
    } else if (strcmp(inst, "brightness") == 0) {
      int b = val.as<int>();
      if (b > 0 && b <= 100) {
        govee.brightness = static_cast<uint8_t>(b);
        govee.briStepIndex = goveeNearestBriStep(govee.brightness);
      }
    } else if (strcmp(inst, "colorRgb") == 0) {
      fetchedRgb = val.as<uint32_t>();
      hasRgb = (fetchedRgb > 0);
    } else if (strcmp(inst, "colorTemperatureK") == 0) {
      int k = val.as<int>();
      if (k > 0) {
        fetchedKelvin = static_cast<uint16_t>(k);
      }
    }
  }

  // Aggiorna colore e verifica se corrisponde a uno dei preset.
  if (fetchedKelvin > 0) {
    govee.kelvin = fetchedKelvin;
    goveeKelvinToRgb(fetchedKelvin, govee.r, govee.g, govee.b);

    bool matched = false;
    for (uint8_t i = 0; i < GOVEE_PRESET_COUNT; ++i) {
      if (GOVEE_PRESETS[i].kelvin > 0 &&
          abs(static_cast<int>(GOVEE_PRESETS[i].kelvin) - static_cast<int>(fetchedKelvin)) <= 250) {
        govee.presetIndex = i;
        govee.r = GOVEE_PRESETS[i].r;
        govee.g = GOVEE_PRESETS[i].g;
        govee.b = GOVEE_PRESETS[i].b;
        govee.customColor = false;
        matched = true;
        break;
      }
    }
    if (!matched) {
      govee.customColor = true;
    }
  } else if (hasRgb) {
    govee.kelvin = 0;
    govee.r = static_cast<uint8_t>((fetchedRgb >> 16) & 0xFF);
    govee.g = static_cast<uint8_t>((fetchedRgb >> 8) & 0xFF);
    govee.b = static_cast<uint8_t>(fetchedRgb & 0xFF);

    bool matched = false;
    for (uint8_t i = 0; i < GOVEE_PRESET_COUNT; ++i) {
      if (GOVEE_PRESETS[i].kelvin == 0 &&
          abs(static_cast<int>(GOVEE_PRESETS[i].r) - static_cast<int>(govee.r)) <= 16 &&
          abs(static_cast<int>(GOVEE_PRESETS[i].g) - static_cast<int>(govee.g)) <= 16 &&
          abs(static_cast<int>(GOVEE_PRESETS[i].b) - static_cast<int>(govee.b)) <= 16) {
        govee.presetIndex = i;
        govee.customColor = false;
        matched = true;
        break;
      }
    }
    if (!matched) {
      govee.customColor = true;
    }
  }

  copyText(govee.statusMessage, sizeof(govee.statusMessage),
           govee.powerOn ? "Accesa" : "In standby");
  return true;
}

// ---------------------------------------------------------------------------
// ENDPOINT 3: INVIO COMANDI (POST /router/api/v1/device/control)
// ---------------------------------------------------------------------------
static bool goveeSendControl(const char *capType, const char *instance, int32_t value) {
  if (!govee.deviceFound && !goveeDiscoverDevice()) {
    return false;
  }

  char reqId[32];
  goveeNewRequestId(reqId, sizeof(reqId));

  char payload[280];
  snprintf(payload, sizeof(payload),
           "{\"requestId\":\"%s\",\"payload\":{\"sku\":\"%s\",\"device\":\"%s\","
           "\"capability\":{\"type\":\"%s\",\"instance\":\"%s\",\"value\":%ld}}}",
           reqId, govee.sku, govee.device,
           capType, instance, static_cast<long>(value));

  JsonDocument filter;
  filter["code"] = true;
  filter["msg"] = true;

  JsonDocument doc;
  if (!goveeHttpPost("/router/api/v1/device/control", payload, doc, filter)) {
    return false;
  }

  int code = doc["code"] | 200;
  if (code != 200) {
    snprintf(govee.statusMessage, sizeof(govee.statusMessage), "Govee code %d", code);
    return false;
  }

  govee.lastPollMs = millis();
  return true;
}

bool goveeSetPower(bool on) {
  govee.powerOn = on;
  markDisplayDirty();
  renderIfNeeded();

  bool ok = goveeSendControl("devices.capabilities.on_off", "powerSwitch", on ? 1 : 0);
  if (ok) {
    copyText(govee.statusMessage, sizeof(govee.statusMessage), on ? "Accesa" : "In standby");
  }
  markDisplayDirty();
  return ok;
}

bool goveeStepBrightness() {
  govee.briStepIndex = (govee.briStepIndex + 1) % 5;
  govee.brightness = GOVEE_BRIGHT_STEPS[govee.briStepIndex];
  markDisplayDirty();
  renderIfNeeded();

  bool ok = goveeSendControl("devices.capabilities.range", "brightness", govee.brightness);
  markDisplayDirty();
  return ok;
}

bool goveeNextPreset() {
  govee.presetIndex = (govee.presetIndex + 1) % GOVEE_PRESET_COUNT;
  const GoveePreset &p = GOVEE_PRESETS[govee.presetIndex];
  govee.r = p.r;
  govee.g = p.g;
  govee.b = p.b;
  govee.kelvin = p.kelvin;
  govee.customColor = false;
  markDisplayDirty();
  renderIfNeeded();

  bool ok;
  if (p.kelvin > 0) {
    ok = goveeSendControl("devices.capabilities.color_setting", "colorTemperatureK", p.kelvin);
  } else {
    int32_t rgbVal = (static_cast<int32_t>(p.r) << 16) |
                     (static_cast<int32_t>(p.g) << 8) |
                     static_cast<int32_t>(p.b);
    ok = goveeSendControl("devices.capabilities.color_setting", "colorRgb", rgbVal);
  }

  markDisplayDirty();
  return ok;
}

// ---------------------------------------------------------------------------
// AGGIORNAMENTO PERIODICO E DIAGNOSTICA SERIALE
// ---------------------------------------------------------------------------
void refreshGoveeData() {
  if (currentPage != PAGE_GOVEE || displayDirty) {
    return;
  }

  if (!govee.apiKeyPresent || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (govee.lastPollMs != 0 && millis() - govee.lastPollMs < GOVEE_REFRESH_MS) {
    return;
  }

  bool prevFound  = govee.deviceFound;
  bool prevOnline = govee.online;
  bool prevPower  = govee.powerOn;
  uint8_t prevBri = govee.brightness;
  uint8_t prevR   = govee.r;
  uint8_t prevG   = govee.g;
  uint8_t prevB   = govee.b;
  uint16_t prevK  = govee.kelvin;

  goveeFetchState();
  govee.lastPollMs = millis();

  if (prevFound != govee.deviceFound ||
      prevOnline != govee.online ||
      prevPower != govee.powerOn ||
      prevBri != govee.brightness ||
      prevR != govee.r || prevG != govee.g || prevB != govee.b ||
      prevK != govee.kelvin) {
    markDisplayDirty();
  }
}

void printGoveeStatus() {
  Serial.println("--- govee ---");
  Serial.printf("  api key     : %s\n", govee.apiKeyPresent ? "presente" : "MANCANTE (Secrets.h)");
  Serial.printf("  dispositivo : %s\n", govee.deviceFound ? govee.deviceName : "(non associato)");
  Serial.printf("  sku / mac   : %s / %s\n",
                govee.sku[0] ? govee.sku : "-",
                govee.device[0] ? govee.device : "-");
  Serial.printf("  stato       : %s, %s, lum %u%%\n",
                govee.online ? "online" : "offline",
                govee.powerOn ? "ACCESA" : "SPENTA",
                static_cast<unsigned>(govee.brightness));
  Serial.printf("  colore      : RGB(%u, %u, %u)  %u K\n",
                govee.r, govee.g, govee.b, static_cast<unsigned>(govee.kelvin));
  Serial.printf("  messaggio   : %s (HTTP %d)\n", govee.statusMessage, govee.lastStatusCode);
}

