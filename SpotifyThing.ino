// ===========================================================================
// SpotifyThing - ESP32 + display ST7789 240x320 usato in orizzontale
// ---------------------------------------------------------------------------
// Stazione da scrivania con quattro schermate: orologio NTP, sensore
// ambientale DHT22, controllo del player Spotify e timer Pomodoro.
//
// Il cablaggio completo di display, sensore e pulsanti e' documentato in
// Config.h, che e' anche l'unico punto in cui vanno modificati i GPIO.
//
// Organizzazione dei file:
//   Config.h              pinout, cablaggi, temporizzazioni, parametri di rete
//   Secrets.h             credenziali Spotify di fabbrica (facoltativo)
//   Theme.h               palette e geometria dell'interfaccia
//   SpotifyThing.ino      stato globale e orchestrazione (questo file)
//   Display.ino           driver del pannello e primitive di disegno
//   AppLogic.ino          input, rete, sensori, logica delle funzioni
//   ScreenViews.ino       impaginazione delle singole schermate
//   SpotifyLink.ino       collegamento dell'account Spotify con QR code
// ===========================================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SpotifyEsp32.h>
#include <TJpg_Decoder.h>

// Generatore di QR code del framework ESP-IDF, gia' compreso nel core esp32
// (e' quello che usa WiFiProv): non serve installare nulla. Sta qui e non in
// Display.ino perche' il preprocessore Arduino scrive i prototipi delle
// funzioni in cima al sorgente, e quello della callback che disegna il QR
// nomina un tipo di questo header.
//
// Non si usa la libreria "QRCode" del Library Manager: il suo header ha lo
// stesso nome di questo, e con il core esp32 il compilatore prende l'altro.
#if __has_include("qrcode.h")
#include "qrcode.h"
#define QR_AVAILABLE 1
#else
#define QR_AVAILABLE 0
#endif

#include "Config.h"
#include "Theme.h"

// Secrets.h e' facoltativo. Se c'e', Client ID e Client Secret scritti li'
// fanno da valori di fabbrica; se manca, lo sketch compila lo stesso e l'app
// Spotify si configura dal portale WiFi (vedi SpotifyLink.ino). E' cio' che
// permette di distribuire lo stesso firmware a chi usa la propria app.
#if __has_include("Secrets.h")
#include "Secrets.h"
#else
constexpr const char *SPOTIFY_CLIENT_ID = "";
constexpr const char *SPOTIFY_CLIENT_SECRET = "";
#endif

// ---------------------------------------------------------------------------
// PERIFERICHE
// ---------------------------------------------------------------------------
// Il pannello viene pilotato in SPI hardware sul bus HSPI: rispetto al vecchio
// ST7735 in bit-banging il refresh e' di un ordine di grandezza piu' veloce,
// cosa necessaria con 240x320 pixel da riempire.
SPIClass hspi(HSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&hspi, TFT_CS, TFT_DC, TFT_RST);

DHT dht(DHTPIN, DHTTYPE);
Spotify *spotifyClient = nullptr;

// ---------------------------------------------------------------------------
// TESTI A LUNGHEZZA FISSA
// ---------------------------------------------------------------------------
// Nomi di brano, artisti e messaggi di stato stanno in buffer di dimensione
// fissa dentro le struct, non in String.
//
// Il motivo e' la frammentazione dell'heap. Ogni poll a Spotify riassegnava
// tre String, e ogni riassegnazione e' una free seguita da una malloc di
// misura diversa: dopo qualche migliaio di cicli l'heap e' spezzettato, e
// l'handshake TLS del poll successivo - che vuole decine di kilobyte
// contigui - non trova piu' spazio. Il guasto si presenta come un errore di
// rete dopo giorni di funzionamento, ed e' il tipo di difetto che non si
// riproduce mai al banco.
//
// Con i buffer inline la memoria di questi campi fa parte della struct, viene
// allocata una volta all'avvio e non si muove piu'. Un titolo troppo lungo
// viene troncato in scrittura: a schermo ne entrano una trentina di caratteri,
// quindi non si perde niente di visibile.
constexpr size_t TEXT_TRACK_MAX  = 64;
constexpr size_t TEXT_STATUS_MAX = 48;
constexpr size_t TEXT_CLOCK_MAX  = 6;   // "HH:MM"

// Credenziali Spotify. Client ID e Secret sono 32 caratteri esadecimali; il
// refresh token ha la stessa capienza del buffer interno della libreria.
constexpr size_t SPOTIFY_KEY_MAX        = 48;
constexpr size_t SPOTIFY_TOKEN_MAX      = 300;
constexpr size_t SPOTIFY_STATE_LEN      = 25;   // "ESP32-" + 19 caratteri casuali
constexpr size_t SPOTIFY_LOGIN_URL_MAX  = 320;

// ---------------------------------------------------------------------------
// MODELLO DELLE SCHERMATE
// ---------------------------------------------------------------------------
enum Page {
  PAGE_CLOCK = 0,
  PAGE_DHT,
  PAGE_SPOTIFY,
  PAGE_POMODORO,
  PAGE_COUNT
};

typedef void (*ScreenDrawFn)();
typedef void (*ScreenButtonFn)(size_t index);

// Configurazione plug and play di una schermata.
// Per aggiungere o modificare una pagina servono tre cose: una funzione di
// disegno, un handler dei pulsanti e una riga nella tabella screenViews[].
//
// Le etichette dei quattro tasti non stanno qui: cambiano con lo stato della
// pagina (PAUSA diventa RIPRENDI, AVVIA diventa SOSPENDI), quindi le compone
// la funzione di disegno e le passa a drawKeyBar().
struct ScreenView {
  const char *title;
  ScreenDrawFn draw;
  ScreenButtonFn onButton;
};

// Stato di un pulsante. I campi volatile sono scritti dalla ISR e letti dal
// loop: le pressioni vengono catturate dall'interrupt e messe in coda, non
// campionate a polling, cosi' non si perdono mentre il loop e' fermo dentro
// una chiamata di rete bloccante.
//
// `pressed` e' il livello logico del tasto per come la ISR lo ha accettato,
// non quello che si legge sul pin: e' lui a rendere l'antirimbalzo una
// macchina a stati invece di un filtro sui fronti. Da rilasciato si puo' solo
// passare a premuto e viceversa, quindi il rimbalzo non ha modo di generare
// due pressioni di fila.
struct Button {
  uint8_t pin;
  const char *label;
  volatile bool pressed;             // livello logico stabile
  volatile uint32_t lastEdgeUs;      // ultima transizione ACCETTATA
  volatile uint32_t pressStartUs;    // inizio della pressione in corso
  volatile uint32_t edgeCount;       // fronti totali, solo per la diagnostica
  volatile uint32_t bounceCount;     // fronti scartati dall'antirimbalzo
  volatile uint32_t resyncCount;     // riallineamenti dopo un fronte perso
  volatile uint8_t pressCount;       // pressioni brevi non ancora servite
  volatile uint8_t longPressCount;   // pressioni lunghe non ancora servite
};

// Cache dell'ultima lettura del DHT22.
struct DhtState {
  float temperature;
  float humidity;
  bool valid;
  unsigned long lastReadMs;
  // Contatore dei fallimenti consecutivi e ora dell'ultima lettura buona:
  // la schermata del sensore muto li mostra al posto dei due numeri.
  uint16_t failureCount;
  char lastValidClock[TEXT_CLOCK_MAX];
};

// Meteo esterno. `city` e' il nome scritto nel portale, `place` quello che ha
// risposto la geocodifica: non sempre coincidono, e a schermo va il secondo -
// se uno scrive "ariano" e il servizio capisce "Ariano Irpino", e' giusto che
// si legga quello che e' stato davvero interrogato.
//
// `located` dice che le coordinate ci sono; senza, non si interroga niente.
// `valid` dice che c'e' un dato e `lastOkMs` quanto e' vecchio: la pagina li
// usa tutti e due, perche' un dato vecchio si mostra ma non si spaccia per
// fresco.
struct WeatherState {
  char city[WEATHER_CITY_MAX];
  char place[WEATHER_CITY_MAX];
  float latitude;
  float longitude;
  bool located;

  float temperature;
  uint8_t humidity;
  int16_t code;          // codice WMO della condizione
  bool valid;

  unsigned long lastOkMs;
  unsigned long lastTryMs;
  char lastOkClock[TEXT_CLOCK_MAX];
  char message[40];      // perche' il dato non c'e'
};

// Stato runtime del modulo Spotify.
struct SpotifyState {
  bool configured;
  bool awaitingLogin;
  bool authenticated;
  bool playbackActive;
  bool isPlaying;
  int lastStatusCode;
  char trackName[TEXT_TRACK_MAX];
  char artistName[TEXT_TRACK_MAX];
  char statusMessage[TEXT_STATUS_MAX];
  // Posizione e durata del brano in millisecondi, per la barra di avanzamento.
  unsigned long progressMs;
  unsigned long durationMs;
  // URL della copertina nella misura che serve a noi. Vuoto se non c'e'.
  char artUrl[ART_URL_MAX];
  unsigned long lastPollMs;
  // Nessun Client ID / Secret ne' in Secrets.h ne' salvato dal portale: il
  // player non puo' nemmeno chiedere il login.
  bool appKeysMissing;
  // Prima meta' della conferma a due pressioni per scollegare l'account.
  bool unlinkArmed;
  unsigned long unlinkArmedMs;
};

// Credenziali in uso. Client ID e Secret vengono dal portale se ci sono stati
// scritti, altrimenti da Secrets.h; il refresh token solo dalla NVS, dove lo
// salva il login fatto con il QR code.
struct SpotifyCredentials {
  char clientId[SPOTIFY_KEY_MAX];
  char clientSecret[SPOTIFY_KEY_MAX];
  char refreshToken[SPOTIFY_TOKEN_MAX];
};

// Login in corso: lo state identifica questo dispositivo presso il server che
// riceve il ritorno da Spotify, l'URL e' quello codificato nel QR code.
struct SpotifyLogin {
  char state[SPOTIFY_STATE_LEN + 1];
  char url[SPOTIFY_LOGIN_URL_MAX];
  unsigned long lastPollMs;
};

// Esito di un'interrogazione del server del login. Sta qui e non in
// SpotifyLink.ino perche' il preprocessore Arduino scrive i prototipi in cima
// al sorgente, prima di qualunque tipo dichiarato nei file successivi.
enum SpotifyLoginPoll : uint8_t {
  SPOTIFY_LOGIN_PENDING = 0,  // nessuno ha ancora completato il login
  SPOTIFY_LOGIN_CODE,         // login completato: c'e' un codice da scambiare
  SPOTIFY_LOGIN_UNREACHABLE   // server non raggiungibile o risposta illeggibile
};

// Copertina dell'album tenuta in RAM. `data` e' il JPEG compresso, non il
// raster decodificato: il ridisegno costa una decodifica invece di una copia,
// ma si risparmiano i 44 KB che il raster vorrebbe a questa risoluzione.
// `url` dice a quale immagine appartiene il buffer, ed e' quello che evita di
// riscaricare la stessa copertina a ogni ridisegno di pagina.
struct AlbumArtState {
  char url[ART_URL_MAX];
  uint8_t *data;
  size_t size;        // byte validi
  size_t capacity;    // byte allocati
  bool ready;         // il buffer contiene un JPEG verificato
  bool unavailable;   // tentativo concluso senza immagine: segnaposto fisso
};

// Stato runtime del timer Pomodoro.
struct PomodoroState {
  bool running;
  bool isBreak;
  unsigned long phaseDurationMs;
  unsigned long phaseStartMs;
  unsigned long remainingMs;
  unsigned long lastTickMs;
  uint16_t completedWorkSessions;
};

Button buttons[] = {
  {BTN_K1, "K1", false, 0, 0, 0, 0, 0, 0, 0},
  {BTN_K2, "K2", false, 0, 0, 0, 0, 0, 0, 0},
  {BTN_K3, "K3", false, 0, 0, 0, 0, 0, 0, 0},
  {BTN_K4, "K4", false, 0, 0, 0, 0, 0, 0, 0},
};

constexpr size_t BUTTON_COUNT = sizeof(buttons) / sizeof(buttons[0]);

// ---------------------------------------------------------------------------
// STATO GLOBALE
// ---------------------------------------------------------------------------
Page currentPage = PAGE_CLOCK;
DhtState dhtState = {NAN, NAN, false, 0, 0, ""};
SpotifyState spotifyState = {false, false, false, false, false, 0, "", "", "Spotify non inizializzato", 0, 0, "", 0, false, false, 0};
SpotifyCredentials spotifyCredentials = {"", "", ""};
SpotifyLogin spotifyLogin = {"", "", 0};

// Alzato quando il portale salva un Client ID o un Secret nuovi: il client
// Spotify va ricostruito con quelli (vedi ensureSpotifyClient).
bool spotifyCredentialsChanged = false;

PomodoroState pomodoroState = {false, false, POMODORO_WORK_MS, 0, POMODORO_WORK_MS, 0, 0};
AlbumArtState albumArt = {"", nullptr, 0, 0, false, false};
WeatherState weather = {"", "", 0.0f, 0.0f, false,
                        NAN, 0, -1, false, 0, 0, "", "nessuna citta' scelta"};

// displayDirty governa il solo ridisegno completo della pagina, che serve
// quando cambia la schermata o un elemento statico. I valori che scorrono
// (ora, timer) non hanno un flag: le funzioni di tick in ScreenViews.ino
// confrontano il testo da mostrare con la cache di cio' che e' gia' a schermo
// e ridipingono le sole celle diverse.
bool displayDirty = true;

// Gestione della cancellazione della rete WiFi salvata.
// wifiResetRequested viene alzato dalla richiesta all'accensione e consumato
// da connectToWifi(). wifiForgetArmed e' la prima meta' della conferma a due
// pressioni disponibile dalla pagina orologio.
bool wifiResetRequested = false;
bool wifiForgetArmed = false;
unsigned long wifiForgetArmedMs = 0;

// ---------------------------------------------------------------------------
// DICHIARAZIONI - Display.ino
// ---------------------------------------------------------------------------
void setupDisplay();
void clearScreen();
void clearContentBand(int16_t y, int16_t height);
int16_t textLen(const char *text);
void drawLine(const char *text, int16_t x, int16_t y, uint8_t size, uint16_t color);
void drawCenteredLine(const char *text, int16_t y, uint8_t size, uint16_t color);
void drawCenteredIn(const char *text, int16_t x, int16_t width, int16_t y, uint8_t size, uint16_t color);
void drawFittedLine(const char *text, int16_t x, int16_t y, uint8_t size, uint16_t color, int16_t maxChars);
int16_t shownChars(const char *text, int16_t maxChars);
void drawCenteredFittedLine(const char *text, int16_t y, uint8_t size, uint16_t color, int16_t maxChars);
void drawCachedText(const char *text, char *cache, uint8_t length, int16_t x, int16_t y, uint8_t size, uint16_t color, uint16_t background, bool forceAll);
int16_t drawWrappedLines(const char *text, int16_t x, int16_t y, uint8_t size, uint16_t color, int16_t maxChars, int16_t maxLines);
void drawCornerMark(int16_t cx, int16_t cy, uint16_t color);
void drawFrame(int16_t x, int16_t y, int16_t width, int16_t height, uint16_t color);
void drawFilledFrame(int16_t x, int16_t y, int16_t width, int16_t height, uint16_t fill, uint16_t border);
void drawSignalBars(int16_t x, int16_t baselineY, int8_t bars, bool lost);
int8_t signalBarsFromRssi(int32_t rssi);
void drawStatusBar(const char *label, uint16_t labelColor, bool showSignal, int8_t signalBars, bool signalLost, const char *clockText, uint8_t pageIndex);
void drawNotice(const char *title, const char *detail);
void drawThinBar(int16_t x, int16_t y, int16_t width, float ratio, uint16_t fillColor, uint16_t trackColor);
void drawProgressBar(int16_t x, int16_t y, int16_t width, float ratio, uint16_t fillColor, uint16_t trackColor, int16_t &filledPx, bool forceAll);
void drawPomodoroPips(int16_t x, int16_t y, uint16_t completed);
void drawKeyGlyph(KeyGlyph glyph, int16_t x, int16_t y, uint16_t color);
uint16_t keyForeground(KeyStyle style);
uint16_t keyBackground(KeyStyle style);
void drawKeyBar(const KeyBar &bar);
void drawBootScreen(const char *statusLabel, const char *statusDetail, bool statusPending, float progress, const char *footerNote);
void drawBootProgress(float progress);
int16_t drawQrCode(const char *text, int16_t x, int16_t y, int16_t maxSide);

// ---------------------------------------------------------------------------
// DICHIARAZIONI - AppLogic.ino
// ---------------------------------------------------------------------------
void IRAM_ATTR enqueueRelease(Button &button, uint32_t releaseUs);
bool takeShortPress(Button &button);
bool takeLongPress(Button &button);
bool isButtonHeld(const Button &button);
void resyncButtonStates();
void setupButtons();
void clearButtonQueue();
void reportButtonDiagnostics();
void connectToWifi();
void checkWifiResetRequest();
void waitForWifiResetRelease();
void forgetWifiAndReconfigure();
void initTimeClock();
void readDhtSensor();
void initSpotify();
void ensureSpotifyClient();
void updateSpotifyState();
void refreshPageData();
void handleButtonPress(size_t index);
void handleLongButtonPress(size_t index);
void refreshNetworkLink();
void copyText(char *destination, size_t size, const char *source);
void selectAlbumArtUrl(JsonVariant item);
void appendText(char *destination, size_t size, const char *source);
void resetPomodoroPhase(bool breakPhase);
void startPomodoroTimer();
void pausePomodoroTimer();
void resetPomodoroTimer();
void skipPomodoroBreak();
unsigned long pomodoroRemainingSeconds();
void markDisplayDirty();
void handleClockButtonPress(size_t index);
void handleDhtButtonPress(size_t index);
void handleSpotifyButtonPress(size_t index);
void handlePomodoroButtonPress(size_t index);

// ---------------------------------------------------------------------------
// DICHIARAZIONI - AlbumArt.ino
// ---------------------------------------------------------------------------
void artRelease();
bool artHasImage();
bool artHoldsUrl(const char *url);
size_t artBudget();
bool artFetch(const char *url);
bool artBlockOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);
void artSetupDecoder();
bool artDraw(int16_t x, int16_t y);
void artDrawPlaceholder(int16_t x, int16_t y, bool pending);
void artRender(int16_t x, int16_t y);
void refreshAlbumArt();
void drawWeatherIcon(int16_t cx, int16_t cy, int16_t size, int16_t code);

// ---------------------------------------------------------------------------
// DICHIARAZIONI - Weather.ino
// ---------------------------------------------------------------------------
void loadWeatherSettings();
void setWeatherCity(const char *name);
void printWeatherStatus();
void addWeatherPortalParams(WiFiManager &wifiManager);
void storePortalParams();
bool resolveWeatherCity();
void refreshWeatherData();
bool weatherIsStale();
const char *weatherConditionName(int16_t code);

// ---------------------------------------------------------------------------
// DICHIARAZIONI - Console.ino
// ---------------------------------------------------------------------------
void pollSerialConsole();

// ---------------------------------------------------------------------------
// DICHIARAZIONI - ScreenViews.ino
// ---------------------------------------------------------------------------
const ScreenView &currentScreenView();
bool networkOnline();
void formatShortClock(char *out, size_t size);
bool pomodoroIsCompleted();
void renderPage();
void renderClockTick();
void renderPomodoroTick();
void invalidateScreenCaches();
void drawClockPage();
void drawDhtPage();
void drawSpotifyPage();
void drawPomodoroPage();

// ---------------------------------------------------------------------------
// DICHIARAZIONI - SpotifyLink.ino
// ---------------------------------------------------------------------------
void loadSpotifyCredentials();
void saveSpotifyRefreshToken(const char *token);
void clearSpotifyRefreshToken();
void addSpotifyPortalParams(WiFiManager &wifiManager);
void storeSpotifyPortalParams();
void openSetupPortal();
void newSpotifyLoginState();
void serviceSpotifyLogin();
void restartSpotify();
void armSpotifyUnlink();
void disarmSpotifyUnlink();
void unlinkSpotifyAccount();
void refreshSpotifyUnlinkArming();

// ---------------------------------------------------------------------------
// "MAIN" APPLICATIVO (ORCHESTRAZIONE)
// ---------------------------------------------------------------------------
void initializeHardware() {
  Serial.begin(115200);
  delay(200);
  setupButtons();
  setupDisplay();
  dht.begin();
  artSetupDecoder();

  // Schermata 01 del design: il dispositivo si presenta mentre i servizi di
  // rete partono. Resta sotto gli occhi per tutto il tempo dell'aggancio WiFi,
  // che e' la parte lenta dell'avvio.
  drawBootScreen("connessione alla rete", "", true, 0.15f, "ATTENDERE");
}

void initializeServices() {
  connectToWifi();
  if (WiFi.status() == WL_CONNECTED) {
    initTimeClock();
    ensureSpotifyClient();
    if (spotifyState.authenticated) {
      updateSpotifyState();
    }
  }

  readDhtSensor();
  resetPomodoroPhase(false);
}

void initializeApplication() {
  initializeHardware();

  // Citta' e coordinate del meteo: si leggono subito, cosi' la pagina del
  // sensore ha un nome da mostrare anche prima che la rete sia su. Le
  // rilegge anche il portale quando precompila il campo, ed e' un'operazione
  // senza effetti collaterali: farla due volte non costa niente.
  loadWeatherSettings();

  // Va interrogato prima dei servizi: decide se connectToWifi() debba
  // riaprire il portale invece di riusare la rete salvata.
  checkWifiResetRequest();

  initializeServices();

  Serial.println("Sistema avviato");
  Serial.printf("Display ST7789 attivo: %d x %d\n", tft.width(), tft.height());
  Serial.println("Pagine: Clock -> DHT22 -> Spotify -> Pomodoro");
  Serial.println("Nell'orologio: K2 risincronizza NTP, K3 riconnette, K4 dimentica rete");
  Serial.println("Nel player: K2 precedente, K3 play/pausa, K4 successiva");
  Serial.println("Nel player: K4 tenuto premuto scollega l'account Spotify");
  Serial.println("Nel Pomodoro: K3 avvia timer, K4 reset");
  Serial.println("Navigazione pagine: K1 avanza, K1 tenuto premuto torna all'orologio");
  Serial.println("All'accensione: K4 tenuto premuto 3s cancella la rete WiFi salvata");

  // Il tasto tenuto premuto per il reset WiFi, e ogni disturbo raccolto
  // durante il boot, hanno gia' accodato pressioni: vanno buttate via prima
  // di mostrare la prima pagina, o verrebbero servite subito.
  clearButtonQueue();

  // Primo avvio, o account appena scollegato: invece dell'orologio si apre
  // la pagina Spotify, che in questo stato mostra il QR code del login (o la
  // richiesta di configurare l'app). E' il punto in cui chi ha appena
  // acceso il dispositivo deve mettere mano, quindi gli si va incontro.
  if (WiFi.status() == WL_CONNECTED &&
      (spotifyState.awaitingLogin || spotifyState.appKeysMissing)) {
    currentPage = PAGE_SPOTIFY;
  }

  markDisplayDirty();
}

void handleInputs() {
  // Prima di servire le code: se un fronte si e' perso, la pressione viene
  // recuperata adesso e finisce in coda in tempo per essere servita in questo
  // stesso giro.
  resyncButtonStates();

  for (size_t i = 0; i < BUTTON_COUNT; ++i) {
    // Le due code sono separate e si servono entrambe: una raffica veloce di
    // pressioni brevi e una lunga non si scavalcano a vicenda.
    if (takeLongPress(buttons[i])) {
      handleLongButtonPress(i);
    }
    if (takeShortPress(buttons[i])) {
      handleButtonPress(i);
    }
  }
}

void runBackgroundTasks() {
  refreshPageData();
}

void renderIfNeeded() {
  if (displayDirty) {
    renderPage();
    return;
  }

  // Chiamate a ogni giro: escono subito se non sono la pagina attiva, e anche
  // quando lo sono non toccano il pannello finche' il valore non cambia.
  renderClockTick();
  renderPomodoroTick();
}

void runMain() {
  handleInputs();
  runBackgroundTasks();
  renderIfNeeded();
}

void setup() {
  initializeApplication();
}

void loop() {
  runMain();

  delay(LOOP_DELAY_MS);
}
