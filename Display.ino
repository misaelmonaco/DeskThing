// ===========================================================================
// DISPLAY - inizializzazione del pannello e primitive di disegno
// ---------------------------------------------------------------------------
// Qui vive tutto cio' che sa com'e' fatto il display. Le schermate in
// ScreenViews.ino usano solo queste funzioni e le costanti di Theme.h, cosi'
// un eventuale cambio di pannello si risolve toccando Config.h e questo file.
//
// Le primitive ricalcano i tre elementi ricorrenti del design: la barra di
// stato, la cornice con le crocette agli angoli e la barra dei quattro tasti.
// Nessuna pagina disegna un bordo o una cella a mano.
// ===========================================================================

// ---------------------------------------------------------------------------
// INIZIALIZZAZIONE
// ---------------------------------------------------------------------------
// L'ST7789 e' di sola scrittura: il bus HSPI viene aperto senza pin MISO
// (secondo argomento a -1) in modo da lasciare libero il GPIO 12.
void setupDisplay() {
  hspi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  // La init vuole sempre la risoluzione nativa del pannello, non quella
  // dell'orientamento in uso: la rotazione si applica subito dopo.
  tft.init(TFT_WIDTH_PX, TFT_HEIGHT_PX);
  tft.setSPISpeed(TFT_SPI_HZ);

  // La init della libreria lascia il pannello con l'inversione attiva: su
  // questi moduli i colori tornano corretti solo disattivandola.
  tft.invertDisplay(TFT_COLOR_INVERSION);

  // Rotation 1 = orizzontale, 320 px di larghezza per 240 di altezza.
  tft.setRotation(TFT_ROTATION);

  tft.setTextWrap(false);
  clearScreen();
}

// ---------------------------------------------------------------------------
// PULIZIA DELLE AREE
// ---------------------------------------------------------------------------
void clearScreen() {
  tft.fillScreen(COL_BG);
}

// Cancella una banda orizzontale a tutta larghezza dello schermo.
void clearContentBand(int16_t y, int16_t height) {
  tft.fillRect(0, y, SCREEN_W, height, COL_BG);
}

// ---------------------------------------------------------------------------
// TESTO
// ---------------------------------------------------------------------------
// Tutto il disegno lavora su `const char *`, non su String.
//
// Non e' pignoleria stilistica. Ogni String costruita o concatenata e' una
// malloc e una free sull'heap, e un ridisegno di pagina ne faceva una dozzina.
// Per giorni non si nota niente; poi l'handshake TLS verso Spotify chiede
// trenta o quaranta kilobyte contigui, non li trova piu' in un heap ridotto a
// coriandoli, e il player smette di funzionare sembrando un problema di rete.
// I percorsi di disegno adesso non allocano nulla: i testi variabili vengono
// formattati con snprintf in buffer locali sullo stack, che muore con la
// funzione e non frammenta niente.
int16_t textLen(const char *text) {
  return text != nullptr ? static_cast<int16_t>(strlen(text)) : 0;
}

// Scrive una riga su sfondo gia' pulito.
void drawLine(const char *text, int16_t x, int16_t y, uint8_t size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(text);
}

// Scrive una riga centrata orizzontalmente sullo schermo.
void drawCenteredLine(const char *text, int16_t y, uint8_t size, uint16_t color) {
  drawLine(text, centeredX(size, textLen(text)), y, size, color);
}

// Scrive una riga centrata dentro una colonna arbitraria: serve alle pagine
// divise in due riquadri affiancati, dove il centro non e' quello dello schermo.
void drawCenteredIn(const char *text, int16_t x, int16_t width,
                    int16_t y, uint8_t size, uint16_t color) {
  int16_t textX = x + (width - textWidthPx(size, textLen(text))) / 2;
  drawLine(text, textX, y, size, color);
}

// Scrive una riga troncandola a maxChars con i puntini di sospensione.
// Il vecchio fitLine restituiva una String nuova per farlo; qui i caratteri
// da tenere si scrivono uno per uno e quelli di troppo non vengono copiati
// da nessuna parte.
void drawFittedLine(const char *text, int16_t x, int16_t y, uint8_t size,
                    uint16_t color, int16_t maxChars) {
  if (maxChars <= 0) {
    return;
  }

  if (textLen(text) <= maxChars) {
    drawLine(text, x, y, size, color);
    return;
  }

  bool withEllipsis = (maxChars > 3);
  int16_t keep = withEllipsis ? maxChars - 3 : maxChars;

  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);

  for (int16_t i = 0; i < keep; ++i) {
    tft.write(static_cast<uint8_t>(text[i]));
  }
  if (withEllipsis) {
    tft.print("...");
  }
}

// Quanti caratteri di `text` finiranno effettivamente a schermo con un limite
// di maxChars. Serve a centrare una riga troncata: va centrata su quello che
// si vede, non sulla lunghezza che avrebbe avuto.
int16_t shownChars(const char *text, int16_t maxChars) {
  int16_t len = textLen(text);
  return (len <= maxChars) ? len : maxChars;
}

void drawCenteredFittedLine(const char *text, int16_t y, uint8_t size,
                            uint16_t color, int16_t maxChars) {
  drawFittedLine(text, centeredX(size, shownChars(text, maxChars)), y,
                 size, color, maxChars);
}

// ---------------------------------------------------------------------------
// TESTO A CELLE CON CACHE - il cuore dell'aggiornamento parziale
// ---------------------------------------------------------------------------
// Il font integrato e' a larghezza fissa: ogni carattere occupa esattamente
// GFX_CELL_W * size pixel. Questo permette di trattare una stringa come una
// fila di celle indipendenti e di ridipingere solo quelle il cui carattere e'
// cambiato rispetto all'ultimo disegno.
//
// Sull'orologio "21:04" a dimensione 8 significa ridisegnare 48x64 pixel al
// minuto invece di 240x64, e sui secondi a dimensione 3 appena 18x24 pixel al
// secondo: le cifre piu' significative restano ferme per ore.
//
// `cache` deve essere un buffer di almeno `length` caratteri, persistente fra
// una chiamata e l'altra. Con `forceAll` si ridisegnano tutte le celle: va
// usato dopo un ridisegno completo della pagina, quando cio' che e' a schermo
// non corrisponde piu' al contenuto della cache.
//
// Adafruit_GFX::drawChar, quando il colore di sfondo e' diverso da quello del
// testo, ridipinge l'intera cella 6x8 (colonna di spaziatura compresa): non
// restano residui del carattere precedente e non serve una fillRect a parte.
void drawCachedText(const char *text, char *cache, uint8_t length,
                    int16_t x, int16_t y, uint8_t size,
                    uint16_t color, uint16_t background, bool forceAll) {
  int16_t cellWidth = GFX_CELL_W * size;

  tft.setTextSize(size);
  tft.setTextColor(color, background);

  for (uint8_t i = 0; i < length; ++i) {
    if (!forceAll && cache[i] == text[i]) {
      continue;
    }

    tft.setCursor(x + i * cellWidth, y);
    tft.write(static_cast<uint8_t>(text[i]));
    cache[i] = text[i];
  }
}

// Manda a capo il testo sulle parole per un massimo di maxLines righe; se il
// testo eccede, l'ultima riga viene troncata con i puntini di sospensione.
// Restituisce il numero di righe effettivamente disegnate.
int16_t drawWrappedLines(const char *text, int16_t x, int16_t y, uint8_t size,
                         uint16_t color, int16_t maxChars, int16_t maxLines) {
  if (maxChars <= 0 || maxLines <= 0 || text == nullptr) {
    return 0;
  }

  // Si avanza con un puntatore dentro la stringa originale. La versione
  // precedente tagliava con substring(), e ogni taglio era una String nuova:
  // un titolo su tre righe costava sei allocazioni.
  const char *cursor = text;
  int16_t drawn = 0;
  int16_t lineHeight = textHeightPx(size) + 2 * size;

  while (*cursor != '\0' && drawn < maxLines) {
    int16_t lineY = y + drawn * lineHeight;

    if (textLen(cursor) <= maxChars) {
      drawLine(cursor, x, lineY, size, color);
      return drawn + 1;
    }

    if (drawn == maxLines - 1) {
      drawFittedLine(cursor, x, lineY, size, color, maxChars);
      return drawn + 1;
    }

    // Ultimo spazio entro il limite, per non spezzare le parole. L'indice
    // maxChars si puo' leggere: sappiamo che la stringa e' piu' lunga.
    int16_t cut = -1;
    for (int16_t i = 0; i <= maxChars; ++i) {
      if (cursor[i] == ' ') {
        cut = i;
      }
    }
    if (cut <= 0) {
      cut = maxChars;
    }

    tft.setTextSize(size);
    tft.setTextColor(color);
    tft.setCursor(x, lineY);
    for (int16_t i = 0; i < cut; ++i) {
      tft.write(static_cast<uint8_t>(cursor[i]));
    }

    cursor += cut;
    while (*cursor == ' ') {
      ++cursor;
    }
    drawn++;
  }

  return drawn;
}

// ---------------------------------------------------------------------------
// CORNICE DEL CONTENUTO
// ---------------------------------------------------------------------------
// Il rettangolo che contiene la pagina, con le quattro crocette appoggiate
// appena fuori dagli angoli. Sono il tratto piu' riconoscibile del design:
// vengono disegnate a mano come due segmenti incrociati di 5 px perche' il
// carattere "+" del font integrato risulterebbe troppo grande e disallineato.
void drawCornerMark(int16_t cx, int16_t cy, uint16_t color) {
  tft.drawFastHLine(cx - 2, cy, 5, color);
  tft.drawFastVLine(cx, cy - 2, 5, color);
}

void drawFrame(int16_t x, int16_t y, int16_t width, int16_t height, uint16_t color) {
  tft.drawRect(x, y, width, height, color);

  drawCornerMark(x, y, COL_ACC_700);
  drawCornerMark(x + width - 1, y, COL_ACC_700);
  drawCornerMark(x, y + height - 1, COL_ACC_700);
  drawCornerMark(x + width - 1, y + height - 1, COL_ACC_700);
}

// Cornice riempita di un fondo proprio: la usa il Pomodoro concluso, che nel
// design ha il riquadro caldo invece del nero di pagina.
void drawFilledFrame(int16_t x, int16_t y, int16_t width, int16_t height,
                     uint16_t fill, uint16_t border) {
  tft.fillRect(x, y, width, height, fill);
  drawFrame(x, y, width, height, border);
}

// ---------------------------------------------------------------------------
// BARRA DI STATO
// ---------------------------------------------------------------------------
// Tre tacche crescenti di 3 px, come nel design. Con `lost` a true restano
// tutte spente e viene tirata sopra la barretta obliqua dell'assenza di rete.
void drawSignalBars(int16_t x, int16_t baselineY, int8_t bars, bool lost) {
  const int8_t heights[3] = {4, 6, 9};

  for (int8_t i = 0; i < 3; ++i) {
    uint16_t color;
    if (lost) {
      color = COL_SIGNAL_OFF;
    } else {
      color = (i < bars) ? COL_ACC_400 : COL_ACC_800;
    }
    tft.fillRect(x + i * 5, baselineY - heights[i], 3, heights[i], color);
  }

  if (lost) {
    // Sbarra diagonale sulle tacche: due pixel di spessore per restare
    // leggibile senza antialiasing.
    tft.drawLine(x - 1, baselineY - 1, x + 13, baselineY - 10, COL_AMBER);
    tft.drawLine(x - 1, baselineY - 2, x + 13, baselineY - 11, COL_AMBER);
  }
}

// Converte l'RSSI in tacche piene. Le soglie sono quelle abituali del WiFi
// domestico: sopra -60 dBm il segnale e' pieno, sotto -80 e' al minimo.
int8_t signalBarsFromRssi(int32_t rssi) {
  if (rssi >= -60) {
    return 3;
  }
  if (rssi >= -70) {
    return 2;
  }
  if (rssi >= -80) {
    return 1;
  }
  return 0;
}

// La riga in cima: etichetta di sezione a sinistra, e a destra - quando
// servono - le tacche del segnale, l'ora corrente e l'indicatore di pagina.
// `clockText` vuoto sopprime l'orologio, `pageIndex` a 0 l'indicatore.
void drawStatusBar(const char *label, uint16_t labelColor,
                   bool showSignal, int8_t signalBars, bool signalLost,
                   const char *clockText, uint8_t pageIndex) {
  clearContentBand(0, STATUS_Y + STATUS_H);

  drawFittedLine(label, MARGIN_X, STATUS_Y, 1, labelColor, maxCharsForSize(1));

  int16_t cursorX = SCREEN_W - MARGIN_X;

  if (pageIndex > 0) {
    char indicator[8];
    snprintf(indicator, sizeof(indicator), "%u/%u",
             static_cast<unsigned>(pageIndex), static_cast<unsigned>(PAGE_COUNT));
    cursorX -= textWidthPx(1, textLen(indicator));
    drawLine(indicator, cursorX, STATUS_Y + 1, 1, COL_ACC_600);
    cursorX -= 8;
  }

  if (textLen(clockText) > 0) {
    cursorX -= textWidthPx(1, textLen(clockText));
    drawLine(clockText, cursorX, STATUS_Y + 1, 1, COL_ACC_700);
    cursorX -= 8;
  }

  if (showSignal) {
    cursorX -= 13;
    drawSignalBars(cursorX, STATUS_Y + STATUS_H - 1, signalBars, signalLost);
  }
}

// ---------------------------------------------------------------------------
// FASCIA DI AVVISO
// ---------------------------------------------------------------------------
// Rettangolo scuro caldo con il filetto ambra a sinistra: rete caduta, sensore
// che non risponde, conferma della cancellazione della rete. Il titolo e'
// facoltativo, il dettaglio lo segue sulla stessa riga.
void drawNotice(const char *title, const char *detail) {
  tft.fillRect(MARGIN_X, NOTICE_Y, SCREEN_W - 2 * MARGIN_X, NOTICE_H, COL_WARN_BG);
  tft.fillRect(MARGIN_X, NOTICE_Y, 3, NOTICE_H, COL_AMBER);

  int16_t textY = centeredY(NOTICE_Y, NOTICE_H, 1);
  int16_t textX = MARGIN_X + 11;
  bool hasTitle = (textLen(title) > 0);

  if (hasTitle) {
    drawLine(title, textX, textY, 1, COL_AMBER);
    textX += textWidthPx(1, textLen(title) + 2);
  }

  if (textLen(detail) > 0) {
    int16_t available = (SCREEN_W - MARGIN_X - 6 - textX) / GFX_CELL_W;
    drawFittedLine(detail, textX, textY, 1,
                   hasTitle ? COL_ACC_500 : COL_AMBER, available);
  }
}

// ---------------------------------------------------------------------------
// BARRE DI AVANZAMENTO
// ---------------------------------------------------------------------------
// Barra piatta da 3 px, senza cornice: nel design e' una riga sottile di
// fondo su cui scorre il riempimento.
void drawThinBar(int16_t x, int16_t y, int16_t width, float ratio,
                 uint16_t fillColor, uint16_t trackColor) {
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }

  tft.fillRect(x, y, width, 3, trackColor);
  int16_t filled = static_cast<int16_t>(width * ratio);
  if (filled > 0) {
    tft.fillRect(x, y, filled, 3, fillColor);
  }
}

// Versione incrementale della barra sottile: nel caso normale dipinge solo la
// striscia di pixel guadagnata dall'ultimo aggiornamento, non l'intera barra.
//
// `filledPx` e' lo stato: va passata sempre la stessa variabile, e la funzione
// la aggiorna. Si ridisegna tutto solo quando la barra arretra (reset o cambio
// di fase) o quando `forceAll` lo impone.
void drawProgressBar(int16_t x, int16_t y, int16_t width, float ratio,
                     uint16_t fillColor, uint16_t trackColor,
                     int16_t &filledPx, bool forceAll) {
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }

  int16_t target = static_cast<int16_t>(width * ratio);

  if (forceAll || target < filledPx) {
    drawThinBar(x, y, width, ratio, fillColor, trackColor);
    filledPx = target;
    return;
  }

  if (target > filledPx) {
    tft.fillRect(x + filledPx, y, target - filledPx, 3, fillColor);
    filledPx = target;
  }
}

// ---------------------------------------------------------------------------
// CONTATORE DEI POMODORI
// ---------------------------------------------------------------------------
// Quattro quadratini da 7 px: pieni ambra quelli fatti, a solo contorno quelli
// che restano. Il ciclo e' di quattro, come nel disegno.
void drawPomodoroPips(int16_t x, int16_t y, uint16_t completed) {
  uint16_t inCycle = completed % 4;
  bool cycleFull = (completed > 0 && inCycle == 0);

  for (int8_t i = 0; i < 4; ++i) {
    int16_t pipX = x + i * 10;
    bool filled = cycleFull || (i < static_cast<int8_t>(inCycle));

    if (filled) {
      tft.fillRect(pipX, y, 7, 7, COL_AMBER);
    } else {
      tft.drawRect(pipX, y, 7, 7, COL_ACC_700);
    }
  }
}

// ---------------------------------------------------------------------------
// BARRA DEI TASTI
// ---------------------------------------------------------------------------
// I pittogrammi del design sono SVG a 13x13: qui sono ridisegnati a mano con
// primitive di GFX dentro una cella della stessa dimensione, perche' bitmap
// pregenerate costerebbero flash senza aggiungere nulla a questa scala.
void drawKeyGlyph(KeyGlyph glyph, int16_t x, int16_t y, uint16_t color) {
  switch (glyph) {
    case GLYPH_PAGE:
      tft.drawRect(x, y + 1, 7, 10, color);
      tft.drawRect(x + 5, y + 3, 8, 10, color);
      break;

    case GLYPH_CLOCK:
      tft.drawCircle(x + 6, y + 6, 6, color);
      tft.drawFastVLine(x + 6, y + 3, 4, color);
      tft.drawFastHLine(x + 6, y + 6, 3, color);
      break;

    case GLYPH_WIFI:
      tft.drawFastHLine(x + 1, y + 3, 11, color);
      tft.drawPixel(x, y + 4, color);
      tft.drawPixel(x + 12, y + 4, color);
      tft.drawFastHLine(x + 3, y + 7, 7, color);
      tft.drawPixel(x + 2, y + 8, color);
      tft.drawPixel(x + 10, y + 8, color);
      tft.fillRect(x + 5, y + 10, 3, 3, color);
      break;

    case GLYPH_CROSS:
      tft.drawLine(x + 1, y + 1, x + 11, y + 11, color);
      tft.drawLine(x + 11, y + 1, x + 1, y + 11, color);
      break;

    case GLYPH_REFRESH:
      tft.drawCircle(x + 6, y + 6, 5, color);
      // Morso in alto a destra piu' punta della freccia: l'anello aperto e'
      // cio' che rende il simbolo leggibile come "ricarica".
      tft.fillRect(x + 7, y, 6, 4, COL_BG);
      tft.drawFastHLine(x + 7, y + 2, 5, color);
      tft.drawLine(x + 9, y, x + 12, y + 2, color);
      tft.drawLine(x + 9, y + 4, x + 12, y + 2, color);
      break;

    case GLYPH_CHART:
      tft.drawLine(x, y + 11, x + 4, y + 5, color);
      tft.drawLine(x + 4, y + 5, x + 7, y + 8, color);
      tft.drawLine(x + 7, y + 8, x + 12, y + 1, color);
      break;

    case GLYPH_PREV:
      tft.fillRect(x, y + 2, 2, 9, color);
      tft.fillTriangle(x + 12, y + 2, x + 12, y + 10, x + 4, y + 6, color);
      break;

    case GLYPH_NEXT:
      tft.fillRect(x + 11, y + 2, 2, 9, color);
      tft.fillTriangle(x, y + 2, x, y + 10, x + 8, y + 6, color);
      break;

    case GLYPH_PLAY:
      tft.fillTriangle(x + 2, y + 1, x + 2, y + 11, x + 11, y + 6, color);
      break;

    case GLYPH_PAUSE:
      tft.fillRect(x + 2, y + 1, 3, 11, color);
      tft.fillRect(x + 8, y + 1, 3, 11, color);
      break;

    case GLYPH_SEARCH:
      tft.drawCircle(x + 5, y + 5, 4, color);
      tft.drawLine(x + 8, y + 8, x + 12, y + 12, color);
      break;

    case GLYPH_RESET:
      tft.drawCircle(x + 6, y + 6, 5, color);
      tft.fillRect(x, y, 6, 4, COL_BG);
      tft.drawFastHLine(x + 1, y + 2, 5, color);
      tft.drawLine(x + 3, y, x, y + 2, color);
      tft.drawLine(x + 3, y + 4, x, y + 2, color);
      break;

    case GLYPH_SKIP:
      tft.fillTriangle(x, y + 2, x, y + 10, x + 6, y + 6, color);
      tft.fillTriangle(x + 5, y + 2, x + 5, y + 10, x + 11, y + 6, color);
      tft.fillRect(x + 11, y + 2, 2, 9, color);
      break;

    case GLYPH_NONE:
    default:
      break;
  }
}

// Colore del contenuto di una cella in base al suo stato.
uint16_t keyForeground(KeyStyle style) {
  switch (style) {
    case KEY_PRIMARY:  return COL_AMBER;
    case KEY_ACTIVE:   return COL_INK;
    case KEY_DISABLED: return COL_ACC_700;
    case KEY_IDLE:
    default:           return COL_ACC_400;
  }
}

uint16_t keyBackground(KeyStyle style) {
  switch (style) {
    case KEY_PRIMARY: return COL_KEY_AMBER;
    case KEY_ACTIVE:  return COL_KEY_BLUE;
    default:          return COL_BG;
  }
}

// Disegna l'intera barra dei tasti: riga di separazione, quattro celle di
// uguale larghezza, divisori verticali fra l'una e l'altra.
void drawKeyBar(const KeyBar &bar) {
  clearContentBand(FOOTER_Y, FOOTER_H);
  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_ACC_800);

  for (int16_t i = 0; i < KEY_COUNT; ++i) {
    const KeyCell &key = bar.keys[i];
    int16_t cellX = i * KEY_W;
    int16_t cellW = (i == KEY_COUNT - 1) ? (SCREEN_W - cellX) : KEY_W;

    uint16_t background = keyBackground(key.style);
    if (background != COL_BG) {
      tft.fillRect(cellX + 1, FOOTER_Y + 1, cellW - 1, FOOTER_H - 1, background);
    }

    if (i > 0) {
      tft.drawFastVLine(cellX, FOOTER_Y + 1, FOOTER_H - 1, COL_ACC_800);
    }

    uint16_t foreground = keyForeground(key.style);

    // Una cella senza pittogramma (tasto inattivo) centra la sola etichetta;
    // con il pittogramma, questo sta sopra e l'etichetta sotto.
    if (key.glyph == GLYPH_NONE) {
      drawCenteredIn(key.label, cellX, cellW,
                     centeredY(FOOTER_Y, FOOTER_H, 1), 1, foreground);
      continue;
    }

    drawKeyGlyph(key.glyph, cellX + (cellW - 13) / 2, FOOTER_Y + 7, foreground);
    drawCenteredIn(key.label, cellX, cellW, FOOTER_Y + 23, 1, foreground);
  }
}

// ---------------------------------------------------------------------------
// SCHERMATA DI AVVIO
// ---------------------------------------------------------------------------
// La prima della sequenza: titolo, versione, elenco dei sottosistemi e barra
// di avanzamento. Vive fuori dalla cornice header/footer perche' all'accensione
// non esiste ancora una pagina corrente, ed e' usata anche dal portale WiFi.
// La cornice dell'avvio sale piu' in alto delle altre: non c'e' la barra di
// stato, quindi si prende anche quello spazio. In basso si ferma dove si
// fermano tutte, sei pixel sopra la riga dei tasti.
constexpr int16_t BOOT_FRAME_Y = FRAME_Y - 14;
constexpr int16_t BOOT_FRAME_H = (FOOTER_Y - 6) - BOOT_FRAME_Y;
constexpr int16_t BOOT_TEXT_X  = FRAME_X + 16;
constexpr int16_t BOOT_TITLE_Y = FRAME_Y + 24;
constexpr int16_t BOOT_LIST_Y  = BOOT_TITLE_Y + textHeightPx(4) + 32;
constexpr int16_t BOOT_BAR_Y   = BOOT_LIST_Y + 44;
constexpr int16_t BOOT_BAR_W   = FRAME_W - 32;

void drawBootScreen(const char *statusLabel, const char *statusDetail,
                    bool statusPending, float progress, const char *footerNote) {
  clearScreen();

  drawFrame(FRAME_X, BOOT_FRAME_Y, FRAME_W, BOOT_FRAME_H, COL_ACC_800);

  drawLine("DESK CONSOLE", BOOT_TEXT_X, BOOT_TITLE_Y, 4, COL_INK);
  drawLine("ESP32 - ST7789 - v1.0", BOOT_TEXT_X,
           BOOT_TITLE_Y + textHeightPx(4) + 9, 1, COL_ACC_500);

  drawLine("[ok]", BOOT_TEXT_X, BOOT_LIST_Y, 1, COL_ACC_400);
  drawLine("display - sensore - pulsanti", BOOT_TEXT_X + textWidthPx(1, 6),
           BOOT_LIST_Y, 1, COL_ACC_500);

  drawLine(statusPending ? "[..]" : "[ok]", BOOT_TEXT_X, BOOT_LIST_Y + 13, 1,
           statusPending ? COL_AMBER : COL_ACC_400);
  drawFittedLine(statusLabel, BOOT_TEXT_X + textWidthPx(1, 6), BOOT_LIST_Y + 13, 1,
                 COL_ACC_300, maxCharsForSize(1) - 6);

  if (textLen(statusDetail) > 0) {
    drawFittedLine(statusDetail, BOOT_TEXT_X, BOOT_LIST_Y + 28, 1, COL_ACC_600,
                   maxCharsForSize(1));
  }

  drawThinBar(BOOT_TEXT_X, BOOT_BAR_Y, BOOT_BAR_W, progress, COL_ACC_400, COL_TRACK);

  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, COL_ACC_800);
  drawCenteredLine(footerNote, centeredY(FOOTER_Y, FOOTER_H, 1), 1, COL_ACC_600);
}

// Aggiorna la sola barra della schermata di avvio. Serve alla pressione
// prolungata del reset WiFi, che deve mostrare il tempo che passa: ridisegnare
// tutta la schermata a ogni campione farebbe sfarfallare il testo, e a 40 MHz
// di SPI un riempimento a pieno schermo non e' nemmeno gratis.
void drawBootProgress(float progress) {
  drawThinBar(BOOT_TEXT_X, BOOT_BAR_Y, BOOT_BAR_W, progress, COL_AMBER, COL_TRACK_WARM);
}

// ---------------------------------------------------------------------------
// QR CODE
// ---------------------------------------------------------------------------
// Il generatore di ESP-IDF non restituisce la matrice: la passa a una callback
// e la libera appena questa ritorna. Dove e quanto grande disegnarla sta
// quindi in variabili di file, impostate da drawQrCode() prima di generarla.
//
// E' l'unico punto dell'interfaccia fuori palette: moduli neri su fondo
// bianco. Molti lettori di QR non riconoscono un codice a colori invertiti, e
// il contrasto massimo e' cio' che lo rende leggibile a 2 mm per modulo.
constexpr uint16_t QR_DARK  = 0x0000;
constexpr uint16_t QR_LIGHT = 0xFFFF;

// Margine bianco attorno al codice, in moduli. Lo standard ne chiede quattro;
// su fondo nero due bastano ai telefoni, e lo spazio risparmiato permette
// moduli da 3 px invece che da 2.
constexpr int16_t QR_QUIET_MODULES = 2;

// Versione massima: la 10 contiene 271 caratteri con correzione bassa, e
// l'URL del login ne occupa 270. Qualche versione in piu' e' margine
// per un Client ID o un Redirect URI piu' lunghi; il lato si adatta da solo.
constexpr int QR_MAX_VERSION = 13;

struct QrPlacement {
  int16_t x;
  int16_t y;
  int16_t maxSide;
  int16_t drawnSide;
};

QrPlacement qrPlacement = {0, 0, 0, 0};

#if QR_AVAILABLE
void drawQrModules(esp_qrcode_handle_t qrcode) {
  int16_t size = static_cast<int16_t>(esp_qrcode_get_size(qrcode));
  int16_t modules = size + 2 * QR_QUIET_MODULES;
  int16_t scale = qrPlacement.maxSide / modules;

  if (scale < 1) {
    qrPlacement.drawnSide = 0;
    return;
  }

  int16_t side = modules * scale;
  tft.fillRect(qrPlacement.x, qrPlacement.y, side, side, QR_LIGHT);

  int16_t originX = qrPlacement.x + QR_QUIET_MODULES * scale;
  int16_t originY = qrPlacement.y + QR_QUIET_MODULES * scale;

  // I moduli scuri contigui di una riga si dipingono con un rettangolo solo:
  // un QR della versione 10 ha circa 1600 moduli scuri, ma in corse
  // orizzontali diventano poche centinaia di transazioni SPI.
  for (int16_t row = 0; row < size; ++row) {
    int16_t col = 0;
    while (col < size) {
      if (!esp_qrcode_get_module(qrcode, col, row)) {
        ++col;
        continue;
      }

      int16_t runStart = col;
      while (col < size && esp_qrcode_get_module(qrcode, col, row)) {
        ++col;
      }

      tft.fillRect(originX + runStart * scale, originY + row * scale,
                   (col - runStart) * scale, scale, QR_DARK);
    }
  }

  qrPlacement.drawnSide = side;
}
#endif

// Disegna `text` come QR code nel quadrato di lato massimo `maxSide` con
// l'angolo in alto a sinistra in (x, y), usando il modulo piu' grande che ci
// sta. Restituisce il lato effettivo in pixel, 0 se non e' stato possibile
// (testo troppo lungo, spazio insufficiente o generatore assente nel core).
int16_t drawQrCode(const char *text, int16_t x, int16_t y, int16_t maxSide) {
#if QR_AVAILABLE
  qrPlacement = {x, y, maxSide, 0};

  esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
  config.display_func = drawQrModules;
  config.max_qrcode_version = QR_MAX_VERSION;
  config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

  if (esp_qrcode_generate(&config, text) != ESP_OK) {
    return 0;
  }

  return qrPlacement.drawnSide;
#else
  (void)text;
  (void)x;
  (void)y;
  (void)maxSide;
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// ICONA DEL METEO
// ---------------------------------------------------------------------------
// Disegnata con primitive, non con bitmap. Otto pittogrammi a bitmap
// costerebbero qualche KB di flash ciascuno e sarebbero legati a una
// dimensione sola; disegnati costano un pugno di istruzioni, restano netti a
// qualunque misura e si ritingono cambiando un colore.
//
// `size` e' il lato del quadrato in cui l'icona sta, `cx`/`cy` il suo centro.
// Tutto dentro e' in frazioni di `size`, cosi' la stessa funzione serve per il
// riquadro grande e per un eventuale uso piccolo.

// Nuvola: tre lobi e una base piatta. E' la forma che compare in sei degli
// otto pittogrammi, quindi vive per conto suo.
static void drawCloud(int16_t cx, int16_t cy, int16_t w, uint16_t color) {
  int16_t r = w / 4;
  tft.fillCircle(cx - r, cy, r, color);
  tft.fillCircle(cx + r, cy, r * 3 / 4, color);
  tft.fillCircle(cx, cy - r * 2 / 3, r * 5 / 6, color);
  tft.fillRect(cx - r, cy, 2 * r, r, color);
}

// Gocce inclinate sotto la nuvola. `count` ne cambia il numero, che e' la sola
// differenza fra pioviggine, pioggia e rovescio.
static void drawDrops(int16_t cx, int16_t cy, int16_t w, uint8_t count,
                      uint16_t color) {
  int16_t step = w / (count + 1);
  int16_t len = w / 5;

  for (uint8_t i = 0; i < count; i++) {
    int16_t x = cx - w / 2 + step * (i + 1);
    tft.drawLine(x, cy, x - len / 2, cy + len, color);
  }
}

void drawWeatherIcon(int16_t cx, int16_t cy, int16_t size, int16_t code) {
  int16_t w = size;
  uint16_t sun = COL_AMBER;
  uint16_t cloud = COL_INK;
  uint16_t water = COL_ACC_400;

  // Sereno e quasi sereno: solo il sole, con i raggi quando e' pieno.
  if (code <= 1) {
    int16_t r = w / 4;
    tft.fillCircle(cx, cy, r, sun);
    if (code == 0) {
      for (uint8_t i = 0; i < 8; i++) {
        float a = i * PI / 4.0f;
        int16_t x0 = cx + static_cast<int16_t>(cos(a) * (r + 3));
        int16_t y0 = cy + static_cast<int16_t>(sin(a) * (r + 3));
        int16_t x1 = cx + static_cast<int16_t>(cos(a) * (r + 8));
        int16_t y1 = cy + static_cast<int16_t>(sin(a) * (r + 8));
        tft.drawLine(x0, y0, x1, y1, sun);
      }
    }
    return;
  }

  // Poco nuvoloso: sole che sbuca da dietro la nuvola, come nel disegno.
  if (code == 2) {
    tft.fillCircle(cx - w / 5, cy + w / 8, w / 6, sun);
    drawCloud(cx + w / 10, cy - w / 12, w * 3 / 4, cloud);
    return;
  }

  // Nebbia: nuvola e righe orizzontali sotto.
  if (code == 45 || code == 48) {
    drawCloud(cx, cy - w / 6, w * 3 / 4, cloud);
    for (uint8_t i = 0; i < 3; i++) {
      int16_t y = cy + w / 5 + i * (w / 10);
      tft.drawFastHLine(cx - w / 3 + (i % 2) * 4, y, w * 2 / 3 - 4, water);
    }
    return;
  }

  // Temporale: nuvola e saetta.
  if (code >= 95) {
    drawCloud(cx, cy - w / 6, w * 3 / 4, cloud);
    int16_t x = cx - w / 12;
    int16_t y = cy + w / 6;
    tft.fillTriangle(x + w / 10, y, x - w / 12, y + w / 5,
                     x + w / 24, y + w / 5, sun);
    tft.fillTriangle(x + w / 24, y + w / 5, x + w / 8, y + w / 5,
                     x - w / 24, y + w * 2 / 5, sun);
    return;
  }

  // Neve: nuvola e fiocchi a croce.
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    drawCloud(cx, cy - w / 6, w * 3 / 4, cloud);
    for (uint8_t i = 0; i < 3; i++) {
      int16_t x = cx - w / 4 + i * (w / 4);
      int16_t y = cy + w / 4;
      tft.drawFastHLine(x - 3, y, 7, water);
      tft.drawFastVLine(x, y - 3, 7, water);
    }
    return;
  }

  // Tutto il resto e' acqua che cade: cambia solo quanta.
  uint8_t drops = (code == 65 || code == 82) ? 5
                : (code >= 61)               ? 4
                                             : 3;
  drawCloud(cx, cy - w / 6, w * 3 / 4, cloud);
  drawDrops(cx, cy + w / 5, w * 2 / 3, drops, water);
}
