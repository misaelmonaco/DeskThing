// ===========================================================================
// SPOTIFY LINK - collegamento dell'account Spotify senza toccare il codice
// ---------------------------------------------------------------------------
// Il dispositivo deve poter essere configurato da chi non ha mai visto lo
// sketch. Servono due cose, e nessuna delle due passa piu' da Secrets.h:
//
//   1. L'APP Spotify (Client ID e Client Secret). Vengono dal portale WiFi,
//      dove compaiono come campi in fondo alla pagina della rete; se nessuno
//      li ha scritti li', valgono quelli di fabbrica di Secrets.h.
//
//   2. L'ACCOUNT (refresh token). Si ottiene inquadrando con il telefono il
//      QR code mostrato sulla pagina Spotify e facendo il login. Il token
//      finisce nella NVS e sopravvive ai riavvii.
//
// Come funziona il login con il QR code:
//   - il dispositivo genera uno "state" casuale e compone l'URL di
//     autorizzazione di Spotify, che e' il contenuto del QR code;
//   - dopo il login Spotify rimanda il telefono al Redirect URI, un piccolo
//     server pubblico (SPOTIFY_AUTH_HOST) che conserva il codice monouso
//     sotto quello state;
//   - il dispositivo interroga il server ogni pochi secondi, ritira il codice
//     e lo scambia con Spotify per il refresh token.
//
// Lo scambio lo fa la libreria SpotifyEsp32 (get_refresh_token), che parla
// direttamente con accounts.spotify.com: il Client Secret non lascia mai il
// dispositivo se non verso Spotify. Nota: dalla versione 5 la libreria fa le
// richieste dei token senza verificare il certificato del server (lo dichiara
// lei stessa nei log). E' una scelta della libreria, non di questo sketch.
// ===========================================================================

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
// Chiavi della NVS (massimo 15 caratteri ciascuna). "token_owner" ricorda con
// quale Client ID e' stato ottenuto il refresh token: Spotify lega il token
// all'app, e se l'app cambia dal portale il token vecchio non vale piu'.
constexpr const char *NVS_KEY_CLIENT_ID   = "client_id";
constexpr const char *NVS_KEY_SECRET      = "secret";
constexpr const char *NVS_KEY_REFRESH     = "refresh";
constexpr const char *NVS_KEY_TOKEN_OWNER = "token_owner";

// Legge una stringa dalla NVS dentro un buffer. Se la chiave non c'e' il
// buffer resta com'era: e' cosi' che i valori di fabbrica sopravvivono.
void readNvsText(Preferences &prefs, const char *key, char *out, size_t size) {
  if (!prefs.isKey(key)) {
    return;
  }

  if (prefs.getString(key, out, size) == 0) {
    out[0] = '\0';
  }
}

void loadSpotifyCredentials() {
  copyText(spotifyCredentials.clientId, sizeof(spotifyCredentials.clientId), SPOTIFY_CLIENT_ID);
  copyText(spotifyCredentials.clientSecret, sizeof(spotifyCredentials.clientSecret), SPOTIFY_CLIENT_SECRET);
  spotifyCredentials.refreshToken[0] = '\0';

  // In sola lettura. Al primissimo avvio lo spazio non esiste ancora e begin()
  // fallisce: restano i valori di fabbrica e nessun account, che e' giusto.
  Preferences prefs;
  if (!prefs.begin(SPOTIFY_NVS_NAMESPACE, true)) {
    return;
  }

  readNvsText(prefs, NVS_KEY_CLIENT_ID, spotifyCredentials.clientId, sizeof(spotifyCredentials.clientId));
  readNvsText(prefs, NVS_KEY_SECRET, spotifyCredentials.clientSecret, sizeof(spotifyCredentials.clientSecret));

  char tokenOwner[SPOTIFY_KEY_MAX] = "";
  readNvsText(prefs, NVS_KEY_TOKEN_OWNER, tokenOwner, sizeof(tokenOwner));
  if (strcmp(tokenOwner, spotifyCredentials.clientId) == 0) {
    readNvsText(prefs, NVS_KEY_REFRESH, spotifyCredentials.refreshToken,
                sizeof(spotifyCredentials.refreshToken));
  }

  prefs.end();
}

void saveSpotifyRefreshToken(const char *token) {
  copyText(spotifyCredentials.refreshToken, sizeof(spotifyCredentials.refreshToken), token);

  Preferences prefs;
  if (!prefs.begin(SPOTIFY_NVS_NAMESPACE, false)) {
    Serial.println("NVS non disponibile: l'account restera' collegato fino al riavvio");
    return;
  }

  prefs.putString(NVS_KEY_REFRESH, token);
  prefs.putString(NVS_KEY_TOKEN_OWNER, spotifyCredentials.clientId);
  prefs.end();
}

void clearSpotifyRefreshToken() {
  spotifyCredentials.refreshToken[0] = '\0';

  Preferences prefs;
  if (!prefs.begin(SPOTIFY_NVS_NAMESPACE, false)) {
    return;
  }

  prefs.remove(NVS_KEY_REFRESH);
  prefs.remove(NVS_KEY_TOKEN_OWNER);
  prefs.end();
}

// ---------------------------------------------------------------------------
// CAMPI NEL PORTALE WIFI
// ---------------------------------------------------------------------------
// Tre parametri di WiFiManager: un'intestazione con le istruzioni e i due
// campi. Stanno a livello di file perche' WiFiManager ne conserva solo il
// puntatore, e devono restare vivi per tutto il tempo in cui il portale e'
// aperto.
//
// Il Client ID viene precompilato con quello in uso, cosi' si vede quale app
// e' configurata. Il Secret no: un campo vuoto vuol dire "non cambiare", e
// mostrare il segreto in chiaro su una pagina web non ha senso.
char spotifyPortalHeadingHtml[400];

WiFiManagerParameter spotifyPortalHeading(spotifyPortalHeadingHtml);
WiFiManagerParameter spotifyPortalClientId("spotify_id", "Spotify Client ID", "", SPOTIFY_KEY_MAX - 1);
WiFiManagerParameter spotifyPortalSecret("spotify_secret", "Spotify Client Secret", "", SPOTIFY_KEY_MAX - 1);

void addSpotifyPortalParams(WiFiManager &wifiManager) {
  loadSpotifyCredentials();

  // Chi crea la propria app deve registrarci il Redirect URI esatto e
  // aggiungere il proprio account fra gli utenti: e' la parte che si sbaglia
  // sempre, quindi la pagina lo dice dove serve, non in un README.
  snprintf(spotifyPortalHeadingHtml, sizeof(spotifyPortalHeadingHtml),
           "<hr><h3>Spotify</h3>"
           "<p>Facoltativo. Dati della tua app su developer.spotify.com.<br>"
           "Nell'app registra come Redirect URI:<br><small>%s</small><br>"
           "e aggiungi il tuo account in User Management.<br>"
           "Un campo vuoto lascia il valore attuale.</p>",
           SPOTIFY_REDIRECT_URI);

  spotifyPortalClientId.setValue(spotifyCredentials.clientId, SPOTIFY_KEY_MAX - 1);
  spotifyPortalSecret.setValue("", SPOTIFY_KEY_MAX - 1);

  wifiManager.addParameter(&spotifyPortalHeading);
  wifiManager.addParameter(&spotifyPortalClientId);
  wifiManager.addParameter(&spotifyPortalSecret);
  wifiManager.setSaveParamsCallback(storeSpotifyPortalParams);
}

// Copia togliendo gli spazi in testa e in coda: incollando da telefono ne
// arriva spesso uno in fondo, e un Client ID con uno spazio e' un Client ID
// sbagliato che nessuno riuscirebbe a vedere.
void copyTrimmed(char *destination, size_t size, const char *source) {
  if (source == nullptr) {
    source = "";
  }
  while (*source == ' ' || *source == '\t') {
    ++source;
  }

  copyText(destination, size, source);

  size_t length = strlen(destination);
  while (length > 0 && (destination[length - 1] == ' ' || destination[length - 1] == '\t')) {
    destination[--length] = '\0';
  }
}

// Callback di WiFiManager al salvataggio dei campi. Scrive nella NVS solo
// cio' che e' davvero cambiato, e segnala il cambiamento: ricostruire il
// client tocca a chi ha aperto il portale, una volta che si e' chiuso.
void storeSpotifyPortalParams() {
  char clientId[SPOTIFY_KEY_MAX];
  char clientSecret[SPOTIFY_KEY_MAX];
  copyTrimmed(clientId, sizeof(clientId), spotifyPortalClientId.getValue());
  copyTrimmed(clientSecret, sizeof(clientSecret), spotifyPortalSecret.getValue());

  bool idChanged = clientId[0] != '\0' &&
                   strcmp(clientId, spotifyCredentials.clientId) != 0;
  bool secretChanged = clientSecret[0] != '\0' &&
                       strcmp(clientSecret, spotifyCredentials.clientSecret) != 0;

  if (!idChanged && !secretChanged) {
    return;
  }

  Preferences prefs;
  if (!prefs.begin(SPOTIFY_NVS_NAMESPACE, false)) {
    Serial.println("NVS non disponibile: app Spotify non salvata");
    return;
  }

  if (idChanged) {
    prefs.putString(NVS_KEY_CLIENT_ID, clientId);
    copyText(spotifyCredentials.clientId, sizeof(spotifyCredentials.clientId), clientId);
  }
  if (secretChanged) {
    prefs.putString(NVS_KEY_SECRET, clientSecret);
    copyText(spotifyCredentials.clientSecret, sizeof(spotifyCredentials.clientSecret), clientSecret);
  }
  prefs.end();

  spotifyCredentialsChanged = true;
  Serial.println("App Spotify aggiornata dal portale");
}

// Portale a richiesta: dal tasto CONFIGURA della pagina Spotify quando l'app
// e' da impostare, e dal tasto CITTA' della pagina del sensore sempre.
//
// I campi stanno su una pagina a parte ("Setup"): cambiare l'app o la citta'
// non deve costringere a riscegliere la rete e riscrivere la password del
// WiFi. Appena i campi vengono salvati il portale si chiude da solo.
//
// C'e' anche la citta' del meteo, e non per simmetria: senza, l'unico modo
// per cambiarla sarebbe dimenticare la rete e rifare tutto l'aggancio.
void openSetupPortal() {
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wifiManager.setParamsPage(true);
  addSpotifyPortalParams(wifiManager);
  addWeatherPortalParams(wifiManager);

  // stopConfigPortal() dentro la callback e' sicuro: con il portale
  // bloccante alza solo il flag che fa uscire il suo ciclo, dopo che la
  // pagina di conferma e' gia' stata inviata al telefono.
  wifiManager.setSaveParamsCallback([&wifiManager]() {
    storePortalParams();
    wifiManager.stopConfigPortal();
  });

  char portalLabel[48];
  snprintf(portalLabel, sizeof(portalLabel), "collegati a %s", WIFI_PORTAL_SSID);
  drawBootScreen(portalLabel, "poi apri 192.168.4.1 e scegli Setup", true, 0.5f,
                 "CONFIGURAZIONE");
  Serial.print("Portale Spotify aperto: collegati a ");
  Serial.print(WIFI_PORTAL_SSID);
  Serial.println(", apri http://192.168.4.1 e scegli Setup");

  wifiManager.startConfigPortal(WIFI_PORTAL_SSID);

  // Il portale lascia il WiFi in modalita' access point + stazione: si torna
  // alla sola stazione e, se nel frattempo il collegamento e' caduto, lo si
  // riprende con la rete salvata.
  WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin();
    WiFi.waitForConnectResult(10000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    ensureSpotifyClient();
  }

  // I tasti premuti mentre il portale era aperto non erano rivolti a questa
  // pagina: vanno buttati, non serviti.
  clearButtonQueue();
  markDisplayDirty();
}

// ---------------------------------------------------------------------------
// LOGIN CON IL QR CODE
// ---------------------------------------------------------------------------
// Nuovo state e nuovo URL, cioe' un QR code nuovo. Si rigenera a ogni
// costruzione del client e dopo un login fallito: il codice monouso di quel
// tentativo e' ormai consumato, e con lo stesso state si ritirerebbe ancora.
//
// Lo state ha la forma che usa la libreria ("ESP32-" + 19 caratteri): e' la
// forma che il server del login si aspetta.
void newSpotifyLoginState() {
  static const char charset[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const char prefix[] = "ESP32-";

  copyText(spotifyLogin.state, sizeof(spotifyLogin.state), prefix);
  size_t used = strlen(prefix);
  while (used < SPOTIFY_STATE_LEN) {
    spotifyLogin.state[used++] = charset[esp_random() % (sizeof(charset) - 1)];
  }
  spotifyLogin.state[used] = '\0';

  snprintf(spotifyLogin.url, sizeof(spotifyLogin.url),
           "https://accounts.spotify.com/authorize"
           "?client_id=%s&response_type=code&redirect_uri=%s&scope=%s&state=%s",
           spotifyCredentials.clientId, SPOTIFY_REDIRECT_URI, SPOTIFY_SCOPES,
           spotifyLogin.state);

  // Nessuno completa un login in meno di un intervallo: la prima domanda al
  // server puo' aspettare, e intanto la pagina si disegna senza intoppi.
  spotifyLogin.lastPollMs = millis();

  // L'URL sulla seriale serve a chi deve capire perche' il QR non funziona.
  // Non contiene nulla di segreto: il Client ID e' pubblico per definizione.
  Serial.print("Login Spotify, URL del QR code: ");
  Serial.println(spotifyLogin.url);
}

// Chiede al server del login se per il nostro state c'e' un codice.
// Il server risponde {"code":null} finche' nessuno ha completato il login.
//
// La connessione non verifica il certificato: da qui passa solo il codice
// monouso, che senza il Client Secret non vale niente. Il Secret non passa
// mai da questo server.
SpotifyLoginPoll fetchSpotifyLoginCode(char *code, size_t size) {
  char url[128];
  snprintf(url, sizeof(url), "https://%s/api/spotify/get_code?state=%s",
           SPOTIFY_AUTH_HOST, spotifyLogin.state);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  // HTTP/1.0 obbliga il server a mandare il corpo in un pezzo solo, senza
  // chunked encoding: cosi' ArduinoJson puo' leggerlo direttamente dal
  // flusso, senza passare da una String.
  http.useHTTP10(true);
  http.setTimeout(SPOTIFY_LOGIN_HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    return SPOTIFY_LOGIN_UNREACHABLE;
  }

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return SPOTIFY_LOGIN_UNREACHABLE;
  }

  JsonDocument filter;
  filter["code"] = true;

  JsonDocument doc;
  DeserializationError error =
    deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (error) {
    return SPOTIFY_LOGIN_UNREACHABLE;
  }

  const char *value = doc["code"] | "";
  if (value[0] == '\0') {
    return SPOTIFY_LOGIN_PENDING;
  }

  copyText(code, size, value);
  return SPOTIFY_LOGIN_CODE;
}

// Scambia il codice con il refresh token e lo mette al sicuro nella NVS.
void completeSpotifyLogin(const char *code) {
  bool linked = spotifyClient->get_refresh_token(code, SPOTIFY_REDIRECT_URI) &&
                spotifyClient->is_auth();

  if (!linked) {
    // Di solito e' il Client Secret sbagliato, o un Redirect URI registrato
    // nella dashboard diverso da SPOTIFY_REDIRECT_URI. In ogni caso il
    // codice e' bruciato: si riparte con un QR nuovo.
    Serial.println("Spotify ha rifiutato il codice: controlla Client Secret e Redirect URI");
    newSpotifyLoginState();
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
             "Login rifiutato: controlla il Secret");
    markDisplayDirty();
    return;
  }

  // La libreria non espone il token se non in copia, allocata con strdup:
  // le tre stringhe vanno liberate.
  user_tokens tokens = spotifyClient->get_user_tokens();
  saveSpotifyRefreshToken(tokens.refresh_token);
  free(const_cast<char *>(tokens.client_id));
  free(const_cast<char *>(tokens.client_secret));
  free(const_cast<char *>(tokens.refresh_token));

  spotifyClient->get_access_token();

  spotifyState.awaitingLogin = false;
  spotifyState.authenticated = true;
  spotifyState.lastStatusCode = 0;
  spotifyState.lastPollMs = 0;
  copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "Account collegato");

  Serial.println("Account Spotify collegato e salvato nella NVS");
  markDisplayDirty();
}

// Chiamata dal loop mentre la pagina del QR e' a schermo (vedi
// refreshSpotifyData). Ogni chiamata al server e' HTTPS bloccante, quindi si
// fa di rado e mai con altre pagine aperte.
void serviceSpotifyLogin() {
  if (spotifyClient == nullptr || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - spotifyLogin.lastPollMs < SPOTIFY_LOGIN_POLL_MS) {
    return;
  }

  // Un codice di Spotify arriva a qualche centinaio di caratteri: statico per
  // non caricarlo sullo stack del loop.
  static char code[768];

  char previousStatus[TEXT_STATUS_MAX];
  copyText(previousStatus, sizeof(previousStatus), spotifyState.statusMessage);

  SpotifyLoginPoll result = fetchSpotifyLoginCode(code, sizeof(code));

  // L'intervallo parte dalla fine della chiamata, non dall'inizio: con il
  // server lento le domande non si accavallano e il loop respira.
  spotifyLogin.lastPollMs = millis();

  switch (result) {
    case SPOTIFY_LOGIN_CODE:
      completeSpotifyLogin(code);
      return;
    case SPOTIFY_LOGIN_PENDING:
      copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
               "In attesa del login");
      break;
    case SPOTIFY_LOGIN_UNREACHABLE:
      copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
               "Server del login non raggiungibile");
      break;
  }

  // Ridisegnare vuol dire ridipingere anche il QR: solo se la riga di stato
  // e' davvero cambiata.
  if (strcmp(previousStatus, spotifyState.statusMessage) != 0) {
    markDisplayDirty();
  }
}

// ---------------------------------------------------------------------------
// RICOSTRUZIONE E SCOLLEGAMENTO
// ---------------------------------------------------------------------------
// Butta il client e lo ricostruisce dalle credenziali salvate. Serve quando
// cambia l'app dal portale o quando l'account viene scollegato.
void restartSpotify() {
  if (spotifyClient != nullptr) {
    delete spotifyClient;
    spotifyClient = nullptr;
  }

  spotifyState.configured = false;
  spotifyState.awaitingLogin = false;
  spotifyState.authenticated = false;
  spotifyState.playbackActive = false;
  spotifyState.isPlaying = false;
  spotifyState.lastStatusCode = 0;
  spotifyState.trackName[0] = '\0';
  spotifyState.artistName[0] = '\0';
  spotifyState.progressMs = 0;
  spotifyState.durationMs = 0;
  spotifyState.lastPollMs = 0;
  spotifyState.appKeysMissing = false;
  spotifyState.unlinkArmed = false;

  initSpotify();
  markDisplayDirty();
}

void armSpotifyUnlink() {
  spotifyState.unlinkArmed = true;
  spotifyState.unlinkArmedMs = millis();
  markDisplayDirty();
}

void disarmSpotifyUnlink() {
  spotifyState.unlinkArmed = false;
}

// Dimentica l'account e ripropone il QR code. Il token viene cancellato dal
// dispositivo; per revocarlo anche lato Spotify il proprietario dell'account
// lo toglie da spotify.com/account/apps.
void unlinkSpotifyAccount() {
  clearSpotifyRefreshToken();
  Serial.println("Account Spotify scollegato");
  restartSpotify();
}

// Scade la prima meta' della conferma, come per il reset del WiFi.
void refreshSpotifyUnlinkArming() {
  if (!spotifyState.unlinkArmed) {
    return;
  }
  if (millis() - spotifyState.unlinkArmedMs <= SPOTIFY_UNLINK_CONFIRM_MS) {
    return;
  }

  spotifyState.unlinkArmed = false;
  if (currentPage == PAGE_SPOTIFY) {
    markDisplayDirty();
  }
}
