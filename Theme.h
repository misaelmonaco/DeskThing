#pragma once
#include <Arduino.h>
#include "Config.h"

// ===========================================================================
// TEMA GRAFICO E GEOMETRIA DELL'INTERFACCIA
// ---------------------------------------------------------------------------
// Palette e misure vengono dal design "Schermate ST7789", impaginato su
// artboard da 320x240: pannello in orizzontale, una barra di stato sottile in
// alto, una cornice di contenuto con le crocette agli angoli, e una barra dei
// tasti divisa in quattro celle.
//
// Tutte le coordinate delle pagine derivano da queste costanti: cambiando
// risoluzione, rotazione o margini l'intera interfaccia si riadatta senza
// inseguire numeri sparsi nel codice di disegno.
// ===========================================================================

// Converte una terna RGB a 8 bit nel formato RGB565 usato dal pannello.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------------------------------------------------------------------------
// PALETTE - rampa "accent" del design system industry + ambra di segnalazione
// ---------------------------------------------------------------------------
// La rampa e' fredda e desaturata: il blu porta l'informazione normale,
// l'ambra tutto cio' che chiede attenzione (rete caduta, sensore muto, player
// in pausa, fase di lavoro del Pomodoro). Non ci sono altri colori: uno stato
// anomalo si riconosce perche' e' l'unica cosa calda sullo schermo.
constexpr uint16_t COL_BG        = rgb565(  7,   9,  11);  // fondo pagina
constexpr uint16_t COL_INK       = rgb565(238, 246, 255);  // accent-100, testo pieno
constexpr uint16_t COL_ACC_300   = rgb565(181, 217, 253);
constexpr uint16_t COL_ACC_400   = rgb565(148, 188, 227);  // testo dei tasti attivi
constexpr uint16_t COL_ACC_500   = rgb565(116, 157, 196);  // testo secondario
constexpr uint16_t COL_ACC_600   = rgb565( 89, 126, 163);  // etichette di sezione
constexpr uint16_t COL_ACC_700   = rgb565( 65,  97, 128);  // crocette, valori spenti
constexpr uint16_t COL_ACC_800   = rgb565( 44,  69,  93);  // cornici e divisori
constexpr uint16_t COL_AMBER     = rgb565(224, 162,  74);  // stato che chiede attenzione
constexpr uint16_t COL_TRACK     = rgb565( 22,  34,  46);  // fondo delle barre blu
constexpr uint16_t COL_TRACK_WARM= rgb565( 42,  35,  26);  // fondo della barra ambra
constexpr uint16_t COL_WARN_BG   = rgb565( 42,  31,  14);  // fascia di avviso
constexpr uint16_t COL_KEY_AMBER = rgb565( 58,  42,  18);  // cella tasto evidenziata
constexpr uint16_t COL_KEY_BLUE  = rgb565( 18,  36,  54);  // cella tasto attiva
constexpr uint16_t COL_DONE_BG   = rgb565( 21,  15,   6);  // riquadro pomodoro concluso
constexpr uint16_t COL_DONE_EDGE = rgb565(106,  79,  31);
constexpr uint16_t COL_SIGNAL_OFF= rgb565( 74,  60,  34);  // tacche del segnale assente

// ---------------------------------------------------------------------------
// METRICHE DEL FONT INTEGRATO ADAFRUIT_GFX
// ---------------------------------------------------------------------------
// Il font di default occupa una cella di 6x8 pixel per ogni unita' di
// setTextSize(): a size 2 un carattere misura 12x16 pixel, e cosi' via.
//
// Il design usa Barlow Condensed e JetBrains Mono, che non esistono a bordo:
// le dimensioni sono state ricondotte al multiplo del font 6x8 piu' vicino
// (62-66 px -> size 8, 40 px -> size 5, 30 px -> size 4, 10 px -> size 1).
// Le geometrie - margini, altezze delle fasce, spessori - restano invece
// identiche al disegno, che e' cio' che da' alle pagine la loro impronta.
constexpr int16_t GFX_CELL_W = 6;
constexpr int16_t GFX_CELL_H = 8;

// ---------------------------------------------------------------------------
// GEOMETRIA DELLE FASCE
// ---------------------------------------------------------------------------
// In orizzontale gli assi del pannello sono scambiati: 320 di larghezza per
// 240 di altezza.
constexpr int16_t SCREEN_W = TFT_LANDSCAPE ? TFT_HEIGHT_PX : TFT_WIDTH_PX;
constexpr int16_t SCREEN_H = TFT_LANDSCAPE ? TFT_WIDTH_PX : TFT_HEIGHT_PX;

constexpr int16_t MARGIN_X = 10;

// Barra di stato: etichetta di pagina a sinistra, tacche del segnale e
// indicatore "n/4" a destra. Non ha fondo proprio, vive sul nero della pagina.
constexpr int16_t STATUS_Y = 7;
constexpr int16_t STATUS_H = 10;

// Barra dei tasti: quattro celle di uguale larghezza separate da divisori
// verticali, sormontate da una riga di separazione.
constexpr int16_t FOOTER_H = 34;
constexpr int16_t FOOTER_Y = SCREEN_H - FOOTER_H;
constexpr int16_t KEY_COUNT = 4;
constexpr int16_t KEY_W = SCREEN_W / KEY_COUNT;

// Cornice del contenuto: il rettangolo che occupa tutto lo spazio fra la barra
// di stato e quella dei tasti, con le crocette agli angoli.
constexpr int16_t FRAME_X = MARGIN_X;
constexpr int16_t FRAME_Y = STATUS_Y + STATUS_H + 7;
constexpr int16_t FRAME_W = SCREEN_W - 2 * MARGIN_X;
constexpr int16_t FRAME_H = FOOTER_Y - 6 - FRAME_Y;

// Area utile interna alla cornice, gia' scontato il bordo da 1 px.
constexpr int16_t CONTENT_X = FRAME_X + 1;
constexpr int16_t CONTENT_Y = FRAME_Y + 1;
constexpr int16_t CONTENT_W = FRAME_W - 2;
constexpr int16_t CONTENT_H = FRAME_H - 2;

// Fascia di avviso ambra sopra la barra dei tasti (rete assente, sensore muto).
// Quando c'e', la cornice si accorcia per farle posto.
constexpr int16_t NOTICE_H = 18;
constexpr int16_t NOTICE_Y = FOOTER_Y - 6 - NOTICE_H;
constexpr int16_t FRAME_H_WITH_NOTICE = NOTICE_Y - 6 - FRAME_Y;

// ---------------------------------------------------------------------------
// COPERTINA E COLONNA DI TESTO DELLA PAGINA PLAYER
// ---------------------------------------------------------------------------
// Copertina a sinistra, centrata in altezza nella cornice; testo a destra.
// La colonna di testo che ne risulta e' larga 200 px, cioe' sedici caratteri
// per riga a dimensione 2: uno in piu' di quanti ne teneva il titolo a
// dimensione 3 su tutta la larghezza, prima della copertina. Si perde in
// altezza dei caratteri, non in quantita' di testo: e' questo che rende
// accettabile cedere 64 px di larghezza all'immagine.
constexpr int16_t ART_X      = FRAME_X + 12;
constexpr int16_t ART_Y      = CONTENT_Y + (CONTENT_H - ART_SIZE) / 2;
constexpr int16_t ART_TEXT_X = ART_X + ART_SIZE + 12;
constexpr int16_t ART_TEXT_W = FRAME_X + FRAME_W - 12 - ART_TEXT_X;

// ---------------------------------------------------------------------------
// HELPER DI IMPAGINAZIONE
// ---------------------------------------------------------------------------
// Larghezza in pixel di una stringa di `chars` caratteri a una data size.
constexpr int16_t textWidthPx(uint8_t size, int16_t chars) {
  return static_cast<int16_t>(chars * GFX_CELL_W * size);
}

// Altezza in pixel di una riga a una data size.
constexpr int16_t textHeightPx(uint8_t size) {
  return static_cast<int16_t>(GFX_CELL_H * size);
}

// Quanti caratteri entrano nella cornice a una data size.
constexpr int16_t maxCharsForSize(uint8_t size) {
  return static_cast<int16_t>(CONTENT_W / (GFX_CELL_W * size));
}

// Ascissa che centra orizzontalmente una stringa sullo schermo.
constexpr int16_t centeredX(uint8_t size, int16_t chars) {
  return static_cast<int16_t>((SCREEN_W - textWidthPx(size, chars)) / 2);
}

// Ordinata che centra verticalmente una riga dentro una fascia.
constexpr int16_t centeredY(int16_t bandY, int16_t bandH, uint8_t size) {
  return static_cast<int16_t>(bandY + (bandH - textHeightPx(size)) / 2);
}

// ---------------------------------------------------------------------------
// STILI DELLE CELLE DELLA BARRA DEI TASTI
// ---------------------------------------------------------------------------
// Il design distingue quattro condizioni: tasto normale, tasto evidenziato
// perche' e' l'azione principale della schermata (fondo ambra o blu), e tasto
// spento perche' in quello stato non fa nulla.
enum KeyStyle : uint8_t {
  KEY_IDLE = 0,     // disponibile, testo blu chiaro
  KEY_PRIMARY,      // azione principale, fondo ambra
  KEY_ACTIVE,       // azione in corso, fondo blu
  KEY_DISABLED      // nessuna azione in questo stato
};

// Una cella della barra dei tasti. `glyph` sceglie il pittogramma disegnato
// sopra l'etichetta (vedi drawKeyGlyph in Display.ino).
enum KeyGlyph : uint8_t {
  GLYPH_NONE = 0,
  GLYPH_PAGE,       // due riquadri sovrapposti: cambio pagina
  GLYPH_CLOCK,      // quadrante con lancette: sincronia NTP
  GLYPH_WIFI,       // tre archi: rete
  GLYPH_CROSS,      // X: dimentica / chiudi
  GLYPH_REFRESH,    // freccia circolare: aggiorna / riprova
  GLYPH_CHART,      // spezzata: storico
  GLYPH_PREV,       // barra e triangolo a sinistra
  GLYPH_NEXT,       // triangolo e barra a destra
  GLYPH_PLAY,       // triangolo pieno
  GLYPH_PAUSE,      // due barre verticali
  GLYPH_SEARCH,     // lente
  GLYPH_RESET,      // freccia indietro
  GLYPH_SKIP        // doppio triangolo
};

struct KeyCell {
  const char *label;
  KeyGlyph glyph;
  KeyStyle style;
};

// Le quattro celle di una schermata, nell'ordine K1..K4.
struct KeyBar {
  KeyCell keys[KEY_COUNT];
};
