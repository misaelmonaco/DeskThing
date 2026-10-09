// ===========================================================================
// CONSOLE DI SERVIZIO SUL SERIALE
// ===========================================================================
// Quattro comandi scritti nel monitor seriale a 115200. Servono a provare le
// cose senza passare dal portale WiFi, che per cambiare una citta' costa
// collegarsi a un access point col telefono.
//
//   ?                elenco dei comandi
//   meteo            stato del meteo, passaggio per passaggio
//   meteo <citta>    imposta la citta' e riparte dalla geocodifica
//   aggiorna         forza subito un'interrogazione, senza aspettare i 15 min
//
// Non e' un'interfaccia: e' un attrezzo da banco. Per questo i comandi sono
// in chiaro e senza conferme - chi ci arriva ha gia' il cavo in mano.
//
// La lettura non blocca: si prende quello che c'e' nel buffer a ogni giro di
// loop e si agisce solo sulla riga completa. Bloccare qui vorrebbe dire
// fermare il ridisegno e la lettura dei pulsanti mentre qualcuno digita.
// ===========================================================================

#include <strings.h>

// Una riga di comando piu' il margine per il nome piu' lungo.
static char consoleLine[WEATHER_CITY_MAX + 24];
static size_t consoleLen = 0;
static bool consoleOverflow = false;

static void consoleHelp() {
  Serial.println("--- comandi ---");
  Serial.println("  ?              questo elenco");
  Serial.println("  meteo          stato del meteo");
  Serial.println("  meteo <citta>  imposta la citta' (es: meteo Ariano Irpino)");
  Serial.println("  aggiorna       forza subito un'interrogazione del meteo");
}

// Toglie spazi e tabulazioni in testa e in coda, in posto.
static char *consoleTrim(char *s) {
  while (*s == ' ' || *s == '\t') {
    ++s;
  }

  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
    s[--n] = '\0';
  }

  return s;
}

static void consoleExecute(char *raw) {
  char *line = consoleTrim(raw);

  if (line[0] == '\0') {
    return;
  }

  if (strcmp(line, "?") == 0 || strcasecmp(line, "aiuto") == 0 ||
      strcasecmp(line, "help") == 0) {
    consoleHelp();
    return;
  }

  if (strcasecmp(line, "aggiorna") == 0) {
    // Azzerare lastTryMs basta: la passata successiva non trova piu' nessuna
    // scadenza da rispettare e interroga subito.
    weather.lastTryMs = 0;
    Serial.println("[meteo] interrogazione forzata");
    return;
  }

  if (strncasecmp(line, "meteo", 5) == 0) {
    char *arg = consoleTrim(line + 5);

    if (arg[0] == '\0') {
      printWeatherStatus();
      return;
    }

    if (strlen(arg) >= WEATHER_CITY_MAX) {
      Serial.printf("[meteo] nome troppo lungo: il massimo e' %u caratteri\n",
                    static_cast<unsigned>(WEATHER_CITY_MAX - 1));
      return;
    }

    setWeatherCity(arg);
    Serial.printf("[meteo] citta' impostata dal seriale: %s\n", weather.city);
    Serial.println("[meteo] la geocodifica parte al prossimo giro");
    return;
  }

  Serial.printf("comando sconosciuto: \"%s\"   (? per l'elenco)\n", line);
}

void pollSerialConsole() {
  while (Serial.available() > 0) {
    int c = Serial.read();

    if (c == '\r') {
      continue;          // i monitor mandano CRLF, LF o solo CR
    }

    if (c == '\n') {
      if (consoleOverflow) {
        Serial.println("riga troppo lunga, ignorata");
      } else {
        consoleLine[consoleLen] = '\0';
        consoleExecute(consoleLine);
      }

      consoleLen = 0;
      consoleOverflow = false;
      continue;
    }

    // Oltre il buffer si segna l'eccesso e si continua a consumare fino a
    // fine riga: scartare i caratteri in silenzio farebbe eseguire un comando
    // troncato, che e' peggio di non eseguirne nessuno.
    if (consoleLen + 1 < sizeof(consoleLine)) {
      consoleLine[consoleLen++] = static_cast<char>(c);
    } else {
      consoleOverflow = true;
    }
  }
}
