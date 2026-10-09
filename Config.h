#pragma once

// ===========================================================================
// CONFIGURAZIONE HARDWARE
// ESP32 DevKit + display ST7789 240x320 + sensore DHT22 + 4 pulsanti
// ===========================================================================
//
// ---------------------------------------------------------------------------
// CABLAGGIO 1/3 - DISPLAY ST7789 (240x320, SPI hardware sul bus HSPI)
// ---------------------------------------------------------------------------
//   Pin modulo ST7789     ESP32        Note
//   ---------------------------------------------------------------------
//   GND                -> GND
//   VCC                -> 3.3V        5V solo se il modulo ha il regolatore
//   SCL / SCK / CLK    -> GPIO 15     clock SPI (HSPI CLK)
//   SDA / MOSI / DIN   -> GPIO 13     dati SPI  (HSPI MOSI)
//   RES / RST          -> GPIO 22     reset del pannello
//   DC  / RS / A0      -> GPIO 21     selettore dati/comando
//   CS  / SS           -> GPIO 2      chip select
//   BLK / LED / BL     -> 3.3V        retroilluminazione sempre accesa
//   MISO / SDO         -> NON COLLEGATO
//
//   Il pannello e' di sola scrittura: il filo MISO va lasciato staccato in
//   modo che il GPIO 12 resti libero. Il GPIO 12 e' un pin di strapping e se
//   viene trovato alto durante il boot l'ESP32 non si avvia.
//
//   Anche GPIO 2 e GPIO 15 sono pin di strapping, ma tollerano l'uso come
//   CS e CLK perche' il display li pilota solo dopo il boot. Se la scheda
//   dovesse rifiutare l'upload, staccare il display durante il flash oppure
//   spostare il CS su un pin libero (es. GPIO 5) aggiornando TFT_CS.
//
// ---------------------------------------------------------------------------
// CABLAGGIO 2/3 - SENSORE DHT22 (AM2302)
// ---------------------------------------------------------------------------
//   Pin DHT22             ESP32
//   ---------------------------------------------------------------------
//   1  VCC             -> 3.3V
//   2  DATA            -> GPIO 27
//   3  NC              -> non collegato
//   4  GND             -> GND
//
//   Tra DATA e VCC serve una resistenza di pull-up da 10 kOhm. I moduli
//   gia' montati su breakout a 3 pin la integrano: in quel caso non va
//   aggiunta. Il DHT22 non accetta letture piu' frequenti di 2 secondi.
//
// ---------------------------------------------------------------------------
// CABLAGGIO 3/3 - 4 PULSANTI (attivi a massa)
// ---------------------------------------------------------------------------
//   Pulsante              ESP32        Funzione
//   ---------------------------------------------------------------------
//   K1 (un capo)       -> GPIO 25      cambio pagina
//   K2 (un capo)       -> GPIO 26      azione contestuale
//   K3 (un capo)       -> GPIO 32      azione contestuale
//   K4 (un capo)       -> GPIO 33      azione contestuale
//   K1..K4 (altro capo)-> GND          in comune su un'unica barra di massa
//
//   I pin sono configurati in INPUT_PULLUP: a riposo leggono HIGH e vanno a
//   LOW alla pressione. Non serve nessuna resistenza esterna. I GPIO 34-39
//   sono volutamente evitati perche' non dispongono di pull-up interno.
//
// ---------------------------------------------------------------------------
// RIEPILOGO GPIO OCCUPATI
// ---------------------------------------------------------------------------
//   2  CS display      13 MOSI display   15 SCK display   21 DC display
//   22 RST display     25 K1             26 K2            27 DATA DHT22
//   32 K3              33 K4
//   Liberi e sicuri per espansioni: 4, 14, 16, 17, 18, 19, 23, 34-39 (solo input)
// ===========================================================================

// ---------------------------------------------------------------------------
// PINOUT DISPLAY ST7789
// ---------------------------------------------------------------------------
#define TFT_CS   2
#define TFT_DC   21
#define TFT_RST  22
#define TFT_MOSI 13
#define TFT_SCLK 15

// Risoluzione nativa del pannello in orientamento verticale (rotation 0).
// La init della libreria vuole sempre queste, a prescindere dalla rotazione.
constexpr int16_t TFT_WIDTH_PX = 240;
constexpr int16_t TFT_HEIGHT_PX = 320;

// Orientamento dell'interfaccia. Le schermate sono impaginate in orizzontale,
// 320 px di larghezza per 240 di altezza: rotation 1 ruota il pannello di 90
// gradi in senso orario e scambia gli assi. Theme.h deriva SCREEN_W e SCREEN_H
// da questa costante, quindi tornare al verticale richiede solo di rimetterla
// a 0 e ricompilare.
constexpr uint8_t TFT_ROTATION = 1;
constexpr bool TFT_LANDSCAPE = (TFT_ROTATION == 1 || TFT_ROTATION == 3);

// Frequenza del bus SPI. Se compaiono artefatti grafici scendere a 27 o 20 MHz.
constexpr uint32_t TFT_SPI_HZ = 40000000UL;

// La init della libreria Adafruit lascia il pannello in INVON: su questi
// moduli i colori risultano corretti solo disattivando l'inversione.
constexpr bool TFT_COLOR_INVERSION = false;

// ---------------------------------------------------------------------------
// PINOUT SENSORE DHT22
// ---------------------------------------------------------------------------
#define DHTPIN  27
#define DHTTYPE DHT22

// ---------------------------------------------------------------------------
// PINOUT PULSANTI
// ---------------------------------------------------------------------------
#define BTN_K1 25
#define BTN_K2 26
#define BTN_K3 32
#define BTN_K4 33

// ---------------------------------------------------------------------------
// TEMPORIZZAZIONI
// ---------------------------------------------------------------------------
// Finestra di antirimbalzo, applicata dentro la ISR dei pulsanti.
// Tutti i fronti che arrivano entro questo tempo dal precedente appartengono
// alla stessa pressione. 25 ms coprono abbondantemente il rimbalzo di un
// microswitch, che si esaurisce in genere entro 5 ms. Se la diagnostica
// mostrasse treni di rimbalzi piu' lunghi, e' questo il valore da alzare.
constexpr uint32_t BUTTON_DEBOUNCE_US = 25000;

// Soglia oltre la quale una pressione conta come lunga. 700 ms e' il valore
// che separa bene l'intenzione dall'esitazione: sotto si preme, sopra si
// tiene premuto. Su K1 la pressione lunga riporta all'orologio; sugli altri
// tasti vale come una pressione normale, cosi' tenere premuto per sbaglio non
// lascia mai il tasto senza risposta.
constexpr uint32_t BUTTON_LONG_PRESS_MS = 700;

// Tempo dopo il quale il loop considera attendibile un disaccordo fra il
// livello del pin e lo stato della macchina a stati, e la riallinea.
//
// Serve perche' un fronte puo' andare perso: la lettura del DHT22 disabilita
// gli interrupt, e un rilascio che cade in quella finestra non arriva mai alla
// ISR. Senza rete di sicurezza il tasto resterebbe per sempre "premuto" e non
// risponderebbe piu'. Sei volte la finestra di antirimbalzo e' abbastanza da
// non intromettersi mai in un rimbalzo in corso.
constexpr uint32_t BUTTON_RESYNC_US = 6 * BUTTON_DEBOUNCE_US;

// Numero massimo di pressioni che possono restare in coda per un singolo
// tasto. Serve a non farsi travolgere da una raffica di disturbi, ma deve
// restare > 1: e' proprio la coda che permette di non perdere la seconda
// pressione quando la prima ha avviato una chiamata di rete bloccante.
constexpr uint8_t BUTTON_QUEUE_MAX = 4;

// Diagnostica dei pulsanti. Con true, ogni secondo viene stampato sulla
// seriale quanti fronti ha visto ciascun tasto e quanti ne ha scartati
// l'antirimbalzo. Come si legge, premendo un tasto una volta sola:
//   fronti 2,  scarti 0  -> contatto pulito, tutto a posto
//   fronti 30, scarti 28 -> rimbalza parecchio, ma il filtro lo assorbe
//   fronti 30, scarti 10 -> il filtro non copre il rimbalzo: alzare
//                           BUTTON_DEBOUNCE_US
//   fronti 0,  scarti 0  -> il segnale non arriva al pin: problema elettrico
constexpr bool BUTTON_DIAGNOSTICS = true;
constexpr unsigned long BUTTON_DIAG_REPORT_MS = 1000;

// Periodo del loop principale: 10 ms significa cento giri al secondo, quindi
// un tick dell'orologio non puo' arrivare in ritardo di piu' di 10 ms. Non
// serve scendere: quando i secondi saltano la causa e' una chiamata bloccante
// dentro al loop, non la frequenza con cui il loop gira.
constexpr unsigned long LOOP_DELAY_MS = 10;

// Il DHT22 si legge spesso solo quando la sua pagina e' sotto gli occhi.
// Non e' un risparmio di corrente: la libreria disabilita gli interrupt per
// tutta la durata della lettura one-wire, e in quella finestra le pressioni
// dei pulsanti non arrivano. Farlo ogni tre secondi a pagina spenta significa
// bucare l'input venti volte al minuto per dei numeri che nessuno guarda.
// ---------------------------------------------------------------------------
// COPERTINA DELL'ALBUM
// ---------------------------------------------------------------------------
// Spotify fornisce la copertina a 640, 300 e 64 px, e il decoder riduce solo
// per 1/2, 1/4 e 1/8: la misura a schermo non si sceglie, si ricava.
//
// Si usa la sorgente da 64 px senza riduzione. La prima versione scaricava
// quella da 300 e la riduceva a 1/4, ma era un errore di impostazione: su
// questo pannello l'area attiva misura circa 49 mm per 320 pixel, cioe' 0,153
// mm per pixel, quindi il riquadro della copertina e' un quadrato di DIECI
// MILLIMETRI. Per riempirlo si scaricava un file da 300x300 - misurato sul
// campo: 34 KB una copertina, 55 KB la successiva - e se ne buttavano tre
// quarti in decodifica.
//
// Con la sorgente da 64 px il file pesa pochi kilobyte, non serve nessuna
// riduzione, e il problema della memoria sparisce invece di essere inseguito
// alzando un tetto che la copertina dopo supera di nuovo. A dieci millimetri
// le due versioni sono indistinguibili.
constexpr int16_t ART_SOURCE_PX = 64;    // quale delle tre misure si scarica
constexpr uint8_t ART_SCALE     = 1;     // nessuna riduzione
constexpr int16_t ART_SIZE      = ART_SOURCE_PX / ART_SCALE;

// Tetto alla dimensione del file. Con la sorgente da 64 px bastano pochi
// kilobyte; 16 e' un limite largo che serve solo a non inseguire un file
// assurdo se un giorno l'immagine cambiasse natura.
//
// Il guardiano vero non e' questo: e' ART_TLS_MARGIN confrontato con
// getMaxAllocHeap(), che e' cio' che impedisce di allocare mangiandosi lo
// spazio contiguo che servira' all'handshake TLS.
constexpr size_t ART_MAX_BYTES  = 16 * 1024;

// Margine da lasciare libero per l'handshake TLS. Prima di allocare si
// controlla ESP.getMaxAllocHeap(), che e' il blocco contiguo piu' grande
// disponibile: e' quello che conta, non l'heap totale.
constexpr size_t ART_TLS_MARGIN = 40 * 1024;

// Allocazione iniziale quando il server non manda Content-Length, e passo di
// crescita della realloc.
constexpr size_t ART_CHUNK      = 8 * 1024;

// Scadenza di inattivita' dello stream: si rimanda a ogni byte ricevuto, cosi'
// un download lento non viene interrotto ma uno fermo si.
constexpr unsigned long ART_STALL_MS = 4000;
constexpr unsigned long ART_HTTP_TIMEOUT_MS = 8000;

constexpr size_t ART_URL_MAX = 160;

constexpr unsigned long DHT_REFRESH_MS = 3000;
constexpr unsigned long DHT_IDLE_REFRESH_MS = 30000;
constexpr unsigned long SPOTIFY_REFRESH_MS = 5000;
constexpr unsigned long POMODORO_WORK_MS = 40UL * 60UL * 1000UL;
constexpr unsigned long POMODORO_BREAK_MS = 10UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// RETE
// ---------------------------------------------------------------------------
// Nome dell'access point che il dispositivo CREA quando va configurato.
// Non e' una rete a cui si collega: le credenziali della rete di casa non
// stanno nel codice, le salva WiFiManager nella NVS dell'ESP32 dopo la prima
// configurazione dal portale.
constexpr const char *WIFI_PORTAL_SSID = "SpotifyThing-Setup";
constexpr unsigned int WIFI_PORTAL_TIMEOUT_S = 180;

// Cancellazione della rete salvata. Serve quando si cambia router o si sposta
// il dispositivo: senza, l'unico modo per fargli dimenticare la rete sarebbe
// riflashare o azzerare la NVS.
//
// Via a freddo: tenere premuto questo tasto all'accensione. Funziona anche
// quando il dispositivo non riesce piu' a connettersi e quindi non si arriva
// alla pagina orologio.
#define BTN_WIFI_RESET BTN_K4

// Posizione dello stesso tasto dentro l'array buttons[]. L'attesa a freddo
// interroga la macchina a stati dell'antirimbalzo invece del pin, quindi le
// serve l'indice e non il numero di GPIO.
constexpr size_t BUTTON_WIFI_RESET_INDEX = 3;

constexpr unsigned long WIFI_RESET_HOLD_MS = 3000;

// Via a caldo: K4 sulla pagina orologio, con conferma entro questo tempo.
// La doppia pressione evita di perdere la rete per un tasto sfiorato.
constexpr unsigned long WIFI_FORGET_CONFIRM_MS = 5000;
// ---------------------------------------------------------------------------
// COLLEGAMENTO SPOTIFY
// ---------------------------------------------------------------------------
// Il login si fa dal telefono inquadrando un QR code sul display. Spotify
// accetta come indirizzo di ritorno solo pagine HTTPS, che l'ESP32 non puo'
// servire: il ritorno arriva a un piccolo server pubblico, lo stesso usato
// dalla libreria SpotifyEsp32 5.x, e il dispositivo va a ritirarci il codice.
// La 3.x usava spotifyesp32.vercel.app: se l'app Spotify era stata registrata
// con quello, nella dashboard va aggiunto il Redirect URI qui sotto.
//
// Dal server passa solo il codice monouso del login, che senza il Client
// Secret non serve a nessuno; lo scambio con il token lo fa il dispositivo
// direttamente con Spotify. Per non dipendere da un servizio di terzi basta
// ospitarne una copia e cambiare queste due righe (e il Redirect URI
// registrato nella dashboard Spotify, che deve coincidere carattere per
// carattere con SPOTIFY_REDIRECT_URI).
constexpr const char *SPOTIFY_AUTH_HOST = "spesp32.finianlandes.workers.dev";
constexpr const char *SPOTIFY_REDIRECT_URI = "https://spesp32.finianlandes.workers.dev/api/spotify/callback";

// Permessi chiesti all'account: leggere il brano in corso e comandare il
// player. Sono i soli due che servono, e tenerli al minimo non e' solo
// educazione: ogni permesso allunga l'URL, e un URL lungo diventa un QR code
// cosi' fitto che sul display da 2 pollici il telefono non lo legge piu'.
// Gli spazi fra i permessi vanno scritti come %20.
constexpr const char *SPOTIFY_SCOPES = "user-read-currently-playing%20user-modify-playback-state";

// Ogni quanto si chiede al server se il login e' stato completato. E' una
// chiamata HTTPS bloccante, e si fa solo con la pagina del QR a schermo.
constexpr unsigned long SPOTIFY_LOGIN_POLL_MS = 4000;
constexpr uint16_t SPOTIFY_LOGIN_HTTP_TIMEOUT_MS = 4000;

// Scollegare l'account: K4 tenuto premuto sulla pagina Spotify, poi K4 di
// nuovo entro questo tempo.
constexpr unsigned long SPOTIFY_UNLINK_CONFIRM_MS = 5000;

// Spazio della NVS in cui stanno Client ID, Secret e refresh token.
constexpr const char *SPOTIFY_NVS_NAMESPACE = "spotify";

// ---------------------------------------------------------------------------
// METEO ESTERNO
// ---------------------------------------------------------------------------
// Open-Meteo: niente chiave, niente account, e - cosa che qui conta piu' di
// tutte - risponde in HTTP semplice. Senza TLS non servono i 40 KB contigui
// di heap dell'handshake, che sono il vero collo di bottiglia di questa
// scheda: la copertina dell'album li ha fatti pesare abbastanza da ricordarlo.
// La risposta filtrata sta in poche centinaia di byte.
//
// Due endpoint: la geocodifica traduce una volta il nome della citta' in
// coordinate, le previsioni le interrogano. Le coordinate finiscono in NVS,
// cosi' dopo la prima volta l'avvio non ha bisogno della rete per sapere
// dove siamo.
constexpr const char *WEATHER_HOST     = "api.open-meteo.com";
constexpr const char *WEATHER_GEO_HOST = "geocoding-api.open-meteo.com";

// Spazio della NVS per citta' e coordinate. Separato da quello di Spotify:
// sono due cose che si cancellano in momenti diversi.
constexpr const char *WEATHER_NVS_NAMESPACE = "meteo";

constexpr size_t WEATHER_CITY_MAX = 40;
constexpr size_t WEATHER_URL_MAX  = 200;

// Il dato a monte si muove ogni quarto d'ora - lo dichiara la risposta stessa
// nel campo `interval` - quindi chiedere piu' spesso e' solo traffico. Dopo un
// errore si riprova prima, ma senza accanirsi.
constexpr unsigned long WEATHER_REFRESH_MS = 15UL * 60 * 1000;
constexpr unsigned long WEATHER_RETRY_MS   = 90UL * 1000;

// Oltre questa eta' il valore resta a schermo ma smorzato e con l'ora
// dell'ultima lettura. Un numero vecchio di tre ore spacciato per attuale e'
// lo stesso errore del sensore chiuso nella scatola: plausibile e falso.
constexpr unsigned long WEATHER_STALE_MS = 60UL * 60 * 1000;

constexpr uint16_t WEATHER_HTTP_TIMEOUT_MS = 6000;

constexpr const char *NTP_SERVER = "pool.ntp.org";
constexpr const char *TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";
