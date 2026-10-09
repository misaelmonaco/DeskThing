// ===========================================================================
// METEO ESTERNO - Open-Meteo
// ===========================================================================
// La citta' si sceglie nel portale, insieme alla rete WiFi: un campo di
// testo, il nome scritto come viene. La geocodifica lo traduce una volta in
// coordinate, che finiscono in NVS - da li' in poi l'avvio non ha bisogno
// della rete per sapere dove siamo, e la chiamata delle previsioni e' una
// sola, sempre la stessa.
//
// ---------------------------------------------------------------------------
// PERCHE' QUESTO MODULO NON USA TLS
// ---------------------------------------------------------------------------
// Open-Meteo risponde in HTTP semplice, e qui non e' un dettaglio: su questa
// scheda l'handshake TLS vuole circa 40 KB di heap CONTIGUO, ed e' il vincolo
// che ha fatto penare la copertina dell'album. Niente TLS vuol dire che questa
// chiamata non compete con quelle di Spotify per il blocco grande, e quindi
// non va sequenziata con loro: puo' partire quando vuole.
//
// E' anche il motivo per cui si usa WiFiClient e non WiFiClientSecure. Non e'
// una svista: non c'e' niente da proteggere - nessuna credenziale viaggia, e
// il dato che torna e' pubblico - e il prezzo del TLS sarebbe pagato in
// memoria, che e' esattamente cio' che qui scarseggia.
//
// ---------------------------------------------------------------------------
// LA RISPOSTA
// ---------------------------------------------------------------------------
// Delle previsioni interessano tre numeri su una risposta da circa 400 byte.
// Il filtro di ArduinoJson scarta il resto prima che venga costruito, quindi
// il documento in RAM sta in poche centinaia di byte invece che nei 400 del
// JSON piu' l'albero.
//
// Il campo `interval` della risposta dichiara 900 secondi: il dato a monte si
// muove ogni quarto d'ora, e WEATHER_REFRESH_MS e' tarato su quello. Chiedere
// piu' spesso non darebbe un numero diverso, darebbe solo traffico.
// ===========================================================================

#include <WiFiClient.h>

// Corpo dell'ultima risposta. Mezzo kilobyte basterebbe: queste stanno
// intorno ai 430 byte. Il doppio serve a non troncare se un giorno il
// servizio aggiunge un campo, perche' un corpo troncato non si distingue da
// uno malformato.
static char weatherBody[1024];

static const char *NVS_KEY_CITY  = "city";
static const char *NVS_KEY_PLACE = "place";
static const char *NVS_KEY_LAT   = "lat";
static const char *NVS_KEY_LON   = "lon";

// ---------------------------------------------------------------------------
// MEMORIA PERMANENTE
// ---------------------------------------------------------------------------
void loadWeatherSettings() {
  Preferences prefs;
  if (!prefs.begin(WEATHER_NVS_NAMESPACE, true)) {
    return;
  }

  String city  = prefs.getString(NVS_KEY_CITY, "");
  String place = prefs.getString(NVS_KEY_PLACE, "");
  float latitude  = prefs.getFloat(NVS_KEY_LAT, NAN);
  float longitude = prefs.getFloat(NVS_KEY_LON, NAN);
  prefs.end();

  copyText(weather.city, sizeof(weather.city), city.c_str());
  copyText(weather.place, sizeof(weather.place), place.c_str());

  // Le String di Preferences muoiono qui: tutto il resto del modulo lavora su
  // buffer fissi, e questa e' l'unica finestra in cui esistono.
  if (!isnan(latitude) && !isnan(longitude)) {
    weather.latitude = latitude;
    weather.longitude = longitude;
    weather.located = true;
  }

  if (weather.city[0] == '\0') {
    copyText(weather.message, sizeof(weather.message), "nessuna citta' scelta");
  } else if (!weather.located) {
    copyText(weather.message, sizeof(weather.message), "citta' da localizzare");
  } else {
    copyText(weather.message, sizeof(weather.message), "in attesa del primo dato");
  }
}

static void storeWeatherLocation() {
  Preferences prefs;
  if (!prefs.begin(WEATHER_NVS_NAMESPACE, false)) {
    Serial.println("[meteo] NVS non disponibile: posizione non salvata");
    return;
  }

  prefs.putString(NVS_KEY_CITY, weather.city);
  prefs.putString(NVS_KEY_PLACE, weather.place);
  prefs.putFloat(NVS_KEY_LAT, weather.latitude);
  prefs.putFloat(NVS_KEY_LON, weather.longitude);
  prefs.end();
}

// ---------------------------------------------------------------------------
// CAMPO NEL PORTALE
// ---------------------------------------------------------------------------
// Sta a livello di file per lo stesso motivo dei campi Spotify: WiFiManager ne
// conserva solo il puntatore e deve restare vivo finche' il portale e' aperto.
char weatherPortalHeadingHtml[260];

WiFiManagerParameter weatherPortalHeading(weatherPortalHeadingHtml);
WiFiManagerParameter weatherPortalCity("meteo_city", "Citta' per il meteo", "",
                                       WEATHER_CITY_MAX - 1);

void addWeatherPortalParams(WiFiManager &wifiManager) {
  loadWeatherSettings();

  snprintf(weatherPortalHeadingHtml, sizeof(weatherPortalHeadingHtml),
           "<hr><h3>Meteo</h3>"
           "<p>Facoltativo. Il nome basta: le coordinate le trova da se'.<br>"
           "Se ci sono piu' posti con lo stesso nome, aggiungi la nazione -<br>"
           "<small>Ariano Irpino, Italia</small><br>"
           "Un campo vuoto lascia la citta' attuale.</p>");

  weatherPortalCity.setValue(weather.city, WEATHER_CITY_MAX - 1);

  wifiManager.addParameter(&weatherPortalHeading);
  wifiManager.addParameter(&weatherPortalCity);
}

// Callback unica del salvataggio. WiFiManager ne tiene UNA sola, e
// addSpotifyPortalParams() ne registra gia' una propria per il portale dei
// soli campi Spotify: quando le due famiglie di campi convivono - cioe' nel
// portale della rete - questa la sostituisce e chiama entrambe. Registrarla
// dopo aver aggiunto tutti i parametri non e' un caso, e' il punto.
void storePortalParams() {
  storeSpotifyPortalParams();
  storeWeatherPortalParams();
}

// Cambia la citta' e riparte da zero. La chiamano sia il portale sia la
// console seriale: e' lo stesso gesto e deve avere lo stesso effetto.
//
// Le vecchie coordinate non valgono piu' niente, e nemmeno il dato che ne era
// venuto. Azzerando lastTryMs la geocodifica parte alla passata successiva
// invece di aspettare la scadenza del tentativo precedente.
void setWeatherCity(const char *name) {
  copyText(weather.city, sizeof(weather.city), name);

  weather.located = false;
  weather.valid = false;
  weather.place[0] = '\0';
  weather.lastTryMs = 0;
  copyText(weather.message, sizeof(weather.message), "citta' da localizzare");

  storeWeatherLocation();

  if (currentPage == PAGE_DHT) {
    markDisplayDirty();
  }
}

// Stato del meteo in chiaro sul seriale: dice in quale dei passaggi ci si e'
// fermati senza doverlo dedurre dalla riga sul display.
void printWeatherStatus() {
  Serial.println("--- meteo ---");
  Serial.printf("  citta' scelta : %s\n", weather.city[0] ? weather.city : "(nessuna)");
  Serial.printf("  nome risolto  : %s\n", weather.place[0] ? weather.place : "(non risolto)");

  if (weather.located) {
    Serial.printf("  coordinate    : %.4f, %.4f\n", weather.latitude, weather.longitude);
  } else {
    Serial.println("  coordinate    : (da trovare)");
  }

  if (weather.valid) {
    Serial.printf("  dato          : %.1f C, %u%%, codice %d (%s)\n",
                  weather.temperature, static_cast<unsigned>(weather.humidity),
                  weather.code, weatherConditionName(weather.code));
    Serial.printf("  letto alle    : %s, %lu s fa%s\n",
                  weather.lastOkClock[0] ? weather.lastOkClock : "??:??",
                  (millis() - weather.lastOkMs) / 1000,
                  weatherIsStale() ? "   [VECCHIO]" : "");
  } else {
    Serial.printf("  dato          : assente - %s\n", weather.message);
  }

  Serial.printf("  rete          : %s\n",
                WiFi.status() == WL_CONNECTED ? "connessa" : "assente");
}

void storeWeatherPortalParams() {
  char city[WEATHER_CITY_MAX];
  copyTrimmed(city, sizeof(city), weatherPortalCity.getValue());

  if (city[0] == '\0' || strcmp(city, weather.city) == 0) {
    return;
  }

  setWeatherCity(city);
  Serial.printf("[meteo] citta' impostata dal portale: %s\n", weather.city);
}

// ---------------------------------------------------------------------------
// CHIAMATE
// ---------------------------------------------------------------------------
// Una GET in chiaro, con il corpo dato in pasto a deserializeJson attraverso
// il filtro. Lo stream si legge direttamente dal client: non si costruisce
// mai la stringa intera della risposta, che e' l'abitudine che frammenta
// l'heap in questi progetti.
static bool weatherHttpGet(const char *host, const char *path,
                           JsonDocument &doc, JsonDocument &filter) {
  if (WiFi.status() != WL_CONNECTED) {
    copyText(weather.message, sizeof(weather.message), "rete assente");
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(WEATHER_HTTP_TIMEOUT_MS);

  // Si chiede in HTTP/1.0, e non e' un capriccio.
  //
  // In HTTP/1.1 questo server risponde in "chunked transfer encoding": il
  // corpo arriva a pezzi, ognuno preceduto dalla propria lunghezza scritta in
  // esadecimale. getStreamPtr() restituisce lo stream GREZZO, quei marcatori
  // compresi, e nessuno li toglie: nel buffer finiva "1a5" seguito dal JSON.
  //
  // ArduinoJson leggeva "1a5", ne ricavava il numero 1 - che e' un documento
  // JSON valido - e restituiva Ok. Nessun errore, nessun campo: da qui "dato
  // assente" invece di "risposta illeggibile".
  //
  // E spiega perche' la geocodifica passava: quella risponde con
  // Content-Length, quindi senza chunk. Si vedeva gia' nel registro, nella
  // differenza fra "dichiarati 414" e "dichiarati -1".
  http.useHTTP10(true);

  if (!http.begin(client, host, 80, path)) {
    copyText(weather.message, sizeof(weather.message), "URL non valido");
    return false;
  }

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    snprintf(weather.message, sizeof(weather.message), "HTTP %d", status);
    Serial.printf("[meteo] %s%s -> HTTP %d\n", host, path, status);
    http.end();
    return false;
  }

  // Il corpo si legge in un buffer e POI si parsa, invece di dare lo stream
  // in pasto a deserializeJson.
  //
  // Dallo stream, ArduinoJson interpreta un attimo di "nessun byte
  // disponibile" come fine del documento: se la risposta arriva spezzata in
  // due segmenti TCP - e queste lo fanno, perche' il server manda le
  // intestazioni e il corpo separati - il parser si ferma a meta' senza che
  // nessuno glielo dica. Qui si aspetta davvero tutto il corpo, con una
  // scadenza che si rimanda a ogni byte ricevuto.
  //
  // E costa poco: queste risposte stanno in mezzo kilobyte.
  int declared = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long deadline = millis() + WEATHER_HTTP_TIMEOUT_MS;

  while (got + 1 < sizeof(weatherBody)) {
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

    size_t room = sizeof(weatherBody) - 1 - got;
    size_t take = available < (int)room ? (size_t)available : room;
    int read = stream->readBytes(weatherBody + got, take);
    if (read > 0) {
      got += (size_t)read;
      deadline = millis() + WEATHER_HTTP_TIMEOUT_MS;
    }

    if (declared > 0 && got >= (size_t)declared) {
      break;
    }
  }

  weatherBody[got] = '\0';
  http.end();

  Serial.printf("[meteo] %s -> HTTP %d, %u byte (dichiarati %d)\n",
                host, status, (unsigned)got, declared);

  DeserializationError error = deserializeJson(doc, weatherBody,
                                               DeserializationOption::Filter(filter));
  if (error) {
    copyText(weather.message, sizeof(weather.message), "risposta illeggibile");
    Serial.printf("[meteo] JSON: %s\n", error.c_str());
    Serial.printf("[meteo] corpo: %s\n", weatherBody);
    return false;
  }

  // deserializeJson si accontenta del primo valore completo e ignora quello
  // che segue: davanti a "1a5{...}" restituisce Ok con dentro il numero 1.
  // Qui ci aspettiamo un oggetto, e se non lo e' e' successo qualcosa al
  // corpo prima del parser - il registro lo fa vedere invece di lasciare il
  // campo misteriosamente vuoto.
  if (!doc.is<JsonObject>()) {
    copyText(weather.message, sizeof(weather.message), "risposta inattesa");
    Serial.printf("[meteo] la risposta non e' un oggetto JSON\n");
    Serial.printf("[meteo] corpo: %s\n", weatherBody);
    return false;
  }

  return true;
}

// Scrive il nome della citta' nella query: spazi e accenti vanno codificati,
// altrimenti la richiesta e' malformata e il server risponde 400.
static void appendUrlEncoded(char *destination, size_t size, const char *source) {
  size_t used = strlen(destination);
  static const char *hex = "0123456789ABCDEF";

  for (; *source != '\0' && used + 4 < size; ++source) {
    unsigned char c = static_cast<unsigned char>(*source);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      destination[used++] = static_cast<char>(c);
    } else {
      destination[used++] = '%';
      destination[used++] = hex[c >> 4];
      destination[used++] = hex[c & 0x0F];
    }
  }

  destination[used] = '\0';
}

// Traduce il nome in coordinate. Si fa una volta sola per citta': il risultato
// sta in NVS, e questa funzione non viene piu' chiamata finche' qualcuno non
// cambia il campo nel portale.
bool resolveWeatherCity() {
  if (weather.city[0] == '\0') {
    return false;
  }

  char path[WEATHER_URL_MAX];
  copyText(path, sizeof(path), "/v1/search?count=1&language=it&format=json&name=");
  appendUrlEncoded(path, sizeof(path), weather.city);

  JsonDocument filter;
  filter["results"][0]["latitude"] = true;
  filter["results"][0]["longitude"] = true;
  filter["results"][0]["name"] = true;
  filter["results"][0]["country"] = true;

  JsonDocument doc;
  if (!weatherHttpGet(WEATHER_GEO_HOST, path, doc, filter)) {
    return false;
  }

  JsonVariant first = doc["results"][0];
  if (first.isNull() || !first["latitude"].is<float>()) {
    copyText(weather.message, sizeof(weather.message), "citta' non trovata");
    Serial.printf("[meteo] nessun risultato per \"%s\"\n", weather.city);
    return false;
  }

  weather.latitude = first["latitude"].as<float>();
  weather.longitude = first["longitude"].as<float>();
  weather.located = true;

  // A schermo va il nome che ha risposto il servizio, non quello digitato:
  // se uno scrive "ariano" e viene interrogata Ariano Irpino, e' giusto che
  // si legga quale posto si sta guardando davvero.
  const char *name = first["name"] | weather.city;
  copyText(weather.place, sizeof(weather.place), name);

  storeWeatherLocation();
  Serial.printf("[meteo] %s -> %.4f, %.4f\n", weather.place,
                weather.latitude, weather.longitude);
  return true;
}

static bool fetchWeather() {
  char path[WEATHER_URL_MAX];
  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code",
           weather.latitude, weather.longitude);

  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["relative_humidity_2m"] = true;
  filter["current"]["weather_code"] = true;

  JsonDocument doc;
  if (!weatherHttpGet(WEATHER_HOST, path, doc, filter)) {
    return false;
  }

  JsonVariant current = doc["current"];
  if (current.isNull() || !current["temperature_2m"].is<float>()) {
    copyText(weather.message, sizeof(weather.message), "dato assente");
    return false;
  }

  weather.temperature = current["temperature_2m"].as<float>();
  weather.humidity = current["relative_humidity_2m"] | 0;
  weather.code = current["weather_code"] | -1;
  weather.valid = true;
  weather.lastOkMs = millis();
  weather.message[0] = '\0';

  // Ora dell'ultimo dato buono, per quando diventera' vecchio. Se l'orologio
  // non e' ancora sincronizzato si resta senza: meglio nessuna ora che una
  // sbagliata accanto a un valore che si sta gia' invitando a non fidarsi.
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 0)) {
    strftime(weather.lastOkClock, sizeof(weather.lastOkClock), "%H:%M", &timeInfo);
  }

  Serial.printf("[meteo] %s: %.1f C, %u%%, codice %d\n",
                weather.place, weather.temperature,
                static_cast<unsigned>(weather.humidity), weather.code);
  return true;
}

// ---------------------------------------------------------------------------
// PASSATA PERIODICA
// ---------------------------------------------------------------------------
// Non guarda quale pagina sia a schermo, a differenza di Spotify: il meteo si
// aggiorna ogni quarto d'ora e la pagina del sensore e' quella su cui si butta
// l'occhio di sfuggita. Trovarla ferma all'ora precedente perche' nessuno la
// stava guardando sarebbe il comportamento sbagliato.
void refreshWeatherData() {
  if (weather.city[0] == '\0' || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  unsigned long wait = weather.valid ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS;

  if (weather.lastTryMs != 0 && now - weather.lastTryMs < wait) {
    return;
  }
  weather.lastTryMs = now;

  if (!weather.located && !resolveWeatherCity()) {
    if (currentPage == PAGE_DHT) {
      markDisplayDirty();
    }
    return;
  }

  bool wasValid = weather.valid;
  float previous = weather.temperature;
  int16_t previousCode = weather.code;

  fetchWeather();

  // Si ridisegna solo se la pagina del sensore e' a schermo e qualcosa e'
  // cambiato davvero. Il passaggio da fresco a vecchio lo cattura comunque la
  // pagina, che confronta l'eta' a ogni passata.
  if (currentPage == PAGE_DHT &&
      (!wasValid || weather.temperature != previous || weather.code != previousCode)) {
    markDisplayDirty();
  }
}

// Un dato vecchio resta a schermo, ma smorzato e con l'ora accanto. Nasconderlo
// sarebbe peggio - si perderebbe l'unica informazione disponibile - e mostrarlo
// come fosse attuale anche: un numero plausibile e falso e' l'errore di cui non
// ci si accorge.
bool weatherIsStale() {
  return weather.valid && (millis() - weather.lastOkMs) > WEATHER_STALE_MS;
}

// ---------------------------------------------------------------------------
// CODICI WMO
// ---------------------------------------------------------------------------
// Open-Meteo restituisce il codice meteo dello standard WMO 4677, che ha una
// novantina di valori. A schermo servono otto famiglie: piu' di cosi' non si
// distinguerebbero in un'icona da cinquanta pixel, e la parola sotto dice il
// resto.
const char *weatherConditionName(int16_t code) {
  switch (code) {
    case 0:  return "sereno";
    case 1:  return "quasi sereno";
    case 2:  return "poco nuvoloso";
    case 3:  return "coperto";
    case 45:
    case 48: return "nebbia";
    case 51:
    case 53:
    case 55: return "pioviggine";
    case 56:
    case 57: return "pioviggine gelata";
    case 61:
    case 63: return "pioggia";
    case 65: return "pioggia forte";
    case 66:
    case 67: return "pioggia gelata";
    case 71:
    case 73: return "neve";
    case 75: return "neve forte";
    case 77: return "granelli di neve";
    case 80:
    case 81: return "rovesci";
    case 82: return "rovesci forti";
    case 85:
    case 86: return "rovesci di neve";
    case 95: return "temporale";
    case 96:
    case 99: return "temporale e grandine";
    default: return "";
  }
}
