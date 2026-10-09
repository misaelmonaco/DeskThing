// ===========================================================================
// ALBUM ART - scaricamento e decodifica della copertina
// ---------------------------------------------------------------------------
// Scarica il JPEG della copertina da i.scdn.co e lo disegna nel riquadro della
// pagina player. Il file compresso resta in RAM associato al suo URL: un
// ridisegno di pagina lo ridecodifica, non lo riscarica.
//
// ---------------------------------------------------------------------------
// COSA COSTA, IN MEMORIA
// ---------------------------------------------------------------------------
// La scheda non ha PSRAM, quindi ogni byte esce dai circa 100-150 KB di heap
// che restano liberi con WiFi e TLS attivi.
//
//   JPEG compresso in RAM      15-25 KB tipici, 32 KB il tetto imposto
//   area di lavoro TJpgDec     ~3.1 KB, allocata e liberata per decodifica
//   stato (URL + puntatori)    ~180 byte
//   ------------------------------------------------------------------------
//   residente fra un brano e l'altro    15-25 KB
//   picco durante lo scaricamento       +40 KB di TLS, +2 KB di HTTPClient
//
// Il picco e' il numero che conta: con una copertina da 25 KB in RAM, lo
// scaricamento della successiva chiede 25 + 40 + 2 = 67 KB contemporaneamente.
// Per questo artFetch() libera il buffer vecchio PRIMA di aprire la
// connessione, e prima di allocare controlla ESP.getMaxAllocHeap() - cioe' il
// blocco contiguo piu' grande, non l'heap totale - tenendo ART_TLS_MARGIN di
// margine. Se non c'e' spazio salta tutto e lascia il segnaposto: una
// copertina mancante e' un difetto estetico, un heap esaurito e' un riavvio.
//
// L'alternativa era tenere in RAM il raster decodificato invece del JPEG:
// 75 x 75 x 2 = 11 KB fissi, allocabili una volta all'avvio, e ridisegno
// istantaneo. Costa meno memoria ed e' piu' prevedibile, ma obbliga a
// riscaricare quando il buffer viene invalidato. Qui si e' scelto di tenere il
// compresso, quindi il ridisegno costa una decodifica (~150 ms) invece di una
// copia.
// ===========================================================================

// Riquadro in cui il decoder ha il permesso di scrivere. Il callback di
// TJpgDec non riceve nessun contesto, quindi la destinazione passa da qui.
// Bordo inferiore del riquadro, in coordinate schermo. Il callback di TJpgDec
// non riceve nessun contesto, quindi il limite passa da qui.
static int16_t artClipBottom = 0;

// URL per cui e' stato chiesto un ridisegno ma non ancora fatto lo
// scaricamento. Serve a far comparire il segnaposto PRIMA dei due secondi di
// download: al primo giro si libera il vecchio e si chiede il ridisegno, al
// giro dopo si scarica. Senza questo passaggio il segnaposto non si vedrebbe
// mai, perche' il ridisegno avviene dopo, nello stesso giro di loop.
static char artArmedUrl[ART_URL_MAX] = "";

// ---------------------------------------------------------------------------
// GESTIONE DEL BUFFER
// ---------------------------------------------------------------------------
void artRelease() {
  if (albumArt.data != nullptr) {
    free(albumArt.data);
    albumArt.data = nullptr;
  }
  albumArt.size = 0;
  albumArt.capacity = 0;
  albumArt.ready = false;
  albumArt.url[0] = '\0';
}

bool artHasImage() {
  return albumArt.ready && albumArt.data != nullptr && albumArt.size > 0;
}

bool artHoldsUrl(const char *url) {
  return url != nullptr && url[0] != '\0' && strcmp(albumArt.url, url) == 0;
}

// Quanto possiamo allocare lasciando in piedi il margine per il TLS.
// getMaxAllocHeap() e' il blocco contiguo piu' grande: su un heap frammentato
// e' molto meno di getFreeHeap(), ed e' quello che fa fallire una malloc.
size_t artBudget() {
  size_t largest = ESP.getMaxAllocHeap();
  if (largest <= ART_TLS_MARGIN) {
    return 0;
  }
  size_t budget = largest - ART_TLS_MARGIN;
  return budget > ART_MAX_BYTES ? ART_MAX_BYTES : budget;
}

// Porta la capacita' almeno a `needed`, entro il tetto concesso.
static bool artReserve(size_t needed, size_t limit) {
  if (needed <= albumArt.capacity) {
    return true;
  }
  if (needed > limit) {
    return false;
  }

  size_t target = albumArt.capacity == 0 ? ART_CHUNK : albumArt.capacity;
  while (target < needed) {
    target += ART_CHUNK;
  }
  if (target > limit) {
    target = limit;
  }

  uint8_t *grown = (uint8_t *) realloc(albumArt.data, target);
  if (grown == nullptr) {
    return false;
  }

  albumArt.data = grown;
  albumArt.capacity = target;
  return true;
}

// ---------------------------------------------------------------------------
// SCARICAMENTO
// ---------------------------------------------------------------------------
// Lo stream HTTPS non va letto con una sola readBytes: su TLS restituisce
// quello che ha decifrato finora, che e' quasi sempre meno del totale, e il
// resto andrebbe perso. Qui si cicla finche' lo stream non finisce, con una
// scadenza di inattivita' che si rimanda a ogni byte ricevuto: un download
// lento prosegue, uno fermo viene abbandonato.
bool artFetch(const char *url) {
  if (url == nullptr || url[0] == '\0') {
    return false;
  }

  // Il buffer vecchio se ne va prima di aprire la connessione: tenerlo
  // mentre il TLS chiede i suoi 40 KB e' il modo piu' diretto di finire
  // l'heap proprio nel momento peggiore.
  artRelease();

  size_t limit = artBudget();
  Serial.printf("[art] URL %s\n", url);
  Serial.printf("[art] heap libero %u, blocco massimo %u, tetto %u\n",
                (unsigned) ESP.getFreeHeap(), (unsigned) ESP.getMaxAllocHeap(),
                (unsigned) limit);

  if (limit < ART_CHUNK) {
    Serial.println("[art] memoria insufficiente: scaricamento saltato");
    albumArt.unavailable = true;
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();          // nessuna validazione del certificato
  client.setTimeout(ART_HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(ART_HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    Serial.println("[art] begin() fallita");
    albumArt.unavailable = true;
    return false;
  }

  int status = http.GET();
  int expected = http.getSize();
  Serial.printf("[art] HTTP %d, Content-Length %d\n", status, expected);

  if (status != HTTP_CODE_OK) {
    http.end();
    albumArt.unavailable = true;
    return false;
  }

  if (expected > 0 && (size_t) expected > limit) {
    Serial.printf("[art] file da %d byte oltre il tetto di %u: saltato\n",
                  expected, (unsigned) limit);
    http.end();
    albumArt.unavailable = true;
    return false;
  }

  // Con Content-Length si alloca una volta; senza, si parte da un blocco e si
  // cresce con realloc man mano che arrivano i dati.
  size_t initial = (expected > 0) ? (size_t) expected : ART_CHUNK;
  if (!artReserve(initial, limit)) {
    Serial.println("[art] allocazione iniziale fallita");
    http.end();
    artRelease();
    albumArt.unavailable = true;
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t received = 0;
  unsigned long deadline = millis() + ART_STALL_MS;
  bool overflow = false;

  while (true) {
    if (expected > 0 && received >= (size_t) expected) {
      break;
    }

    size_t available = stream->available();

    if (available == 0) {
      // Connessione chiusa e niente in canna: con Content-Length ignoto e'
      // il solo modo di sapere che il file e' finito.
      if (!stream->connected()) {
        break;
      }
      if ((long) (millis() - deadline) >= 0) {
        Serial.println("[art] stream fermo: scadenza superata");
        break;
      }
      delay(1);
      continue;
    }

    if (received + available > albumArt.capacity) {
      if (!artReserve(received + available, limit)) {
        Serial.printf("[art] oltre il tetto a %u byte: troncato\n",
                      (unsigned) received);
        overflow = true;
        break;
      }
    }

    int read = stream->readBytes(albumArt.data + received,
                                 albumArt.capacity - received);
    if (read > 0) {
      received += (size_t) read;
      deadline = millis() + ART_STALL_MS;   // si rimanda a ogni byte ricevuto
    }
  }

  http.end();
  albumArt.size = received;

  Serial.printf("[art] attesi %d, ricevuti %u, heap libero %u\n",
                expected, (unsigned) received, (unsigned) ESP.getFreeHeap());

  bool complete = received > 0 && !overflow &&
                  (expected <= 0 || received == (size_t) expected);

  if (!complete) {
    Serial.println("[art] scaricamento incompleto: buffer scartato");
    artRelease();
    albumArt.unavailable = true;
    return false;
  }

  // Si verifica che l'immagine sia davvero della misura attesa: il rapporto di
  // riduzione e' fisso, quindi una sorgente diversa produrrebbe un riquadro
  // della misura sbagliata che sborderebbe sul testo.
  uint16_t w = 0, h = 0;
  if (TJpgDec.getJpgSize(&w, &h, albumArt.data, albumArt.size) != JDR_OK) {
    Serial.println("[art] intestazione JPEG illeggibile");
    artRelease();
    albumArt.unavailable = true;
    return false;
  }

  Serial.printf("[art] JPEG %ux%u, scala 1/%u -> %ux%u px\n",
                w, h, ART_SCALE, w / ART_SCALE, h / ART_SCALE);

  if (w / ART_SCALE > (uint16_t) ART_SIZE || h / ART_SCALE > (uint16_t) ART_SIZE) {
    Serial.println("[art] misura inattesa per il riquadro: scartata");
    artRelease();
    albumArt.unavailable = true;
    return false;
  }

  copyText(albumArt.url, sizeof(albumArt.url), url);
  albumArt.ready = true;
  albumArt.unavailable = false;
  return true;
}

// ---------------------------------------------------------------------------
// DECODIFICA
// ---------------------------------------------------------------------------
// Callback di TJpgDec: riceve un blocco gia' in RGB565 e lo riversa sul
// pannello. Il brief parlava di pushImage(), che e' l'API di TFT_eSPI; qui il
// driver e' Adafruit_ST7789, e l'equivalente di Adafruit_GFX e'
// drawRGBBitmap(), che prende lo stesso blocco con gli stessi parametri.
//
// Restituire false interrompe la decodifica: si usa quando il blocco cade
// sotto il riquadro, cosi' un'immagine piu' alta del previsto non continua a
// scrivere sul resto della pagina.
// Le coordinate che arrivano sono gia' ASSOLUTE: la libreria somma da se'
// l'origine passata a drawJpg() (x = jrect->left + jpeg_x nel suo sorgente).
// Sommarla di nuovo qui disegnerebbe il blocco al doppio dell'offset, e
// confrontare y con la sola altezza del riquadro invece che con il suo bordo
// inferiore fa restituire false al secondo blocco: la decodifica si interrompe
// e drawJpg() torna JDR_INTR, che e' il codice 1.
bool artBlockOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= artClipBottom) {
    return false;
  }

  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

void artSetupDecoder() {
  TJpgDec.setJpgScale(ART_SCALE);

  // Nessuno scambio di byte. setSwapBytes(true) serve a TFT_eSPI, che riversa
  // il blocco come sequenza di byte grezzi; drawRGBBitmap di Adafruit_GFX
  // invece legge ogni elemento come uint16_t e lo manda lui al pannello nel
  // verso giusto. Scambiarli qui darebbe colori invertiti - il verde resta
  // verde, ma rosso e blu si scambiano.
  TJpgDec.setSwapBytes(false);

  TJpgDec.setCallback(artBlockOutput);
}

// Disegna la copertina nel riquadro. Ridecodifica dal JPEG in RAM: e' il
// prezzo di tenere in memoria il compresso invece del raster.
bool artDraw(int16_t x, int16_t y) {
  if (!artHasImage()) {
    return false;
  }

  artClipBottom = y + ART_SIZE;

  unsigned long started = millis();
  JRESULT result = TJpgDec.drawJpg(x, y, albumArt.data, albumArt.size);

  if (result != JDR_OK) {
    Serial.printf("[art] decodifica fallita, codice %d\n", (int) result);
    return false;
  }

  Serial.printf("[art] decodificata in %lu ms\n", millis() - started);
  return true;
}

// ---------------------------------------------------------------------------
// SEGNAPOSTO
// ---------------------------------------------------------------------------
// Il riquadro resta al suo posto anche senza immagine: sparire farebbe
// ricomporre la pagina sotto gli occhi a ogni cambio di brano.
void artDrawPlaceholder(int16_t x, int16_t y, bool pending) {
  tft.fillRect(x, y, ART_SIZE, ART_SIZE, COL_TRACK);
  tft.drawRect(x, y, ART_SIZE, ART_SIZE, COL_ACC_800);

  const char *label = pending ? "..." : "-";
  drawCenteredIn(label, x, ART_SIZE, centeredY(y, ART_SIZE, 1), 1, COL_ACC_700);
}

// Disegna cio' che c'e': immagine, oppure segnaposto.
void artRender(int16_t x, int16_t y) {
  if (artHasImage() && artDraw(x, y)) {
    return;
  }

  artDrawPlaceholder(x, y, !albumArt.unavailable);
}

// ---------------------------------------------------------------------------
// AGGIORNAMENTO
// ---------------------------------------------------------------------------
// Chiamata dopo ogni poll a Spotify. Scarica solo quando l'URL cambia, e solo
// con la pagina del player a schermo: lo scaricamento blocca il loop per un
// secondo o due, e non ha senso pagarlo per una pagina che nessuno guarda.
void refreshAlbumArt() {
  const char *wanted = spotifyState.artUrl;

  if (wanted[0] == '\0') {
    if (artHasImage() || albumArt.unavailable) {
      artRelease();
      albumArt.unavailable = false;
      artArmedUrl[0] = '\0';
      markDisplayDirty();
    }
    return;
  }

  if (artHoldsUrl(wanted)) {
    return;
  }

  // Un tentativo per URL. Senza questo controllo un fallimento si ripete a
  // ogni poll: una sessione TLS ogni cinque secondi, per sempre, con il loop
  // bloccato a ogni giro. Se il brano cambia e poi torna, artArmedUrl cambia e
  // il tentativo si rifa'.
  if (albumArt.unavailable && strcmp(artArmedUrl, wanted) == 0) {
    return;
  }

  if (currentPage != PAGE_SPOTIFY || WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Primo giro con un URL nuovo: si butta la copertina vecchia, si azzera
  // l'eventuale esito negativo precedente e si chiede il ridisegno. Lo
  // scaricamento aspetta il giro dopo, quando il segnaposto e' gia' a schermo.
  if (strcmp(artArmedUrl, wanted) != 0) {
    copyText(artArmedUrl, sizeof(artArmedUrl), wanted);
    artRelease();
    albumArt.unavailable = false;
    markDisplayDirty();
    return;
  }

  artFetch(wanted);
  markDisplayDirty();
}
