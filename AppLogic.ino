// ===========================================================================
// APP LOGIC - input, servizi di rete, sensori e logica delle funzioni
// ---------------------------------------------------------------------------
// Nessuna funzione di questo file conosce le coordinate del display: quando
// serve aggiornare la grafica si limita ad alzare un flag di invalidazione.
// ===========================================================================

// ---------------------------------------------------------------------------
// TESTI A LUNGHEZZA FISSA
// ---------------------------------------------------------------------------
// Copia e concatenazione dentro buffer di dimensione nota, con troncamento
// silenzioso. Sono scritte a mano invece di usare strlcpy per non dipendere da
// quale variante di libc monta il core installato, e perche' cosi' il
// troncamento e' una scelta esplicita e non un effetto collaterale.
void copyText(char *destination, size_t size, const char *source) {
  if (destination == nullptr || size == 0) {
    return;
  }
  if (source == nullptr) {
    destination[0] = '\0';
    return;
  }

  size_t i = 0;
  while (i + 1 < size && source[i] != '\0') {
    destination[i] = source[i];
    ++i;
  }
  destination[i] = '\0';
}

void appendText(char *destination, size_t size, const char *source) {
  if (destination == nullptr || size == 0 || source == nullptr) {
    return;
  }

  size_t used = strlen(destination);
  if (used + 1 >= size) {
    return;
  }

  copyText(destination + used, size - used, source);
}

// ---------------------------------------------------------------------------
// INVALIDAZIONE DEL RENDERING
// ---------------------------------------------------------------------------
// Richiede il ridisegno completo della pagina. I valori che scorrono al
// secondo non passano di qui: se ne accorgono da soli confrontando il testo
// da mostrare con la cache di cio' che e' gia' sul pannello.
void markDisplayDirty() {
  displayDirty = true;
}

// ---------------------------------------------------------------------------
// PULSANTI
// ---------------------------------------------------------------------------
// I pulsanti sono gestiti a interrupt, non a campionamento.
//
// Il motivo e' che il loop principale non gira a ritmo costante: una chiamata
// a Spotify o al portale WiFi puo' tenerlo fermo per centinaia di millisecondi.
// Leggendo i pin a polling, una pressione che comincia e finisce dentro quella
// finestra non viene vista affatto: e' persa, non ritardata. Ed e' proprio
// quando si preme due volte di fila - la seconda perche' la prima "non ha
// risposto" - che il difetto si manifesta di piu'.
//
// Con l'interrupt la pressione viene catturata nell'istante in cui avviene e
// messa in coda; il loop la serve appena torna disponibile. La coda conserva
// anche le pressioni multiple.
//
// I pin sono in INPUT_PULLUP: a riposo leggono HIGH, premuti vanno a LOW.
// Un solo capo dei pulsanti torna al GND comune della pulsantiera.

// Protegge i contatori condivisi fra ISR e loop. L'ESP32 e' dual core: senza
// sezione critica il decremento della coda potrebbe sovrapporsi a un
// incremento fatto dalla ISR sull'altro core e perdere una pressione.
portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;

// Mette in coda una pressione conclusa, scegliendo la coda in base a quanto
// e' stata tenuta. Va chiamata con buttonMux gia' preso.
void IRAM_ATTR enqueueRelease(Button &button, uint32_t releaseUs) {
  uint32_t heldMs = (releaseUs - button.pressStartUs) / 1000UL;

  if (heldMs >= BUTTON_LONG_PRESS_MS) {
    if (button.longPressCount < BUTTON_QUEUE_MAX) {
      button.longPressCount++;
    }
    return;
  }

  if (button.pressCount < BUTTON_QUEUE_MAX) {
    button.pressCount++;
  }
}

// Antirimbalzo a macchina a stati, eseguito direttamente nella ISR.
//
// La versione a sola finestra temporale aveva due difetti, e nessuno dei due
// si vede finche' i contatti sono nuovi.
//
// Il primo: prendeva il primo fronte di una raffica e leggeva il livello del
// pin in quell'istante. Durante il rimbalzo di rilascio la linea sbatte fra
// alto e basso, e se quel campione capitava basso veniva contata una
// pressione che non era mai avvenuta. Il rimedio e' `pressed`, il livello
// logico gia' accettato: un fronte che racconta uno stato in cui siamo gia'
// non e' una transizione, e' rumore. Da rilasciato si puo' solo passare a
// premuto e viceversa.
//
// Il secondo: la finestra veniva riancorata a ogni fronte, anche a quelli
// scartati. Con un contatto sporco che rimbalza piu' a lungo della finestra,
// il riferimento continuava a slittare in avanti e la pressione buona non
// arrivava mai a superarlo. Qui `lastEdgeUs` segna l'ultima transizione
// *accettata*, quindi la finestra e' un tempo morto di durata fissa dopo ogni
// cambio di stato vero.
//
// La pressione viene messa in coda al rilascio, non alla pressione: e' cio'
// che permette di misurarne la durata e di distinguere il tocco dal tenere
// premuto. Si perde l'immediatezza di pochi centesimi di secondo, si guadagna
// un gesto in piu' su ogni tasto.
void IRAM_ATTR handleButtonInterrupt(void *arg) {
  Button *button = static_cast<Button *>(arg);
  uint32_t now = micros();

  button->edgeCount++;

  bool level = (digitalRead(button->pin) == LOW);

  // Il fronte non porta notizie: la linea risulta gia' in questo stato.
  if (level == button->pressed) {
    button->bounceCount++;
    return;
  }

  // Troppo vicino all'ultima transizione accettata: e' rimbalzo.
  if (now - button->lastEdgeUs < BUTTON_DEBOUNCE_US) {
    button->bounceCount++;
    return;
  }

  button->lastEdgeUs = now;
  button->pressed = level;

  if (level) {
    button->pressStartUs = now;
    return;
  }

  portENTER_CRITICAL_ISR(&buttonMux);
  enqueueRelease(*button, now);
  portEXIT_CRITICAL_ISR(&buttonMux);
}

void setupButtons() {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    pinMode(buttons[i].pin, INPUT_PULLUP);

    // Lo stato iniziale si legge dal pin invece di darlo per rilasciato: se
    // un tasto e' gia' premuto all'accensione - il reset WiFi si fa cosi' -
    // la macchina a stati parte allineata alla realta' e il rilascio viene
    // riconosciuto come tale.
    buttons[i].pressed = (digitalRead(buttons[i].pin) == LOW);
    buttons[i].lastEdgeUs = micros();
    buttons[i].pressStartUs = micros();
    buttons[i].edgeCount = 0;
    buttons[i].bounceCount = 0;
    buttons[i].resyncCount = 0;
    buttons[i].pressCount = 0;
    buttons[i].longPressCount = 0;

    attachInterruptArg(digitalPinToInterrupt(buttons[i].pin),
                       handleButtonInterrupt, &buttons[i], CHANGE);
  }
}

// Estrae una pressione dalla coda. Restituisce true una volta per ogni
// pressione catturata, anche se ne sono arrivate piu' di una nel frattempo.
// Il controllo a vuoto sta fuori dalla sezione critica: solo la ISR
// incrementa, quindi una coda vista vuota non puo' riempirsi e sparire di
// nuovo mentre la si guarda.
bool takeShortPress(Button &button) {
  if (button.pressCount == 0) {
    return false;
  }

  portENTER_CRITICAL(&buttonMux);
  button.pressCount--;
  portEXIT_CRITICAL(&buttonMux);

  return true;
}

bool takeLongPress(Button &button) {
  if (button.longPressCount == 0) {
    return false;
  }

  portENTER_CRITICAL(&buttonMux);
  button.longPressCount--;
  portEXIT_CRITICAL(&buttonMux);

  return true;
}

// Rete di sicurezza della macchina a stati.
//
// Un fronte puo' andare perso: la lettura del DHT22 disabilita gli interrupt
// per tutta la sua durata, e un rilascio che cade li' dentro non arriva mai
// alla ISR. Senza questo controllo il tasto resterebbe fermo su "premuto", e
// da quel momento ogni pressione successiva verrebbe scambiata per rumore -
// il tasto sarebbe morto fino al riavvio.
//
// Il rimedio e' guardare ogni tanto il pin vero. Se non concorda con lo stato
// logico da piu' di BUTTON_RESYNC_US, il fronte e' andato perso: si riallinea
// e, se era un rilascio, la pressione viene recuperata invece che buttata.
void resyncButtonStates() {
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    Button &button = buttons[i];

    bool level = (digitalRead(button.pin) == LOW);
    if (level == button.pressed) {
      continue;
    }

    uint32_t now = micros();
    if (now - button.lastEdgeUs < BUTTON_RESYNC_US) {
      continue;
    }

    portENTER_CRITICAL(&buttonMux);
    // Ricontrollo dentro la sezione critica: fra il primo confronto e qui la
    // ISR puo' essere arrivata da sola alla stessa conclusione.
    if (level != button.pressed) {
      button.lastEdgeUs = now;
      button.pressed = level;
      button.resyncCount++;

      if (level) {
        button.pressStartUs = now;
      } else {
        enqueueRelease(button, now);
      }
    }
    portEXIT_CRITICAL(&buttonMux);
  }
}

// Il tasto risulta premuto adesso, secondo la macchina a stati. Serve al
// reset WiFi all'accensione, che deve sapere quanto a lungo e' tenuto giu'
// senza rileggersi il pin per conto suo.
bool isButtonHeld(const Button &button) {
  return button.pressed;
}

// Butta via le pressioni accodate senza servirle.
void clearButtonQueue() {
  portENTER_CRITICAL(&buttonMux);
  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    buttons[i].pressCount = 0;
    buttons[i].longPressCount = 0;
  }
  portEXIT_CRITICAL(&buttonMux);
}

// ---------------------------------------------------------------------------
// DIAGNOSTICA DEI PULSANTI
// ---------------------------------------------------------------------------
// Attivabile da BUTTON_DIAGNOSTICS in Config.h. Stampa ogni secondo, per
// ciascun tasto, i fronti visti e quanti ne ha scartati l'antirimbalzo.
// Come si legge, premendo una volta sola:
//   fronti 2,  scarti 0  -> contatto pulito
//   fronti 30, scarti 28 -> rimbalza parecchio, ma il filtro tiene
//   fronti 30, scarti 10 -> il filtro non copre il rimbalzo: alzare
//                           BUTTON_DEBOUNCE_US
//   fronti 0,  scarti 0  -> il segnale non arriva al pin: e' hardware
void reportButtonDiagnostics() {
  if (!BUTTON_DIAGNOSTICS) {
    return;
  }

  static unsigned long lastReportMs = 0;
  static uint32_t previousEdges[BUTTON_COUNT] = {0};
  static uint32_t previousBounces[BUTTON_COUNT] = {0};
  static uint32_t previousResyncs[BUTTON_COUNT] = {0};

  if (millis() - lastReportMs < BUTTON_DIAG_REPORT_MS) {
    return;
  }
  lastReportMs = millis();

  bool anyActivity = false;

  // La riga si compone in un buffer sullo stack. Con String ogni += era una
  // riallocazione, una volta al secondo, per sempre: e' esattamente il tipo di
  // churn che a lungo andare sbriciola l'heap.
  char report[160];
  size_t used = 0;
  used += snprintf(report + used, sizeof(report) - used, "BTN ");

  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    uint32_t edges = buttons[i].edgeCount;
    uint32_t bounces = buttons[i].bounceCount;
    uint32_t deltaEdges = edges - previousEdges[i];
    uint32_t deltaBounces = bounces - previousBounces[i];
    previousEdges[i] = edges;
    previousBounces[i] = bounces;

    if (deltaEdges > 0) {
      anyActivity = true;
    }

    if (used < sizeof(report)) {
      used += snprintf(report + used, sizeof(report) - used,
                       "%s fronti=%lu scarti=%lu",
                       buttons[i].label,
                       static_cast<unsigned long>(deltaEdges),
                       static_cast<unsigned long>(deltaBounces));
    }

    if (buttons[i].resyncCount != previousResyncs[i]) {
      if (used < sizeof(report)) {
        used += snprintf(report + used, sizeof(report) - used, " RECUPERI=%lu",
                         static_cast<unsigned long>(buttons[i].resyncCount -
                                                    previousResyncs[i]));
      }
      anyActivity = true;
    }
    previousResyncs[i] = buttons[i].resyncCount;

    if (used < sizeof(report)) {
      used += snprintf(report + used, sizeof(report) - used, "  ");
    }
  }

  // Silenzio quando non succede nulla: cosi' la seriale resta leggibile e
  // ogni riga stampata corrisponde a qualcosa che hai fatto davvero.
  if (anyActivity) {
    Serial.println(report);
  }
}

// ---------------------------------------------------------------------------
// NAVIGAZIONE
// ---------------------------------------------------------------------------
// K1 richiama questa funzione: avanza alla schermata successiva e torna
// alla prima quando supera PAGE_COUNT, poi richiede un ridisegno completo.
void goToNextPage() {
  currentPage = static_cast<Page>((currentPage + 1) % PAGE_COUNT);

  // Spotify viene interrogato solo mentre la sua pagina e' visibile (vedi
  // refreshSpotifyData): entrandoci si azzera il timer di poll per avere dati
  // freschi gia' al primo disegno.
  if (currentPage == PAGE_SPOTIFY) {
    spotifyState.lastPollMs = 0;
  }

  // Stessa ragione per il sensore: a pagina spenta l'ultima lettura puo'
  // essere di mezzo minuto fa, e non e' quello che uno si aspetta di vedere
  // appena apre la pagina.
  if (currentPage == PAGE_DHT) {
    dhtState.lastReadMs = 0;
  }

  markDisplayDirty();
}

// ---------------------------------------------------------------------------
// TIMER POMODORO
// ---------------------------------------------------------------------------
void resetPomodoroPhase(bool breakPhase) {
  pomodoroState.isBreak = breakPhase;
  pomodoroState.phaseDurationMs = breakPhase ? POMODORO_BREAK_MS : POMODORO_WORK_MS;
  pomodoroState.phaseStartMs = millis();
  pomodoroState.remainingMs = pomodoroState.phaseDurationMs;
  pomodoroState.lastTickMs = millis();
}

// Fine di una fase. La successiva viene preparata ma non avviata: e' la
// schermata 11 del design, quella che annuncia il pomodoro concluso e mette
// "AVVIA PAUSA" sul terzo tasto. Far ripartire il timer da solo toglierebbe
// di mezzo proprio il momento in cui ci si deve alzare dalla sedia.
void completePomodoroPhase() {
  if (!pomodoroState.isBreak) {
    pomodoroState.completedWorkSessions++;
  }

  resetPomodoroPhase(!pomodoroState.isBreak);
  pomodoroState.running = false;
  markDisplayDirty();
}

// Sospende il conto alla rovescia lasciando il tempo dov'e': ripartira' da
// li' alla pressione successiva.
void pausePomodoroTimer() {
  if (!pomodoroState.running) {
    return;
  }

  pomodoroState.running = false;
  markDisplayDirty();
}

// Salta la pausa e riporta il timer all'inizio di una fase di lavoro, senza
// avviarla. E' il tasto K2 della schermata del pomodoro concluso.
void skipPomodoroBreak() {
  pomodoroState.running = false;
  resetPomodoroPhase(false);
  markDisplayDirty();
}

// Secondi mancanti alla fine della fase, arrotondati per eccesso.
// L'arrotondamento per eccesso e' quello che rende un conto alla rovescia
// corretto: la fase parte mostrando 40:00 e lo tiene per un secondo pieno,
// e lo zero compare solo quando il tempo e' davvero finito. Con la divisione
// intera secca, invece, 40:00 sparirebbe dopo un millisecondo.
unsigned long pomodoroRemainingSeconds() {
  return (pomodoroState.remainingMs + 999UL) / 1000UL;
}

void updatePomodoroState() {
  if (!pomodoroState.running) {
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - pomodoroState.phaseStartMs;

  if (elapsed >= pomodoroState.phaseDurationMs) {
    completePomodoroPhase();
  } else {
    pomodoroState.remainingMs = pomodoroState.phaseDurationMs - elapsed;
  }

  pomodoroState.lastTickMs = now;
}

void startPomodoroTimer() {
  if (pomodoroState.running) {
    return;
  }

  pomodoroState.phaseStartMs = millis() - (pomodoroState.phaseDurationMs - pomodoroState.remainingMs);
  pomodoroState.lastTickMs = millis();
  pomodoroState.running = true;
  markDisplayDirty();
}

void resetPomodoroTimer() {
  pomodoroState.running = false;
  resetPomodoroPhase(false);
  markDisplayDirty();
}

// ---------------------------------------------------------------------------
// WIFI
// ---------------------------------------------------------------------------
// La rete di casa non e' scritta da nessuna parte nel codice. Alla prima
// accensione il dispositivo crea l'access point WIFI_PORTAL_SSID: ci si
// collega da telefono, si apre 192.168.4.1 e si sceglie la rete. Da quel
// momento WiFiManager conserva le credenziali nella NVS dell'ESP32 e
// autoConnect() le riusa in silenzio a ogni avvio.
//
// Quel salvataggio e' anche il motivo per cui serve un modo esplicito per
// dimenticare la rete: vedi checkWifiResetRequest() e forgetWifiAndReconfigure().
void connectToWifi() {
  WiFiManager wifiManager;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wifiManager.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);

  // In fondo alla pagina della rete compaiono anche i campi di Spotify e la
  // citta' del meteo: si scrivono qui, insieme alla password del WiFi, senza
  // dover ricompilare niente.
  //
  // L'ordine conta per via del callback: WiFiManager ne tiene uno solo, e
  // addSpotifyPortalParams() ne registra uno proprio che salva i soli campi
  // Spotify. Qui i campi sono di due famiglie, quindi dopo averli aggiunti
  // tutti si sovrascrive con quello che le salva entrambe.
  addSpotifyPortalParams(wifiManager);
  addWeatherPortalParams(wifiManager);
  wifiManager.setSaveParamsCallback(storePortalParams);

  // Consuma la richiesta di reset: cancella le credenziali salvate, cosi'
  // l'autoConnect qui sotto non ha piu' nulla da riusare e apre il portale.
  if (wifiResetRequested) {
    wifiResetRequested = false;
    wifiManager.resetSettings();
    Serial.println("Credenziali WiFi cancellate dalla NVS");
    drawBootScreen("rete dimenticata", "credenziali cancellate dalla NVS", false, 0.25f, "ATTENDERE");
    delay(1500);
  }

  char portalLabel[48];
  snprintf(portalLabel, sizeof(portalLabel), "collegati a %s", WIFI_PORTAL_SSID);
  drawBootScreen(portalLabel, "poi apri 192.168.4.1 dal telefono", true, 0.45f,
                 "CONFIGURAZIONE RETE");
  Serial.println("Configurazione WiFi con WiFiManager");
  Serial.print("Se necessario connettiti all'AP ");
  Serial.print(WIFI_PORTAL_SSID);
  Serial.println(" e apri http://192.168.4.1");

  bool connected = wifiManager.autoConnect(WIFI_PORTAL_SSID);

  if (!connected) {
    Serial.println("WiFi non connesso");
    drawBootScreen("rete non raggiungibile", "riavvia il dispositivo per riprovare", false, 1.0f, "WIFI ASSENTE");
    return;
  }

  // Gli ottetti si leggono uno per uno: IPAddress::toString() costruirebbe
  // una String solo per essere concatenata a un'altra e buttata via.
  IPAddress ip = WiFi.localIP();
  char ipText[16];
  snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  char addressLabel[32];
  snprintf(addressLabel, sizeof(addressLabel), "connesso - %s", ipText);

  Serial.print("WiFi connesso. IP: ");
  Serial.println(ipText);
  drawBootScreen(addressLabel, "", false, 1.0f, "RETE PRONTA");
  delay(2000);
}

// Via a freddo per dimenticare la rete: tasto tenuto premuto all'accensione.
// Vale la pena averla separata da quella nel menu perche' funziona anche
// quando il dispositivo non riesce piu' a collegarsi e quindi la pagina
// orologio non e' raggiungibile.
//
// Chiamata dopo setupButtons() e setupDisplay(): serve il pin gia' in
// INPUT_PULLUP e il pannello gia' inizializzato per dare un riscontro a video.
// Attende che il tasto del reset venga mollato. Senza questa attesa la
// pressione appena conclusa finirebbe in coda e verrebbe servita sulla prima
// pagina che compare, come se l'avessi premuto li'.
void waitForWifiResetRelease() {
  while (true) {
    resyncButtonStates();
    if (!isButtonHeld(buttons[BUTTON_WIFI_RESET_INDEX])) {
      return;
    }
    delay(20);
  }
}

void checkWifiResetRequest() {
  if (!isButtonHeld(buttons[BUTTON_WIFI_RESET_INDEX])) {
    return;
  }

  drawBootScreen("tieni premuto K4", "per dimenticare la rete salvata", true, 0.0f, "RESET WIFI");

  unsigned long holdStart = millis();

  // Si interroga la macchina a stati dei pulsanti, non il pin: cosi' il
  // rimbalzo del contatto non fa sembrare rilasciato un tasto che e' ancora
  // giu', e l'attesa non si interrompe a meta'.
  //
  // resyncButtonStates() va chiamata a mano a ogni giro: il loop principale
  // non e' ancora partito, e se il fronte di rilascio andasse perso senza la
  // rete di sicurezza questa attesa non finirebbe piu'.
  while (true) {
    resyncButtonStates();
    if (!isButtonHeld(buttons[BUTTON_WIFI_RESET_INDEX])) {
      break;
    }

    unsigned long held = millis() - holdStart;

    if (held >= WIFI_RESET_HOLD_MS) {
      wifiResetRequested = true;
      Serial.println("Reset WiFi richiesto all'accensione");
      drawBootScreen("confermato", "rilascia il tasto", false, 1.0f, "RESET WIFI");
      waitForWifiResetRelease();
      return;
    }

    // La barra racconta quanto manca: senza, i tre secondi sono un tempo
    // morto in cui non si capisce se il dispositivo abbia sentito il tasto.
    drawBootProgress(static_cast<float>(held) / static_cast<float>(WIFI_RESET_HOLD_MS));
    delay(20);
  }

  // Rilasciato prima del tempo: nessuna modifica alla rete salvata.
  Serial.println("Reset WiFi annullato");
  drawBootScreen("annullato", "la rete salvata resta al suo posto", false, 0.0f, "RESET WIFI");
  delay(900);
}

// Via a caldo, dalla pagina orologio. Cancella la rete e riapre subito il
// portale, cosi' si puo' passare a un'altra rete senza spegnere il dispositivo.
void forgetWifiAndReconfigure() {
  wifiForgetArmed = false;
  wifiResetRequested = true;

  connectToWifi();
  if (WiFi.status() == WL_CONNECTED) {
    initTimeClock();
    ensureSpotifyClient();
  }

  markDisplayDirty();
}

// ---------------------------------------------------------------------------
// OROLOGIO NTP
// ---------------------------------------------------------------------------
void initTimeClock() {
  configTzTime(TZ_INFO, NTP_SERVER);
  Serial.println("Sincronizzazione NTP impostata su timezone Italia");
}

// ---------------------------------------------------------------------------
// SENSORE DHT22
// ---------------------------------------------------------------------------
void readDhtSensor() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  dhtState.lastReadMs = millis();
  if (isnan(temperature) || isnan(humidity)) {
    dhtState.valid = false;
    // Il contatore non si azzera qui: e' il numero di tentativi andati a
    // vuoto di fila, ed e' quello che la schermata 05 mostra al posto
    // dell'umidita' per far capire se e' un singolo salto o un guasto.
    if (dhtState.failureCount < 9999) {
      dhtState.failureCount++;
    }
    return;
  }

  dhtState.temperature = temperature;
  dhtState.humidity = humidity;
  dhtState.valid = true;
  dhtState.failureCount = 0;

  // Ora dell'ultima lettura buona, per la riga "ultima HH:MM" del ramo di
  // errore. Se l'NTP non ha ancora agganciato resta vuota e la schermata
  // ripiega su "mai letta".
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 0)) {
    strftime(dhtState.lastValidClock, sizeof(dhtState.lastValidClock), "%H:%M", &timeInfo);
  }
}

// ---------------------------------------------------------------------------
// SPOTIFY
// ---------------------------------------------------------------------------
// Costruisce il client con le credenziali salvate. Tre esiti possibili:
//   - manca l'app (Client ID / Secret): la pagina chiede di configurarla;
//   - c'e' l'app ma non l'account: parte il login con il QR code;
//   - c'e' tutto: il player e' pronto.
void initSpotify() {
  if (spotifyClient != nullptr) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "WiFi assente");
    return;
  }

  loadSpotifyCredentials();

  if (spotifyCredentials.clientId[0] == '\0' || spotifyCredentials.clientSecret[0] == '\0') {
    spotifyState.appKeysMissing = true;
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "App Spotify da configurare");
    return;
  }
  spotifyState.appKeysMissing = false;

  bool hasAccount = spotifyCredentials.refreshToken[0] != '\0';

  if (hasAccount) {
    spotifyClient = new Spotify(spotifyCredentials.clientId, spotifyCredentials.clientSecret,
                                spotifyCredentials.refreshToken);
  } else {
    spotifyClient = new Spotify(spotifyCredentials.clientId, spotifyCredentials.clientSecret);
  }

  // Senza refresh token begin() stampa sulla seriale un suo URL di login:
  // va ignorato, il login che conta e' quello del QR code, generato qui sotto
  // con uno state che il dispositivo conosce e puo' andare a ritirare.
  spotifyClient->begin();
  spotifyState.configured = true;
  spotifyState.awaitingLogin = !hasAccount;
  spotifyState.authenticated = hasAccount;

  if (hasAccount) {
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "Account collegato");
  } else {
    newSpotifyLoginState();
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "In attesa del login");
  }
}

// Da chiamare ogni volta che la rete torna o il portale si chiude: se il
// portale ha cambiato l'app il client va ricostruito da capo, altrimenti
// basta crearlo quando ancora non c'e'.
void ensureSpotifyClient() {
  if (spotifyCredentialsChanged) {
    spotifyCredentialsChanged = false;
    restartSpotify();
    return;
  }

  initSpotify();
}

// Traduce una risposta fallita in una riga leggibile a schermo, e stampa sulla
// seriale il messaggio completo di Spotify. Sono i casi che prima finivano
// sotto "NESSUN BRANO" e sembravano un player semplicemente fermo.
void describeSpotifyError(const response &res) {
  const char *hint;
  switch (res.status_code) {
    case -1:  hint = "token rifiutato o rete"; break;
    case 400: hint = "richiesta non valida"; break;
    case 401: hint = "token scaduto o revocato"; break;
    case 403: hint = "utente non abilitato all'app"; break;
    case 429: hint = "troppe richieste"; break;
    default:  hint = "errore Spotify"; break;
  }

  snprintf(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
           "HTTP %d - %s", res.status_code, hint);

  const char *detail = res.reply["error"]["message"] | "";
  if (detail[0] == '\0') {
    detail = res.reply.is<const char *>() ? res.reply.as<const char *>() : "";
  }
  Serial.printf("Spotify HTTP %d: %s\n", res.status_code, detail);
}

void updateSpotifyState() {
  if (WiFi.status() != WL_CONNECTED) {
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "WiFi assente");
    spotifyState.playbackActive = false;
    return;
  }

  // Senza client il motivo l'ha gia' scritto initSpotify() (app da
  // configurare); senza account il login lo porta avanti serviceSpotifyLogin().
  if (spotifyClient == nullptr || !spotifyClient->is_auth()) {
    spotifyState.playbackActive = false;
    return;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"] = true;
  // Senza questa riga la copertina non arriva: il filtro scarta tutto cio'
  // che non e' dichiarato, e `album` ne faceva parte.
  filter["item"]["album"]["images"] = true;

  response res = spotifyClient->get_currently_playing_track(filter);

  // L'access token dura un'ora. La libreria lo rinnova da sola solo quando
  // manca del tutto; quando e' scaduto Spotify risponde 401 e, senza questo
  // rinnovo, da un'ora dopo l'avvio ogni poll fallirebbe per sempre.
  if (res.status_code == 401 && spotifyClient->get_access_token()) {
    res = spotifyClient->get_currently_playing_track(filter);
  }

  spotifyState.lastStatusCode = res.status_code;

  if (res.status_code == 204) {
    spotifyState.playbackActive = false;
    spotifyState.isPlaying = false;
    spotifyState.trackName[0] = '\0';
    spotifyState.artistName[0] = '\0';
    spotifyState.progressMs = 0;
    spotifyState.durationMs = 0;
    spotifyState.artUrl[0] = '\0';
    copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage), "Nessuna riproduzione");
    return;
  }

  if (res.status_code < 200 || res.status_code >= 300 || res.reply.isNull()) {
    spotifyState.playbackActive = false;
    spotifyState.isPlaying = false;
    describeSpotifyError(res);
    return;
  }

  spotifyState.playbackActive = !res.reply["item"].isNull();
  spotifyState.isPlaying = res.reply["is_playing"] | false;
  spotifyState.progressMs = res.reply["progress_ms"] | 0UL;
  spotifyState.durationMs = res.reply["item"]["duration_ms"] | 0UL;
  // I valori arrivano da ArduinoJson come const char* che puntano nel buffer
  // del documento: si copiano nei nostri, senza mai costruire una String.
  copyText(spotifyState.trackName, sizeof(spotifyState.trackName),
           res.reply["item"]["name"] | "");
  spotifyState.artistName[0] = '\0';

  JsonArray artists = res.reply["item"]["artists"].as<JsonArray>();
  for (JsonVariant artist : artists) {
    const char *artistLabel = artist["name"] | "";
    if (artistLabel[0] == '\0') {
      continue;
    }
    if (spotifyState.artistName[0] != '\0') {
      appendText(spotifyState.artistName, sizeof(spotifyState.artistName), ", ");
    }
    appendText(spotifyState.artistName, sizeof(spotifyState.artistName), artistLabel);
  }

  if (spotifyState.trackName[0] == '\0') {
    copyText(spotifyState.trackName, sizeof(spotifyState.trackName), "Brano sconosciuto");
  }
  if (spotifyState.artistName[0] == '\0') {
    copyText(spotifyState.artistName, sizeof(spotifyState.artistName), "Artista sconosciuto");
  }

  selectAlbumArtUrl(res.reply["item"]);

  copyText(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
                 spotifyState.isPlaying ? "Riproduzione attiva" : "In pausa");
}

// Sceglie quale delle copertine scaricare.
//
// Spotify ne offre tre - 640, 300 e 64 px - e la misura giusta non e' una
// preferenza estetica: il decoder riduce solo per 1/2, 1/4 e 1/8, quindi
// soltanto una sorgente da ART_SOURCE_PX px produce esattamente il riquadro
// da ART_SIZE. Si cerca quella misura esatta; se manca si prende la piu'
// piccola che la contenga, cosi' la riduzione resta possibile anche se un
// giorno l'elenco cambia.
void selectAlbumArtUrl(JsonVariant item) {
  spotifyState.artUrl[0] = '\0';

  JsonArray images = item["album"]["images"].as<JsonArray>();
  if (images.isNull()) {
    return;
  }

  const char *best = nullptr;
  int bestHeight = 0;

  for (JsonVariant image : images) {
    const char *url = image["url"] | "";
    int height = image["height"] | 0;

    if (url[0] == '\0' || height <= 0) {
      continue;
    }

    if (height == ART_SOURCE_PX) {
      best = url;
      bestHeight = height;
      break;
    }

    // Nessuna corrispondenza esatta: si tiene la piu' piccola disponibile.
    // La riduzione e' fissa, quindi una sorgente di misura diversa verra'
    // comunque rifiutata da artFetch - ma almeno il registro dira' quale.
    if (best == nullptr || height < bestHeight) {
      best = url;
      bestHeight = height;
    }
  }

  if (best == nullptr) {
    return;
  }

  if (bestHeight != ART_SOURCE_PX) {
    Serial.printf("[art] nessuna copertina da %d px, uso quella da %d\n",
                  ART_SOURCE_PX, bestHeight);
  }

  copyText(spotifyState.artUrl, sizeof(spotifyState.artUrl), best);
}

void spotifyPreviousTrack() {
  if (spotifyClient == nullptr || !spotifyClient->is_auth()) {
    return;
  }

  response res = spotifyClient->skip_to_previous();
  snprintf(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
           "Prev HTTP %d", res.status_code);
  spotifyState.lastPollMs = 0;
}

void spotifyTogglePlayback() {
  if (spotifyClient == nullptr || !spotifyClient->is_auth()) {
    return;
  }

  response res = spotifyState.isPlaying ? spotifyClient->pause_playback() : spotifyClient->start_a_users_playback();
  snprintf(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
           "Play HTTP %d", res.status_code);
  spotifyState.lastPollMs = 0;
}

void spotifyNextTrack() {
  if (spotifyClient == nullptr || !spotifyClient->is_auth()) {
    return;
  }

  response res = spotifyClient->skip_to_next();
  snprintf(spotifyState.statusMessage, sizeof(spotifyState.statusMessage),
           "Next HTTP %d", res.status_code);
  spotifyState.lastPollMs = 0;
}

// ---------------------------------------------------------------------------
// AGGIORNAMENTI PERIODICI
// ---------------------------------------------------------------------------
// Ogni modulo ha il proprio intervallo e invalida la grafica solo se e'
// la pagina visualizzata a essere interessata dal cambiamento.
//
// L'orologio non compare qui: non c'e' niente da aggiornare periodicamente,
// perche' l'ora vera si legge direttamente in fase di disegno.
void refreshDhtData() {
  // A pagina spenta si legge molto di rado: la lettura del DHT22 tiene gli
  // interrupt disabilitati per tutta la sua durata, e in quella finestra le
  // pressioni dei pulsanti non vengono viste. Vale la stessa regola gia'
  // applicata a Spotify - il lavoro costoso si fa solo per cio' che e' sotto
  // gli occhi - ma qui il costo non e' la latenza: e' l'input che si perde.
  unsigned long interval = (currentPage == PAGE_DHT) ? DHT_REFRESH_MS
                                                     : DHT_IDLE_REFRESH_MS;

  if (millis() - dhtState.lastReadMs < interval) {
    return;
  }

  bool previousValid = dhtState.valid;
  float previousTemperature = dhtState.temperature;
  float previousHumidity = dhtState.humidity;

  readDhtSensor();

  // Il confronto usa fabs: con letture non valide i valori sono NaN e il
  // confronto risulta falso, cosi' resta la sola transizione di validita'.
  bool dhtChanged =
    previousValid != dhtState.valid ||
    fabs(previousTemperature - dhtState.temperature) >= 0.1f ||
    fabs(previousHumidity - dhtState.humidity) >= 0.1f;

  if (currentPage == PAGE_DHT && dhtChanged) {
    markDisplayDirty();
  }
}

// Interroga Spotify solo mentre la sua pagina e' visibile.
//
// currently_playing() e' una chiamata HTTPS sincrona: fra handshake TLS e
// risposta puo' tenere fermo il loop per centinaia di millisecondi, a volte
// oltre il secondo. Lasciandola girare in sottofondo ogni cinque secondi era
// lei a mangiarsi i tick dell'orologio e del Pomodoro, che restavano indietro
// o saltavano un secondo di netto.
//
// Non si perde nulla: quei dati si vedono solo nella pagina Spotify, e
// goToNextPage() azzera lastPollMs entrandoci, cosi' il primo disegno e' gia'
// aggiornato.
void refreshSpotifyData() {
  if (currentPage != PAGE_SPOTIFY) {
    // La conferma per scollegare l'account non sopravvive all'uscita dalla
    // pagina: tornandoci non si deve trovare una domanda gia' armata.
    spotifyState.unlinkArmed = false;
    return;
  }

  // Prima si disegna, poi si interroga. Nel loop il rendering viene dopo i
  // task di sottofondo: senza questa attesa, entrando nella pagina il poll
  // bloccava il vecchio contenuto a schermo per uno o due secondi, e la
  // seconda pressione di K1 fatta "perche' non ha risposto" restava in coda e
  // scavalcava la pagina Spotify appena comparsa. Cosi' la pagina appare
  // subito con i dati in cache e il poll parte al giro successivo.
  if (displayDirty) {
    return;
  }

  // Con il QR code a schermo non c'e' un player da interrogare: si chiede
  // invece al server del login se qualcuno l'ha completato. Ha un suo ritmo.
  if (spotifyState.awaitingLogin) {
    serviceSpotifyLogin();
    return;
  }

  if (millis() - spotifyState.lastPollMs < SPOTIFY_REFRESH_MS) {
    return;
  }

  bool previousAwaitingLogin = spotifyState.awaitingLogin;
  bool previousAuthenticated = spotifyState.authenticated;
  bool previousPlaybackActive = spotifyState.playbackActive;
  bool previousIsPlaying = spotifyState.isPlaying;
  int previousStatusCode = spotifyState.lastStatusCode;

  // Le tre copie di confronto stanno sullo stack. Erano tre String costruite
  // e distrutte a ogni poll, cioe' ogni cinque secondi per tutto il tempo in
  // cui la pagina Spotify resta aperta: sei allocazioni al giro, di misure
  // sempre diverse perche' dipendono dal titolo del brano. Era il modo piu'
  // efficace di frammentare l'heap proprio prima di chiedergli i kilobyte
  // contigui dell'handshake TLS successivo.
  char previousTrackName[TEXT_TRACK_MAX];
  char previousArtistName[TEXT_TRACK_MAX];
  char previousStatusMessage[TEXT_STATUS_MAX];
  copyText(previousTrackName, sizeof(previousTrackName), spotifyState.trackName);
  copyText(previousArtistName, sizeof(previousArtistName), spotifyState.artistName);
  copyText(previousStatusMessage, sizeof(previousStatusMessage), spotifyState.statusMessage);

  spotifyState.lastPollMs = millis();
  updateSpotifyState();

  bool spotifyChanged =
    previousAwaitingLogin != spotifyState.awaitingLogin ||
    previousAuthenticated != spotifyState.authenticated ||
    previousPlaybackActive != spotifyState.playbackActive ||
    previousIsPlaying != spotifyState.isPlaying ||
    previousStatusCode != spotifyState.lastStatusCode ||
    strcmp(previousTrackName, spotifyState.trackName) != 0 ||
    strcmp(previousArtistName, spotifyState.artistName) != 0 ||
    strcmp(previousStatusMessage, spotifyState.statusMessage) != 0;

  if (currentPage == PAGE_SPOTIFY && spotifyChanged) {
    markDisplayDirty();
  }

  // Dopo il poll, non dentro: lo scaricamento della copertina apre una seconda
  // sessione TLS, e aprirla mentre quella di Spotify e' ancora in piedi
  // vorrebbe il doppio del picco di heap.
  refreshAlbumArt();
}

// Scade la prima meta' della conferma a due pressioni: passato il tempo
// utile, K4 sulla pagina orologio torna a essere innocuo.
void refreshWifiForgetArming() {
  if (!wifiForgetArmed) {
    return;
  }
  if (millis() - wifiForgetArmedMs <= WIFI_FORGET_CONFIRM_MS) {
    return;
  }

  wifiForgetArmed = false;
  if (currentPage == PAGE_CLOCK) {
    markDisplayDirty();
  }
}

// Sorveglia il collegamento e reagisce alle transizioni, non allo stato.
//
// WiFi.setAutoReconnect(true) rimette in piedi il collegamento da solo, ma
// nessuno avvisa il resto del sistema: l'ora resta quella vecchia finche' non
// si risincronizza l'NTP, e il player continua a credere di essere scollegato
// fino al poll successivo. Qui si intercetta il momento in cui la rete torna
// e si rimette tutto in moto.
void refreshNetworkLink() {
  static bool wasOnline = false;

  bool online = (WiFi.status() == WL_CONNECTED);
  if (online == wasOnline) {
    return;
  }
  wasOnline = online;

  if (online) {
    Serial.println("Rete tornata disponibile: risincronizzo NTP e player");
    initTimeClock();
    // Se il client e' gia' costruito non si tocca: updateSpotifyState()
    // rinegozia il token da solo al primo poll utile. Se all'avvio la rete
    // mancava, invece, il client non esiste ancora e va creato adesso.
    ensureSpotifyClient();
    spotifyState.lastPollMs = 0;
  } else {
    Serial.println("Rete caduta");
  }

  markDisplayDirty();
}

void refreshPageData() {
  // Prima di tutto il resto: un comando scritto sul seriale deve avere
  // effetto su questa stessa passata, non sulla prossima.
  pollSerialConsole();

  refreshNetworkLink();
  refreshDhtData();
  refreshWeatherData();
  refreshSpotifyData();
  refreshWifiForgetArming();
  refreshSpotifyUnlinkArming();
  reportButtonDiagnostics();

  // Fa avanzare il conto alla rovescia e gestisce il passaggio di fase.
  // Il ridisegno non lo decide qui: se ne accorge renderPomodoroTick().
  updatePomodoroState();
}

// ---------------------------------------------------------------------------
// GESTIONE INPUT PER SINGOLA SCHERMATA
// ---------------------------------------------------------------------------
void handleClockButtonPress(size_t index) {
  switch (index) {
    case 0:
      goToNextPage();
      break;
    case 1:
      // Senza rete una risincronizzazione NTP non puo' che fallire: il tasto
      // e' disegnato spento e qui resta muto, invece di far sembrare che sia
      // successo qualcosa.
      if (WiFi.status() == WL_CONNECTED) {
        initTimeClock();
        markDisplayDirty();
      }
      break;
    case 2:
      if (WiFi.status() != WL_CONNECTED) {
        connectToWifi();
        if (WiFi.status() == WL_CONNECTED) {
          initTimeClock();
          ensureSpotifyClient();
        }
      }
      markDisplayDirty();
      break;
    case 3:
      // Conferma a due pressioni: la prima arma e lo dichiara a schermo, la
      // seconda entro WIFI_FORGET_CONFIRM_MS cancella davvero la rete.
      if (wifiForgetArmed) {
        forgetWifiAndReconfigure();
      } else {
        wifiForgetArmed = true;
        wifiForgetArmedMs = millis();
        markDisplayDirty();
      }
      break;
    default:
      break;
  }
}

void handleDhtButtonPress(size_t index) {
  switch (index) {
    case 0:
      goToNextPage();
      break;
    case 1:
      // AGGIORNA, e RIPROVA quando il sensore non risponde: e' la stessa
      // lettura forzata, cambia solo l'etichetta sul tasto.
      readDhtSensor();
      markDisplayDirty();
      break;
    case 2:
      // CITTA': apre il portale di configurazione, dove si scrive il posto di
      // cui vedere il meteo.
      //
      // Sta qui perche' altrove non si arriva. Il portale si apriva solo dal
      // tasto CONFIGURA della pagina Spotify, che pero' compare solo quando
      // Spotify e' da configurare o in attesa di login: ad account collegato
      // quel tasto non esiste, e l'unico modo di cambiare citta' sarebbe
      // dimenticare la rete e rifare tutto l'aggancio WiFi. Il posto giusto
      // per quel comando e' la pagina in cui la citta' si vede.
      //
      // Prendeva questo tasto lo STORICO, che non esiste ancora: quando
      // arrivera', questo comando andra' su una pressione lunga.
      openSetupPortal();
      break;
    case 3:
      // CHIUDI riporta all'orologio, la pagina di riposo del dispositivo.
      currentPage = PAGE_CLOCK;
      markDisplayDirty();
      break;
    default:
      break;
  }
}

void handleSpotifyButtonPress(size_t index) {
  // Conferma per scollegare l'account in attesa: i tasti diventano
  // "annulla" e "conferma", qualunque cosa ci fosse sotto.
  if (spotifyState.unlinkArmed) {
    switch (index) {
      case 0:
        disarmSpotifyUnlink();
        goToNextPage();
        break;
      case 2:
        disarmSpotifyUnlink();
        markDisplayDirty();
        break;
      case 3:
        unlinkSpotifyAccount();
        break;
      default:
        break;
    }
    return;
  }

  // App da configurare o QR del login a schermo: K3 apre il portale per
  // scrivere Client ID e Secret, gli altri tasti non hanno nulla da fare.
  if (spotifyState.appKeysMissing || spotifyState.awaitingLogin) {
    if (index == 0) {
      goToNextPage();
    } else if (index == 2) {
      openSetupPortal();
    }
    return;
  }

  // Senza un brano caricato prec./succ. non hanno su cosa agire: il design
  // li disegna spenti, e qui restano muti. K3 in quello stato non e'
  // play/pausa ma RICERCA, cioe' una nuova interrogazione del player per
  // vedere se nel frattempo e' comparsa una sorgente.
  bool hasTrack = spotifyState.configured && spotifyState.authenticated &&
                  spotifyState.playbackActive;

  switch (index) {
    case 0:
      goToNextPage();
      break;
    case 1:
      if (hasTrack) {
        spotifyPreviousTrack();
        markDisplayDirty();
      }
      break;
    case 2:
      // Senza brano basta azzerare il timer: il poll parte al giro successivo,
      // dopo il ridisegno. Chiamarlo anche qui lo faceva partire due volte.
      if (hasTrack) {
        spotifyTogglePlayback();
      } else {
        spotifyState.lastPollMs = 0;
      }
      markDisplayDirty();
      break;
    case 3:
      // Senza brano il quarto tasto e' SCOLLEGA; con il brano e' SUCC., e
      // scollegare resta raggiungibile tenendolo premuto.
      if (hasTrack) {
        spotifyNextTrack();
        markDisplayDirty();
      } else if (spotifyState.authenticated) {
        armSpotifyUnlink();
      }
      break;
    default:
      break;
  }
}

void handlePomodoroButtonPress(size_t index) {
  switch (index) {
    case 0:
      goToNextPage();
      break;
    case 1:
      // A fase conclusa il secondo tasto salta la pausa; negli altri stati
      // chiude la pagina e torna all'orologio.
      if (pomodoroIsCompleted()) {
        skipPomodoroBreak();
      } else {
        currentPage = PAGE_CLOCK;
        markDisplayDirty();
      }
      break;
    case 2:
      // Un tasto solo per le tre etichette AVVIA / SOSPENDI / AVVIA PAUSA:
      // quale delle tre sia lo dice lo stato del timer.
      if (pomodoroState.running) {
        pausePomodoroTimer();
      } else {
        startPomodoroTimer();
      }
      break;
    case 3:
      resetPomodoroTimer();
      break;
    default:
      break;
  }
}

// Pressione lunga. Su K1 e' una scorciatoia di navigazione: da qualsiasi
// pagina riporta all'orologio, che e' la pagina di riposo del dispositivo.
// Sugli altri tasti vale come una pressione normale - tenere premuto per
// esitazione non deve lasciare il tasto senza risposta.
void handleLongButtonPress(size_t index) {
  // K4 tenuto premuto sulla pagina Spotify: scollega l'account, previa
  // conferma. Serve quando il dispositivo passa a un'altra persona.
  if (index == 3 && currentPage == PAGE_SPOTIFY && spotifyState.authenticated &&
      !spotifyState.unlinkArmed) {
    armSpotifyUnlink();
    return;
  }

  if (index != 0) {
    handleButtonPress(index);
    return;
  }

  Serial.println("K1 tenuto premuto: ritorno all'orologio");

  if (currentPage != PAGE_CLOCK) {
    currentPage = PAGE_CLOCK;
    markDisplayDirty();
  }
}

// Smista la pressione all'handler della schermata attiva.
void handleButtonPress(size_t index) {
  ScreenButtonFn handler = currentScreenView().onButton;
  if (handler != nullptr) {
    handler(index);
  }

  Serial.print("Premuto ");
  Serial.println(buttons[index].label);
}
