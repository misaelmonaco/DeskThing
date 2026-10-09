// ===========================================================================
// SCREEN VIEWS - impaginazione delle singole schermate su 320x240
// ---------------------------------------------------------------------------
// Le undici schermate del design "Schermate ST7789" vivono qui. Non sono
// undici pagine: sono quattro pagine (orologio, sensore, player, Pomodoro) che
// cambiano faccia a seconda dello stato, piu' la schermata di avvio. La
// navigazione con K1 gira sulle quattro, gli stati si accendono da soli.
//
//   01 avvio               -> drawBootScreen(), chiamata dal setup
//   02 orologio            -> drawClockPage(), rete presente
//   03 orologio, rete giu' -> drawClockPage(), ramo offline
//   04 sensore             -> drawDhtPage(), lettura valida
//   05 sensore muto        -> drawDhtPage(), ramo di errore
//   06 player in play      -> drawSpotifyPage(), riproduzione attiva
//   07 player in pausa     -> drawSpotifyPage(), ramo pausa
//   08 player scollegato   -> drawSpotifyPage(), nessuna sorgente
//      collega Spotify     -> drawSpotifyLoginPage(), QR code del login
//      configura app       -> drawSpotifyPage(), Client ID / Secret assenti
//   09 Pomodoro pronto     -> drawPomodoroPage(), timer fermo a inizio fase
//   10 focus in corso      -> drawPomodoroPage(), timer che scorre
//   11 Pomodoro concluso   -> drawPomodoroPage(), pausa da avviare
//
// Ogni pagina disegna da se' le proprie tre fasce: la barra di stato in alto,
// la cornice del contenuto e la barra dei quattro tasti. Le etichette dei
// tasti cambiano con lo stato - PAUSA diventa RIPRENDI, AVVIA diventa
// SOSPENDI - quindi non possono stare in una tabella statica.
//
// Le pagine con un valore che scorre (orologio e Pomodoro) non usano flag di
// "schermo sporco": confrontano il testo da mostrare con quello che risulta
// gia' a schermo, carattere per carattere, e ridipingono solo le celle che
// differiscono davvero. La cache stessa fa da stato di invalidazione.
// ===========================================================================

// ---------------------------------------------------------------------------
// COORDINATE DI PAGINA
// ---------------------------------------------------------------------------
// Orologio. L'ora e' "HH:MM" a dimensione 8 con i secondi a dimensione 3
// appoggiati in basso a destra, come il gruppo del design.
constexpr uint8_t CLOCK_HM_LEN  = 5;   // "HH:MM"
constexpr uint8_t CLOCK_SEC_LEN = 2;   // "SS"
constexpr uint8_t CLOCK_DATE_LEN = 10; // "GG/MM/AAAA"

constexpr int16_t CLOCK_HM_W    = textWidthPx(8, CLOCK_HM_LEN);
constexpr int16_t CLOCK_SEC_W   = textWidthPx(3, CLOCK_SEC_LEN);
constexpr int16_t CLOCK_GROUP_W = CLOCK_HM_W + 6 + CLOCK_SEC_W;
constexpr int16_t CLOCK_HM_X    = (SCREEN_W - CLOCK_GROUP_W) / 2;
constexpr int16_t CLOCK_SEC_X   = CLOCK_HM_X + CLOCK_HM_W + 6;

constexpr int16_t CLOCK_BLOCK_H = textHeightPx(8) + 10 + textHeightPx(2) + 10 + textHeightPx(1);
constexpr int16_t CLOCK_HM_Y    = CONTENT_Y + (CONTENT_H - CLOCK_BLOCK_H) / 2;
constexpr int16_t CLOCK_SEC_Y   = CLOCK_HM_Y + textHeightPx(8) - textHeightPx(3);
constexpr int16_t CLOCK_DATE_Y  = CLOCK_HM_Y + textHeightPx(8) + 10;
constexpr int16_t CLOCK_NOTE_Y  = CLOCK_DATE_Y + textHeightPx(2) + 10;
constexpr int16_t CLOCK_DATE_X  = centeredX(2, CLOCK_DATE_LEN);

// Sensore: due riquadri affiancati, fuori a sinistra e dentro a destra.
//
// Non sono larghi uguali. Quello di sinistra deve contenere il nome di una
// citta' e un'icona affiancata alla temperatura; quello di destra due numeri
// impilati, e il piu' largo e' "23.4" che a dimensione 4 misura 96 px: con i
// margini e l'unita' servono 138 px, e quello e' il vincolo che decide la
// spartizione. 156 + 6 + 138 fa esattamente i 300 della cornice.
constexpr int16_t DHT_GAP      = 6;
constexpr int16_t DHT_OUT_W    = 156;
constexpr int16_t DHT_IN_W     = FRAME_W - DHT_GAP - DHT_OUT_W;
constexpr int16_t DHT_LEFT_X   = FRAME_X;
constexpr int16_t DHT_RIGHT_X  = FRAME_X + DHT_OUT_W + DHT_GAP;
constexpr int16_t DHT_PAD_X    = 12;

// Riquadro esterno: etichetta, citta', temperatura con l'icona di fianco,
// condizione. L'icona e' incolonnata a destra e la temperatura le arriva
// accanto senza toccarla: tre cifre a dimensione 4 piu' l'unita' fanno 88 px,
// e l'icona comincia a 100.
constexpr int16_t DHT_ICON     = 44;
constexpr int16_t DHT_ICON_X   = DHT_LEFT_X + DHT_OUT_W - DHT_PAD_X - DHT_ICON;
constexpr int16_t DHT_OUT_H    = textHeightPx(1) + 4 + textHeightPx(2) + 10 +
                                 textHeightPx(4) + 6 + textHeightPx(1);

// Riquadro interno: due blocchi etichetta + numero, e la barra dell'umidita'.
constexpr int16_t DHT_IN_STEP  = textHeightPx(1) + 4 + textHeightPx(4);
constexpr int16_t DHT_IN_H     = DHT_IN_STEP + 12 + DHT_IN_STEP + 8 + 6;
constexpr int16_t DHT_BAR_W    = DHT_IN_W - 2 * DHT_PAD_X;

// Player: etichetta artista, titolo su due righe, riga di avanzamento.
//
// Il titolo sta a dimensione 3, non 4. Il design lo vuole grande, ma con il
// font integrato la dimensione 4 lascia passare undici caratteri per riga:
// "Everything In Its Right Place" diventerebbe "Everything ..." quasi sempre.
// A dimensione 3 ne entrano quindici, trenta su due righe, che e' l'ordine di
// grandezza del disegno. Un titolo leggibile vale piu' di sei pixel di corpo.
// Con la copertina a sinistra il titolo scende a dimensione 2. Non e' una
// perdita di testo: nella colonna che resta entrano sedici caratteri per riga,
// uno piu' dei quindici che ne entravano a dimensione 3 su tutta la larghezza.
// Si perde in altezza dei caratteri, non in quante parole si leggono, e su tre
// righe invece di due il totale sale da trenta a quarantotto caratteri.
constexpr uint8_t SPOT_TITLE_SIZE  = 2;
constexpr int16_t SPOT_PAD_X       = 14;
constexpr int16_t SPOT_TITLE_LINES = 3;
constexpr int16_t SPOT_TITLE_COLS  = ART_TEXT_W / textWidthPx(SPOT_TITLE_SIZE, 1);
constexpr int16_t SPOT_TITLE_STEP  = textHeightPx(SPOT_TITLE_SIZE) + 2 * SPOT_TITLE_SIZE;

// Login: il QR code a sinistra, le istruzioni a destra. Il QR non sta nella
// cornice ma prende tutta l'altezza fra la barra di stato e quella dei tasti:
// con 185 px l'URL del login entra a 3 px per modulo, nella cornice (176 px)
// scenderebbe a 2, e a 2 px su un pannello da 2 pollici un telefono economico
// fatica a metterlo a fuoco.
constexpr int16_t LOGIN_QR_X   = MARGIN_X;
constexpr int16_t LOGIN_QR_Y   = STATUS_Y + STATUS_H + 2;
constexpr int16_t LOGIN_QR_MAX = FOOTER_Y - 2 - LOGIN_QR_Y;
constexpr int16_t LOGIN_TEXT_GAP = 12;

// Pomodoro: timer grande, barra della fase, contatore dei pomodori.
constexpr uint8_t POMO_TIMER_LEN = 5;   // "MM:SS"
constexpr int16_t POMO_TIMER_X   = centeredX(8, POMO_TIMER_LEN);
constexpr int16_t POMO_BAR_W     = 210;
constexpr int16_t POMO_BAR_X     = (SCREEN_W - POMO_BAR_W) / 2;
constexpr int16_t POMO_PIPS_W    = 4 * 10 - 3;

// ---------------------------------------------------------------------------
// CACHE DI RENDERING
// ---------------------------------------------------------------------------
// Copia esatta di cio' che si trova attualmente sul pannello. Serve a decidere
// quali celle ridipingere; va invalidata ogni volta che la pagina viene
// ridisegnata da capo o che lo sfondo sotto al testo cambia.
char clockHmCache[CLOCK_HM_LEN];
char clockSecCache[CLOCK_SEC_LEN];
bool clockCacheValid = false;
bool clockLastSynced = false;
bool clockLastOnline = false;

char pomodoroTimerCache[POMO_TIMER_LEN];
bool pomodoroCacheValid = false;
int16_t pomodoroBarFilledPx = 0;

void invalidateScreenCaches() {
  clockCacheValid = false;
  pomodoroCacheValid = false;
}

// ---------------------------------------------------------------------------
// STATO CONDIVISO FRA LE PAGINE
// ---------------------------------------------------------------------------
bool networkOnline() {
  return WiFi.status() == WL_CONNECTED;
}

// L'ora breve che compare nella barra di stato delle pagine diverse
// dall'orologio. Stringa vuota se l'ora non e' ancora sincronizzata: meglio
// niente che un segnaposto che sembra un guasto.
void formatShortClock(char *out, size_t size) {
  if (out == nullptr || size == 0) {
    return;
  }

  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 0)) {
    out[0] = '\0';
    return;
  }

  strftime(out, size, "%H:%M", &timeInfo);
}

// Barra di stato comune alle pagine 2/4, 3/4 e 4/4: etichetta, ora e numero
// di pagina. L'orologio ha la sua, con le tacche del segnale al posto dell'ora.
void drawPageStatusBar(const char *label, uint16_t labelColor) {
  char clockText[TEXT_CLOCK_MAX];
  formatShortClock(clockText, sizeof(clockText));

  drawStatusBar(label, labelColor, false, 0, false,
                clockText, static_cast<uint8_t>(currentPage) + 1);
}

// Formatta un tempo in millisecondi come "M:SS", il formato dei due estremi
// della barra di avanzamento del player.
void formatTrackTime(char *out, size_t size, unsigned long milliseconds) {
  unsigned long totalSeconds = milliseconds / 1000UL;
  snprintf(out, size, "%lu:%02lu", totalSeconds / 60UL, totalSeconds % 60UL);
}

// ---------------------------------------------------------------------------
// PAGINA OROLOGIO - schermate 02 e 03
// ---------------------------------------------------------------------------
// Disegna ora e secondi confrontandoli con quello che c'e' gia' a schermo.
// Nel caso tipico l'unica cella ridipinta e' quella delle unita' dei secondi.
void renderClockFields() {
  struct tm timeInfo;

  // Il timeout va forzato a zero. Con il valore di default (5000 ms)
  // getLocalTime() resta a girare per cinque secondi ogni volta che l'ora non
  // e' ancora sincronizzata, bloccando il loop e mangiandosi i tick.
  bool synced = getLocalTime(&timeInfo, 0);
  bool online = networkOnline();

  // Le due varianti hanno colori diversi: al cambio di stato il confronto
  // cella per cella non basta e serve ripartire da uno sfondo pulito.
  if (synced != clockLastSynced || online != clockLastOnline) {
    clockLastSynced = synced;
    clockLastOnline = online;
    markDisplayDirty();
    return;
  }

  bool forceAll = !clockCacheValid;

  char hmBuffer[CLOCK_HM_LEN + 1];
  char secBuffer[CLOCK_SEC_LEN + 1];

  if (synced) {
    strftime(hmBuffer, sizeof(hmBuffer), "%H:%M", &timeInfo);
    strftime(secBuffer, sizeof(secBuffer), "%S", &timeInfo);
  } else {
    strcpy(hmBuffer, "--:--");
    strcpy(secBuffer, "--");
  }

  uint16_t hmColor  = online ? COL_INK : COL_ACC_300;
  uint16_t secColor = online ? COL_ACC_500 : COL_ACC_600;

  drawCachedText(hmBuffer, clockHmCache, CLOCK_HM_LEN,
                 CLOCK_HM_X, CLOCK_HM_Y, 8, hmColor, COL_BG, forceAll);

  drawCachedText(secBuffer, clockSecCache, CLOCK_SEC_LEN,
                 CLOCK_SEC_X, CLOCK_SEC_Y, 3, secColor, COL_BG, forceAll);

  clockCacheValid = true;
}

void drawClockPage() {
  struct tm timeInfo;
  bool synced = getLocalTime(&timeInfo, 0);
  bool online = networkOnline();

  clockLastSynced = synced;
  clockLastOnline = online;

  // Barra di stato: qui al posto dell'ora ci sono le tacche del segnale, che
  // e' l'informazione che in questa pagina conta davvero.
  drawStatusBar("ORA LOCALE", COL_ACC_600,
                true, online ? signalBarsFromRssi(WiFi.RSSI()) : 0, !online,
                "", static_cast<uint8_t>(currentPage) + 1);

  // Con la rete giu' o la conferma armata la cornice si accorcia per lasciare
  // spazio alla fascia di avviso sopra i tasti.
  bool hasNotice = !online || wifiForgetArmed;
  drawFrame(FRAME_X, FRAME_Y, FRAME_W,
            hasNotice ? FRAME_H_WITH_NOTICE : FRAME_H, COL_ACC_800);

  // Lo sfondo e' appena stato ripulito: la cache non descrive piu' lo schermo.
  clockCacheValid = false;
  renderClockFields();

  char dateBuffer[CLOCK_DATE_LEN + 1];
  if (synced) {
    strftime(dateBuffer, sizeof(dateBuffer), "%d/%m/%Y", &timeInfo);
  } else {
    strcpy(dateBuffer, "--/--/----");
  }
  drawLine(dateBuffer, CLOCK_DATE_X, CLOCK_DATE_Y, 2, COL_ACC_600);

  // La riga di servizio sotto la data sta solo nella variante in linea: senza
  // rete il suo posto lo prende la fascia di avviso.
  if (!hasNotice) {
    drawCenteredLine(synced ? "NTP - orario sincronizzato"
                            : "NTP - sincronizzazione in corso",
                     CLOCK_NOTE_Y, 1, COL_ACC_500);
  }

  // Prima meta' della conferma di cancellazione della rete WiFi: ha la
  // precedenza sull'avviso di rete assente, perche' e' una domanda in attesa
  // di risposta e non un semplice stato.
  if (wifiForgetArmed) {
    drawNotice("PREMI ANCORA K4", "per dimenticare la rete salvata");
  } else if (!online) {
    drawNotice("RETE ASSENTE", "orario dall'ultimo aggancio NTP");
  }

  KeyBar bar = {{
    {"PAGINA",    GLYPH_PAGE,  KEY_IDLE},
    {"NTP",       GLYPH_CLOCK, online ? KEY_IDLE : KEY_DISABLED},
    {online ? "WIFI" : "RICONNETTI", GLYPH_WIFI, online ? KEY_IDLE : KEY_PRIMARY},
    {"DIMENTICA", GLYPH_CROSS, wifiForgetArmed ? KEY_PRIMARY : KEY_IDLE},
  }};
  drawKeyBar(bar);
}

// ---------------------------------------------------------------------------
// PAGINA SENSORE DHT22 - schermate 04 e 05
// ---------------------------------------------------------------------------
// Un riquadro per grandezza. Nella variante di errore la cornice resta dov'e'
// e cambia solo il contenuto: il numero diventa un tratteggio spento. E' una
// scelta del design, e ha senso - lo schermo non si riorganizza sotto gli
// occhi ogni volta che una lettura salta.
// Blocco "etichetta + numero grande + unita'", che e' il modulo con cui sono
// fatti tutti e tre i valori della pagina. L'unita' si appoggia alla linea di
// base del numero, come nel disegno.
void drawDhtValue(int16_t x, int16_t y, const char *label, const char *value,
                  const char *unit, uint8_t size, uint16_t valueColor) {
  drawLine(label, x, y, 1, COL_ACC_600);

  int16_t valueY = y + textHeightPx(1) + 4;
  drawLine(value, x, valueY, size, valueColor);

  int16_t unitX = x + textWidthPx(size, textLen(value)) + 4;
  drawLine(unit, unitX, valueY + textHeightPx(size) - textHeightPx(2), 2, COL_ACC_500);
}

// Riquadro di sinistra: il meteo della citta' scelta nel portale.
//
// Tre stati, e nessuno dei tre mente. Dato fresco: numero pieno e condizione.
// Dato vecchio: stesso numero ma smorzato, e al posto della condizione l'ora
// a cui e' stato preso - resta leggibile, si vede che non e' di adesso.
// Nessun dato: trattini e il motivo, che e' sempre piu' utile di un riquadro
// vuoto.
void drawWeatherCard(int16_t cardH) {
  drawFrame(DHT_LEFT_X, FRAME_Y, DHT_OUT_W, cardH, COL_ACC_800);

  int16_t textX = DHT_LEFT_X + DHT_PAD_X;
  int16_t blockY = FRAME_Y + (cardH - DHT_OUT_H) / 2;

  drawLine("ESTERNO", textX, blockY, 1, COL_ACC_600);

  // Il nome della citta' prende la dimensione piu' grande in cui ci sta
  // intero. "Ariano Irpino" sono tredici caratteri e a dimensione 2 ne
  // passano undici, quindi scende a 1: troncare il nome di un posto per
  // difendere un corpo di testo sarebbe il compromesso sbagliato.
  const char *place = weather.place[0] != ' ' ? weather.place : weather.city;
  if (place[0] == ' ') {
    place = "nessuna citta'";
  }

  int16_t placeW = DHT_OUT_W - 2 * DHT_PAD_X;
  uint8_t placeSize = textWidthPx(2, textLen(place)) <= placeW ? 2 : 1;
  int16_t placeY = blockY + textHeightPx(1) + 4;
  drawFittedLine(place, textX, placeY, placeSize, COL_INK,
                 placeW / textWidthPx(placeSize, 1));

  int16_t valueY = placeY + textHeightPx(2) + 10;
  bool stale = weatherIsStale();

  if (weather.valid) {
    // Un grado di risoluzione, non un decimo: il dato viene da una griglia di
    // previsione, e il decimo sarebbe precisione finta.
    char temperature[8];
    snprintf(temperature, sizeof(temperature), "%.0f", weather.temperature);

    drawDhtValue(textX, valueY - textHeightPx(1) - 4, "", temperature, "C", 4,
                 stale ? COL_ACC_600 : COL_INK);

    char note[32];
    if (stale) {
      snprintf(note, sizeof(note), "fermo alle %s", weather.lastOkClock);
    } else {
      copyText(note, sizeof(note), weatherConditionName(weather.code));
    }

    drawFittedLine(note, textX, valueY + textHeightPx(4) + 6, 1,
                   stale ? COL_AMBER : COL_ACC_500, placeW / textWidthPx(1, 1));

    drawWeatherIcon(DHT_ICON_X + DHT_ICON / 2, valueY + textHeightPx(4) / 2,
                    DHT_ICON, weather.code);
  } else {
    drawDhtValue(textX, valueY - textHeightPx(1) - 4, "", "--", "C", 4, COL_ACC_700);
    drawFittedLine(weather.message, textX, valueY + textHeightPx(4) + 6, 1,
                   COL_ACC_600, placeW / textWidthPx(1, 1));
  }
}

// Riquadro di destra: i due numeri del DHT22, impilati.
void drawIndoorCard(int16_t cardH, bool valid) {
  drawFrame(DHT_RIGHT_X, FRAME_Y, DHT_IN_W, cardH, COL_ACC_800);

  int16_t textX = DHT_RIGHT_X + DHT_PAD_X;
  int16_t blockY = FRAME_Y + (cardH - DHT_IN_H) / 2;
  int16_t humidityY = blockY + DHT_IN_STEP + 12;

  char temperature[12];
  char humidity[12];

  if (valid) {
    snprintf(temperature, sizeof(temperature), "%.1f", dhtState.temperature);
    snprintf(humidity, sizeof(humidity), "%.0f", dhtState.humidity);
  } else {
    copyText(temperature, sizeof(temperature), "--.-");
    copyText(humidity, sizeof(humidity), "--");
  }

  uint16_t color = valid ? COL_INK : COL_ACC_700;

  drawDhtValue(textX, blockY, "TEMPERATURA", temperature, "C", 4, color);
  drawDhtValue(textX, humidityY, "UMIDITA", humidity, "%", 4, color);

  drawThinBar(textX, humidityY + DHT_IN_STEP + 8, DHT_BAR_W,
              valid ? dhtState.humidity / 100.0f : 0.0f, COL_ACC_400, COL_TRACK);
}

void drawDhtPage() {
  bool valid = dhtState.valid;

  drawPageStatusBar(valid ? "DHT22 - GPIO27" : "DHT22 - NESSUNA RISPOSTA",
                    valid ? COL_ACC_600 : COL_AMBER);

  int16_t cardH = valid ? FRAME_H : FRAME_H_WITH_NOTICE;

  drawWeatherCard(cardH);
  drawIndoorCard(cardH, valid);

  if (!valid) {
    char note[40];
    if (dhtState.lastValidClock[0] != ' ') {
      snprintf(note, sizeof(note), "ultima lettura %s, %u tentativi falliti",
               dhtState.lastValidClock,
               static_cast<unsigned>(dhtState.failureCount));
    } else {
      snprintf(note, sizeof(note), "mai letta, %u tentativi falliti",
               static_cast<unsigned>(dhtState.failureCount));
    }

    drawNotice("", note);
  }

  KeyBar bar = {{
    {"PAGINA",                       GLYPH_PAGE,    KEY_IDLE},
    {valid ? "AGGIORNA" : "RIPROVA", GLYPH_REFRESH, valid ? KEY_IDLE : KEY_PRIMARY},
    {"CITTA'",                       GLYPH_WIFI,    KEY_IDLE},
    {"CHIUDI",                       GLYPH_CROSS,   KEY_IDLE},
  }};
  drawKeyBar(bar);
}

// ---------------------------------------------------------------------------
// PAGINA SPOTIFY - schermate 06, 07 e 08
// ---------------------------------------------------------------------------
// Schermata a vuoto: un messaggio grande al centro della cornice e una riga di
// spiegazione sotto. La usano tutti i casi in cui non c'e' niente da suonare -
// player scollegato, login da fare, token scaduto - perche' dal punto di vista
// di chi guarda sono lo stesso stato: il display non ha una sorgente.
void drawSpotifyEmptyState(const char *headline, const char *detail,
                           uint16_t headlineColor) {
  // Il titolo prende la dimensione piu' grande in cui entra per intero. A
  // dimensione 4 nella cornice stanno dodici caratteri: "NESSUN BRANO" ci sta
  // esatto, "LOGIN RICHIESTO" no. Troncare una parola di due sillabe per
  // difendere un corpo di testo sarebbe il compromesso sbagliato.
  uint8_t size = 4;
  while (size > 1 && textLen(headline) > maxCharsForSize(size)) {
    size--;
  }

  int16_t blockH = textHeightPx(size) + 9 + textHeightPx(1);
  int16_t headlineY = CONTENT_Y + (CONTENT_H - blockH) / 2;

  drawCenteredLine(headline, headlineY, size, headlineColor);
  drawCenteredFittedLine(detail, headlineY + textHeightPx(size) + 9, 1,
                         COL_ACC_500, maxCharsForSize(1));
}

// Pagina del login: QR code dell'URL di autorizzazione e tre passi scritti
// per chi non sa niente del dispositivo. La riga in fondo e' lo stato del
// login, cosi' si vede che il dispositivo sta aspettando e non e' fermo.
void drawSpotifyLoginPage() {
  bool online = networkOnline();

  drawStatusBar("SPOTIFY - DA COLLEGARE", COL_AMBER,
                true, online ? signalBarsFromRssi(WiFi.RSSI()) : 0, !online,
                "", static_cast<uint8_t>(currentPage) + 1);

  int16_t qrSide = drawQrCode(spotifyLogin.url, LOGIN_QR_X, LOGIN_QR_Y, LOGIN_QR_MAX);

  // Senza QR (generatore assente nel core, o URL troppo lungo) resta la
  // strada del Serial Monitor, dove l'URL e' gia' stato stampato.
  if (qrSide == 0) {
    drawFrame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, COL_ACC_800);
    drawSpotifyEmptyState("LOGIN RICHIESTO",
                          "Apri l'URL stampato sul Serial Monitor", COL_AMBER);
  } else {
    int16_t textX = LOGIN_QR_X + qrSide + LOGIN_TEXT_GAP;
    int16_t textCols = (SCREEN_W - MARGIN_X - textX) / textWidthPx(1, 1);
    int16_t lineStep = textHeightPx(1) + 2;
    int16_t y = LOGIN_QR_Y + 4;

    drawLine("COLLEGA", textX, y, 2, COL_INK);
    y += textHeightPx(2) + 4;
    drawLine("SPOTIFY", textX, y, 2, COL_INK);
    y += textHeightPx(2) + 14;

    // Accanto a un QR da 183 px restano 17 caratteri per riga: ogni passo
    // deve stare in due righe, o la colonna invade la riga di stato.
    static const char *const steps[] = {
      "1 Inquadra il QR col telefono",
      "2 Accedi e premi Accetto",
      "3 Attendi qualche secondo",
    };

    for (const char *step : steps) {
      int16_t lines = drawWrappedLines(step, textX, y, 1, COL_ACC_400, textCols, 3);
      y += lines * lineStep + 8;
    }

    int16_t statusY = FOOTER_Y - 8 - 2 * lineStep;
    drawWrappedLines(spotifyState.statusMessage, textX, statusY, 1, COL_AMBER, textCols, 2);
  }

  KeyBar loginBar = {{
    {"PAGINA",    GLYPH_PAGE, KEY_IDLE},
    {"-",         GLYPH_NONE, KEY_DISABLED},
    {"CONFIGURA", GLYPH_WIFI, KEY_IDLE},
    {"-",         GLYPH_NONE, KEY_DISABLED},
  }};
  drawKeyBar(loginBar);
}

// Domanda di conferma prima di scollegare l'account: prende il posto di
// qualunque cosa la pagina stesse mostrando, finche' non si risponde o il
// tempo scade.
void drawSpotifyUnlinkConfirm() {
  drawPageStatusBar("SPOTIFY - SCOLLEGA ACCOUNT", COL_AMBER);
  drawFrame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, COL_ACC_800);
  drawSpotifyEmptyState("SCOLLEGARE?",
                        "Premi ancora K4 per dimenticare l'account", COL_AMBER);

  KeyBar confirmBar = {{
    {"PAGINA",   GLYPH_PAGE,  KEY_IDLE},
    {"-",        GLYPH_NONE,  KEY_DISABLED},
    {"ANNULLA",  GLYPH_RESET, KEY_IDLE},
    {"SCOLLEGA", GLYPH_CROSS, KEY_PRIMARY},
  }};
  drawKeyBar(confirmBar);
}

void drawSpotifyPage() {
  if (spotifyState.unlinkArmed) {
    drawSpotifyUnlinkConfirm();
    return;
  }

  if (spotifyState.awaitingLogin) {
    drawSpotifyLoginPage();
    return;
  }

  bool online = networkOnline();
  bool hasTrack = spotifyState.configured && spotifyState.authenticated &&
                  spotifyState.playbackActive;

  const char *label = "SPOTIFY - NON COLLEGATO";
  uint16_t labelColor = COL_ACC_600;

  if (hasTrack) {
    if (spotifyState.isPlaying) {
      label = "SPOTIFY - IN RIPRODUZIONE";
    } else {
      label = "SPOTIFY - IN PAUSA";
      labelColor = COL_AMBER;
    }
  } else if (spotifyState.appKeysMissing) {
    label = "SPOTIFY - DA CONFIGURARE";
    labelColor = COL_AMBER;
  }

  drawStatusBar(label, labelColor,
                true, online ? signalBarsFromRssi(WiFi.RSSI()) : 0, !online,
                "", static_cast<uint8_t>(currentPage) + 1);

  drawFrame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, COL_ACC_800);

  // --- app da configurare -------------------------------------------------
  // Nessun Client ID / Secret: prima del login serve l'app. Si configura
  // dal portale, che K3 apre al volo.
  if (spotifyState.appKeysMissing) {
    drawSpotifyEmptyState("CONFIGURA APP",
                          "Premi K3 e collegati a SpotifyThing-Setup", COL_AMBER);

    KeyBar setupBar = {{
      {"PAGINA",    GLYPH_PAGE, KEY_IDLE},
      {"-",         GLYPH_NONE, KEY_DISABLED},
      {"CONFIGURA", GLYPH_WIFI, KEY_PRIMARY},
      {"-",         GLYPH_NONE, KEY_DISABLED},
    }};
    drawKeyBar(setupBar);
    return;
  }

  // --- 08: nessuna sorgente ------------------------------------------------
  if (!hasTrack) {
    // `authenticated` dice che c'e' un account, non che la rete c'e': senza
    // WiFi la pagina deve dirlo, non fingere un player fermo.
    if (!online) {
      drawSpotifyEmptyState("NON COLLEGATO", "WiFi assente", COL_ACC_400);
    } else if (!spotifyState.configured || !spotifyState.authenticated) {
      drawSpotifyEmptyState("NON COLLEGATO",
                            spotifyState.statusMessage, COL_ACC_400);
    } else if (spotifyState.lastStatusCode != 0 &&
               (spotifyState.lastStatusCode < 200 || spotifyState.lastStatusCode >= 300)) {
      // Il token c'e' ma Spotify rifiuta la richiesta: e' un guasto, non un
      // player fermo, e il codice HTTP e' l'unico indizio per capire quale.
      drawSpotifyEmptyState("ERRORE SPOTIFY",
                            spotifyState.statusMessage, COL_AMBER);
    } else {
      drawSpotifyEmptyState("NESSUN BRANO",
                            "Avvia la riproduzione da Spotify sul telefono",
                            COL_ACC_400);
    }

    // Con un account collegato il quarto tasto lo scollega: e' la via
    // d'uscita quando il token e' stato revocato o l'account e' sbagliato.
    bool canUnlink = spotifyState.authenticated;

    KeyBar emptyBar = {{
      {"PAGINA",  GLYPH_PAGE,   KEY_IDLE},
      {"-",       GLYPH_NONE,   KEY_DISABLED},
      {"RICERCA", GLYPH_SEARCH, KEY_IDLE},
      {canUnlink ? "SCOLLEGA" : "-", canUnlink ? GLYPH_CROSS : GLYPH_NONE,
       canUnlink ? KEY_IDLE : KEY_DISABLED},
    }};
    drawKeyBar(emptyBar);
    return;
  }

  // --- 07: in pausa --------------------------------------------------------
  // Il brano resta caricato ma il design toglie di mezzo i metadati e mette al
  // centro lo stato, perche' con il player fermo l'informazione utile e' una
  // sola: e' in pausa, e il tasto per ripartire e' il terzo.
  if (!spotifyState.isPlaying) {
    int16_t blockH = 26 + 12 + textHeightPx(3) + 9 + textHeightPx(1);
    int16_t glyphY = CONTENT_Y + (CONTENT_H - blockH) / 2;

    // Pittogramma di pausa ingrandito: due barre da 10x26.
    tft.fillRect(SCREEN_W / 2 - 13, glyphY, 10, 26, COL_AMBER);
    tft.fillRect(SCREEN_W / 2 + 3, glyphY, 10, 26, COL_AMBER);

    int16_t textY = glyphY + 26 + 12;
    drawCenteredLine("IN PAUSA", textY, 3, COL_AMBER);
    drawCenteredFittedLine(spotifyState.trackName, textY + textHeightPx(3) + 9, 1,
                           COL_ACC_500, maxCharsForSize(1));

    KeyBar pausedBar = {{
      {"PAGINA",   GLYPH_PAGE, KEY_IDLE},
      {"PREC.",    GLYPH_PREV, KEY_IDLE},
      {"RIPRENDI", GLYPH_PLAY, KEY_PRIMARY},
      {"SUCC.",    GLYPH_NEXT, KEY_IDLE},
    }};
    drawKeyBar(pausedBar);
    return;
  }

  // --- 06: in riproduzione -------------------------------------------------
  // Copertina a sinistra, metadati a destra. Il riquadro viene disegnato anche
  // quando l'immagine non c'e': se comparisse e sparisse, la pagina si
  // ricomporrebbe sotto gli occhi a ogni cambio di brano.
  artRender(ART_X, ART_Y);

  int16_t textX = ART_TEXT_X;
  int16_t artistCols = ART_TEXT_W / textWidthPx(1, 1);

  drawFittedLine(spotifyState.artistName, textX, ART_Y, 1, COL_ACC_500, artistCols);

  drawWrappedLines(spotifyState.trackName, textX, ART_Y + textHeightPx(1) + 6,
                   SPOT_TITLE_SIZE, COL_INK, SPOT_TITLE_COLS, SPOT_TITLE_LINES);

  // Riga di avanzamento: resta a tutta larghezza sotto la copertina, perche' e'
  // l'unica cosa della pagina che cambia da sola e si legge meglio larga.
  int16_t progressY = CONTENT_Y + CONTENT_H - 12;

  char elapsed[12];
  char total[12];
  formatTrackTime(elapsed, sizeof(elapsed), spotifyState.progressMs);
  formatTrackTime(total, sizeof(total), spotifyState.durationMs);

  drawLine(elapsed, FRAME_X + SPOT_PAD_X, progressY, 1, COL_ACC_600);

  int16_t barX = FRAME_X + SPOT_PAD_X + textWidthPx(1, textLen(elapsed)) + 6;
  int16_t barRight = FRAME_X + FRAME_W - SPOT_PAD_X - textWidthPx(1, textLen(total)) - 6;
  float ratio = spotifyState.durationMs > 0
                  ? static_cast<float>(spotifyState.progressMs) /
                    static_cast<float>(spotifyState.durationMs)
                  : 0.0f;

  drawThinBar(barX, progressY + 2, barRight - barX, ratio, COL_ACC_400, COL_TRACK);
  drawLine(total, barRight + 6, progressY, 1, COL_ACC_600);

  KeyBar playingBar = {{
    {"PAGINA", GLYPH_PAGE,  KEY_IDLE},
    {"PREC.",  GLYPH_PREV,  KEY_IDLE},
    {"PAUSA",  GLYPH_PAUSE, KEY_ACTIVE},
    {"SUCC.",  GLYPH_NEXT,  KEY_IDLE},
  }};
  drawKeyBar(playingBar);
}

// ---------------------------------------------------------------------------
// PAGINA POMODORO - schermate 09, 10 e 11
// ---------------------------------------------------------------------------
// Tre stati distinti, non due: fermo a inizio fase (pronto), in corso, e
// concluso in attesa che la pausa venga avviata. E' la differenza fra "il
// timer non sta girando" e "il pomodoro e' finito", che il design tiene
// separate perche' chiedono azioni diverse.
bool pomodoroIsCompleted() {
  return pomodoroState.isBreak && !pomodoroState.running &&
         pomodoroState.remainingMs == pomodoroState.phaseDurationMs;
}

// Come per l'orologio: si confrontano le cinque celle di "MM:SS" con quelle
// gia' a schermo. La barra avanza dipingendo la sola striscia guadagnata.
void renderPomodoroFields() {
  if (pomodoroIsCompleted()) {
    return;
  }

  bool forceAll = !pomodoroCacheValid;

  unsigned long remainingSeconds = pomodoroRemainingSeconds();
  char timerBuffer[POMO_TIMER_LEN + 1];
  snprintf(timerBuffer, sizeof(timerBuffer), "%02lu:%02lu",
           remainingSeconds / 60, remainingSeconds % 60);

  int16_t blockH = textHeightPx(8) + 12 + 3 + 13 + 7;
  int16_t timerY = CONTENT_Y + (CONTENT_H - blockH) / 2;

  drawCachedText(timerBuffer, pomodoroTimerCache, POMO_TIMER_LEN,
                 POMO_TIMER_X, timerY, 8, COL_INK, COL_BG, forceAll);

  if (pomodoroState.running) {
    float elapsedRatio = 0.0f;
    if (pomodoroState.phaseDurationMs > 0) {
      elapsedRatio = 1.0f - static_cast<float>(pomodoroState.remainingMs) /
                            static_cast<float>(pomodoroState.phaseDurationMs);
    }

    drawProgressBar(POMO_BAR_X, timerY + textHeightPx(8) + 12, POMO_BAR_W,
                    elapsedRatio, COL_AMBER, COL_TRACK_WARM,
                    pomodoroBarFilledPx, forceAll);
  }

  pomodoroCacheValid = true;
}

void drawPomodoroPage() {
  bool completed = pomodoroIsCompleted();
  bool running = pomodoroState.running;

  const char *label;
  uint16_t labelColor = COL_AMBER;

  if (completed) {
    label = "POMODORO COMPLETATO";
  } else if (running) {
    label = pomodoroState.isBreak ? "PAUSA IN CORSO" : "FOCUS IN CORSO";
  } else {
    label = pomodoroState.isBreak ? "PAUSA - IN ATTESA" : "POMODORO - PRONTO";
    labelColor = COL_ACC_600;
  }

  drawPageStatusBar(label, labelColor);

  // --- 11: pomodoro concluso, pausa da avviare -----------------------------
  if (completed) {
    drawFilledFrame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, COL_DONE_BG, COL_DONE_EDGE);

    int16_t blockH = textHeightPx(3) + 9 + textHeightPx(1);
    int16_t titleY = CONTENT_Y + (CONTENT_H - blockH) / 2;

    char pauseText[16];
    snprintf(pauseText, sizeof(pauseText), "PAUSA %lu:00",
             pomodoroState.phaseDurationMs / 60000UL);

    char doneText[48];
    snprintf(doneText, sizeof(doneText), "%uo pomodoro fatto - alzati dalla sedia",
             static_cast<unsigned>(pomodoroState.completedWorkSessions));

    drawCenteredLine(pauseText, titleY, 3, COL_AMBER);
    drawCenteredFittedLine(doneText, titleY + textHeightPx(3) + 9, 1,
                           COL_ACC_500, maxCharsForSize(1));

    KeyBar doneBar = {{
      {"PAGINA",      GLYPH_PAGE,  KEY_IDLE},
      {"SALTA",       GLYPH_SKIP,  KEY_IDLE},
      {"AVVIA PAUSA", GLYPH_PLAY,  KEY_PRIMARY},
      {"RESET",       GLYPH_RESET, KEY_IDLE},
    }};
    drawKeyBar(doneBar);
    return;
  }

  // --- 09 e 10: timer fermo o in corso -------------------------------------
  drawFrame(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, COL_ACC_800);

  pomodoroCacheValid = false;
  pomodoroBarFilledPx = 0;
  renderPomodoroFields();

  // Contatore dei pomodori: l'etichetta e i quattro quadratini, centrati
  // insieme come un blocco solo. Con il timer in corso si aggiunge il totale.
  int16_t blockH = textHeightPx(8) + 12 + 3 + 13 + 7;
  int16_t timerY = CONTENT_Y + (CONTENT_H - blockH) / 2;
  int16_t pipsRowY = timerY + textHeightPx(8) + 12 + (running ? 3 + 13 : 13);

  static const char counterLabel[] = "POMODORI";
  char counterTotal[8];
  counterTotal[0] = '\0';
  if (running) {
    snprintf(counterTotal, sizeof(counterTotal), "%u",
             static_cast<unsigned>(pomodoroState.completedWorkSessions));
  }

  int16_t labelW = textWidthPx(1, textLen(counterLabel));
  int16_t rowW = labelW + 7 + POMO_PIPS_W;
  if (textLen(counterTotal) > 0) {
    rowW += 7 + textWidthPx(1, textLen(counterTotal));
  }

  int16_t rowX = (SCREEN_W - rowW) / 2;
  drawLine(counterLabel, rowX, pipsRowY, 1, COL_ACC_600);

  int16_t pipsX = rowX + labelW + 7;
  drawPomodoroPips(pipsX, pipsRowY - 1, pomodoroState.completedWorkSessions);

  if (textLen(counterTotal) > 0) {
    drawLine(counterTotal, pipsX + POMO_PIPS_W + 7, pipsRowY, 1, COL_ACC_500);
  }

  KeyBar bar = {{
    {"PAGINA", GLYPH_PAGE, KEY_IDLE},
    {"CHIUDI", GLYPH_CROSS, KEY_IDLE},
    {running ? "SOSPENDI" : "AVVIA", running ? GLYPH_PAUSE : GLYPH_PLAY, KEY_PRIMARY},
    {"RESET",  GLYPH_RESET, KEY_IDLE},
  }};
  drawKeyBar(bar);
}

// ---------------------------------------------------------------------------
// REGISTRO DELLE SCHERMATE
// ---------------------------------------------------------------------------
// Il footer non sta piu' qui: le etichette dei tasti cambiano con lo stato
// della pagina, quindi le disegna la funzione di draw insieme al resto.
const ScreenView screenViews[PAGE_COUNT] = {
  {"OROLOGIO", drawClockPage,    handleClockButtonPress},
  {"DHT22",    drawDhtPage,      handleDhtButtonPress},
  {"SPOTIFY",  drawSpotifyPage,  handleSpotifyButtonPress},
  {"POMODORO", drawPomodoroPage, handlePomodoroButtonPress},
};

const ScreenView &currentScreenView() {
  return screenViews[currentPage];
}

// ---------------------------------------------------------------------------
// RENDERING
// ---------------------------------------------------------------------------
// Ridisegno completo: si usa solo quando cambia la pagina o quando cambiano
// elementi statici (stato del timer, dati Spotify, lettura del sensore).
void renderPage() {
  const ScreenView &screen = currentScreenView();

  clearScreen();
  invalidateScreenCaches();

  if (screen.draw != nullptr) {
    screen.draw();
  }

  displayDirty = false;
}

// Chiamate a ogni giro di loop. Non disegnano nulla finche' il valore da
// mostrare non cambia, quindi possono essere invocate liberamente: il costo a
// vuoto e' una manciata di confronti fra caratteri.
void renderClockTick() {
  if (currentPage != PAGE_CLOCK || displayDirty) {
    return;
  }

  renderClockFields();
}

void renderPomodoroTick() {
  if (currentPage != PAGE_POMODORO || displayDirty) {
    return;
  }

  renderPomodoroFields();
}
