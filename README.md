# SpotifyThing

Stazione da scrivania su ESP32 con display ST7789 montato in orizzontale
(320x240): orologio sincronizzato via NTP, sensore ambientale DHT22, controllo
del player Spotify e timer Pomodoro. Quattro pulsanti per navigare fra le
schermate e comandare la pagina attiva.

## Hardware

- Scheda ESP32 DevKit
- Display TFT ST7789 240x320, interfaccia SPI
- Sensore di temperatura e umidita' DHT22 (AM2302)
- Pulsantiera a 4 tasti con GND comune

### Cablaggio

Il pinout completo, con le note sui pin di strapping, sta in
[`Config.h`](Config.h). Riassunto:

**Display ST7789** (SPI hardware sul bus HSPI)

| Pin modulo | ESP32 |
|---|---|
| GND | GND |
| VCC | 3.3V |
| SCL / SCK | GPIO 15 |
| SDA / MOSI | GPIO 13 |
| RES / RST | GPIO 22 |
| DC / RS | GPIO 21 |
| CS | GPIO 2 |
| BLK / LED | 3.3V |
| MISO / SDO | **non collegato** |

Il MISO va lasciato staccato: il pannello e' di sola scrittura e quel filo
andrebbe sul GPIO 12, pin di strapping che se trovato alto al boot impedisce
l'avvio dell'ESP32.

**DHT22**

| Pin | ESP32 |
|---|---|
| VCC | 3.3V |
| DATA | GPIO 27 |
| GND | GND |

Serve un pull-up da 10 kOhm fra DATA e VCC. I breakout a 3 pin lo integrano.

**Pulsanti** (attivi a massa, `INPUT_PULLUP`, nessuna resistenza esterna)

| Tasto | ESP32 | Ruolo |
|---|---|---|
| K1 | GPIO 25 | cambio pagina |
| K2 | GPIO 26 | contestuale |
| K3 | GPIO 32 | contestuale |
| K4 | GPIO 33 | contestuale |

## Librerie

Da installare dal Library Manager dell'IDE Arduino:

- Adafruit ST7735 and ST7789 Library
- Adafruit GFX Library
- DHT sensor library
- WiFiManager
- SpotifyEsp32 **5.x** (le versioni precedenti hanno altri nomi dei metodi)
- ArduinoJson 7.x

Piu' il core **esp32 by Espressif Systems**, scheda `ESP32 Dev Module`.

## Configurazione

Il dispositivo si configura tutto da telefono: chi lo riceve non deve aprire
il codice ne' il Serial Monitor.

### 1. L'app Spotify

Spotify fa collegare un account solo attraverso un'"app" registrata su
[developer.spotify.com](https://developer.spotify.com/dashboard). Due strade:

- **App del proprietario del firmware.** Copia `Secrets.h.example` in
  `Secrets.h` e scrivici Client ID e Client Secret: diventano i valori di
  fabbrica. Ogni persona che userà il dispositivo va aggiunta, con l'email
  del suo account Spotify, in *User Management* della tua app: in Development
  mode Spotify accetta solo gli account in quell'elenco.
- **App di chi usa il dispositivo.** Ognuno crea la propria app (5 minuti,
  nessun codice) e ne scrive Client ID e Secret nel portale di configurazione
  (vedi sotto). Nessun limite di utenti e nessun lavoro per il proprietario.

In entrambi i casi l'app deve avere fra i *Redirect URI* esattamente:

```
https://spesp32.finianlandes.workers.dev/api/spotify/callback
```

`Secrets.h` e' facoltativo: senza, lo sketch compila lo stesso e l'app si
configura solo dal portale. E' escluso dal versionamento.

### 2. Prima accensione

1. Il dispositivo crea la rete `SpotifyThing-Setup`: collegati da telefono,
   apri `192.168.4.1` e scegli la tua rete WiFi. In fondo alla pagina ci sono
   i campi facoltativi *Spotify Client ID* e *Spotify Client Secret*. Tutto
   viene salvato nella NVS dell'ESP32, non nel codice.
2. Collegato il WiFi, il display apre da solo la pagina Spotify con un
   **QR code**: inquadralo con il telefono, accedi a Spotify e premi
   *Accetto*. Dopo qualche secondo il display si collega da solo.
3. L'account resta salvato anche dopo un riavvio.

Se sulla pagina compare **CONFIGURA APP**, nessuna app e' stata impostata:
premi K3 per riaprire il portale. Lì, alla voce *Setup*, si cambiano Client ID
e Secret senza toccare la rete; appena salvati il portale si chiude.

### Scollegare l'account

Serve quando il dispositivo passa a un'altra persona: sulla pagina Spotify
tieni premuto K4, poi premi ancora K4 entro 5 secondi. Il display torna al QR
code. Il token viene cancellato dal dispositivo; per revocarlo anche lato
Spotify si toglie l'app da [spotify.com/account/apps](https://www.spotify.com/account/apps/).

### Come funziona il login

Spotify accetta come indirizzo di ritorno solo pagine HTTPS, che l'ESP32 non
puo' servire. Il ritorno arriva quindi a un piccolo server pubblico (lo stesso
usato dalla libreria SpotifyEsp32 5.x), che conserva il codice monouso del login;
il dispositivo lo ritira e lo scambia direttamente con Spotify. Dal server
passa solo quel codice, inutile senza il Client Secret. Per non dipendere da
un servizio di terzi se ne puo' ospitare una copia e cambiare
`SPOTIFY_AUTH_HOST` e `SPOTIFY_REDIRECT_URI` in `Config.h`.

### Cambiare rete WiFi

- **A freddo:** tieni premuto K4 per 3 secondi all'accensione. Funziona anche
  quando il dispositivo non riesce piu' a collegarsi.
- **A caldo:** K4 sulla pagina orologio, due volte entro 5 secondi.

## Comandi

| Pagina | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| Orologio | pagina succ. | risincronizza NTP | riconnetti WiFi | dimentica rete |
| DHT22 | pagina succ. | aggiorna / riprova | — (storico, non implementato) | chiudi |
| Spotify, con brano | pagina succ. | precedente | pausa / riprendi | successiva (tenuto: scollega) |
| Spotify, senza brano | pagina succ. | — | ricerca sorgente | scollega account |
| Spotify, QR del login | pagina succ. | — | configura app (portale) | — |
| Pomodoro | pagina succ. | chiudi | avvia / sospendi | reset |
| Pomodoro concluso | pagina succ. | salta pausa | avvia pausa | reset |

Le etichette sulla barra dei tasti cambiano insieme allo stato della pagina,
quindi quello che c'e' scritto a video e' sempre quello che il tasto fa davvero.
Un tasto disegnato spento non fa nulla.

**K1 tenuto premuto** riporta all'orologio da qualsiasi pagina. **K4 tenuto
premuto** sulla pagina Spotify chiede di scollegare l'account. Sugli altri
tasti una pressione lunga vale come una pressione normale: tenere premuto per
esitazione non lascia mai il tasto senza risposta.

Le pressioni vengono riconosciute al rilascio, non alla pressione: e' cio' che
permette di misurarne la durata. Il ritardo e' di pochi centesimi di secondo e
non si avverte.

## Schermate

L'interfaccia segue il design *Schermate ST7789*: barra di stato in alto,
cornice del contenuto con le crocette agli angoli, barra dei quattro tasti in
basso. Le quattro pagine cambiano faccia a seconda dello stato, per undici
schermate in tutto piu' quella di avvio.

| Pagina | Stati |
|---|---|
| Orologio | in linea; rete assente (avviso ambra, ora dall'ultimo aggancio NTP) |
| DHT22 | lettura valida; sensore che non risponde (valori a tratteggio) |
| Spotify | in riproduzione con barra di avanzamento; in pausa; nessuna sorgente; errore HTTP; QR code del login; app da configurare; conferma scollegamento |
| Pomodoro | pronto; focus in corso; pomodoro concluso con la pausa da avviare |

La fine di una fase del Pomodoro non ne avvia un'altra da sola: il timer si
ferma sulla schermata del pomodoro concluso e aspetta che la pausa venga
avviata a mano.

## Struttura del progetto

I file `.ino` di uno sketch Arduino vengono concatenati in un unico sorgente,
quindi devono stare tutti nella stessa cartella, che deve chiamarsi come lo
sketch principale.

| File | Contenuto |
|---|---|
| `SpotifyThing.ino` | stato globale e orchestrazione di `setup()` / `loop()` |
| `Config.h` | pinout, cablaggi, temporizzazioni, parametri di rete |
| `Secrets.h` | Client ID e Secret Spotify di fabbrica (facoltativo, non versionato) |
| `Theme.h` | palette e geometria dell'interfaccia |
| `Display.ino` | driver del pannello e primitive di disegno |
| `AppLogic.ino` | input, rete, sensori, logica delle funzioni |
| `ScreenViews.ino` | impaginazione delle schermate |
| `SpotifyLink.ino` | collegamento dell'account: QR code, portale, NVS |

### Note di progetto

**Rendering a celle.** Il font integrato e' a larghezza fissa, quindi orologio
e timer trattano le stringhe come file di celle indipendenti e ridipingono solo
quelle il cui carattere e' cambiato. Un secondo che scorre costa 24x32 pixel
invece di 192x32.

**Pulsanti a interrupt.** Le pressioni vengono catturate dalla ISR e messe in
coda, non campionate a polling: una pressione fatta mentre il loop e' fermo in
una chiamata di rete bloccante viene ritardata, non persa. L'antirimbalzo e' a
finestra temporale dentro la ISR.

**Spotify interrogato solo quando serve.** La richiesta HTTPS e' sincrona e puo'
bloccare il loop per centinaia di millisecondi: viene eseguita soltanto mentre
la pagina Spotify e' visibile, per non far saltare i secondi altrove.

## Diagnostica pulsanti

In `Config.h` imposta `BUTTON_DIAGNOSTICS = true` e apri il Serial Monitor a
115200 baud. Premendo un tasto una volta sola:

| Output | Significato |
|---|---|
| `fronti=2 coda=1` | contatto pulito |
| `fronti=30 coda=1` | rimbalzo forte, filtrato correttamente |
| `fronti=30 coda=3` | rimbalzo oltre la finestra: alzare `BUTTON_DEBOUNCE_US` |
| `fronti=0 coda=0` | il segnale non arriva al pin: problema elettrico |
