# CDC Badge OS - UI System Deep Analysis

## Executive Summary

Das UI-System des CDC Badge OS ist eine **schichtbasierte Architektur** mit einer View-Stack-Navigation, modularer Struktur und Unterstützung für Modals, Exklusive Locks und Inaktivitäts-Timeouts. Es verwendet **CP437-Kodierung** für Display-Text und unterstützt **Internationalisierung (i18n)** mit Englisch als Fallback und Deutsch als Overlay.

---

## 1. Architektur-Übersicht

### 1.1 Hauptschichten

```
┌─────────────────────────────────────────────┐
│          cdc_os_ui (OS-Level UI)            │
│  - AppUi (Hauptsteuerung)                  │
│  - LockScreenView (Sperre)                 │
│  - Settings, SleepManager                  │
│  - WifiMenu, BluetoothMenu, ExpertMenu     │
└─────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────┐
│         cdc_ui (Core UI Framework)          │
│  - IView (Interface)                       │
│  - ViewStack (Navigations-Stack)           │
│  - I18n (Internationalisierung)            │
└─────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────┐
│        cdc_views (Wiederverwendbare Views)  │
│  - ListView (Menüs)                        │
│  - T9InputView (Texteingabe)               │
│  - PinEntryView (PIN-Eingabe)              │
│  - ConfirmView, ToastView, InfoView        │
│  - SliderView, ContextMenuView             │
│  - QRCodeView, CanvasView                  │
└─────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────┐
│      cdc_hal (Hardware Abstraction)         │
│  - IDisplay (E-Paper Display)              │
│  - IKeypad (12-Tasten-Keypad)              │
│  - IPowerManager, ISleepController         │
└─────────────────────────────────────────────┘
```

### 1.2 View-Hierarchy

```
IView (Interface)
├── ViewBase (Basis-Klasse mit gemeinsamer Logik)
    ├── cdc_views (Reusable Views)
    │   ├── ListView
    │   ├── T9InputView
    │   ├── PinEntryView
    │   ├── ConfirmView
    │   ├── ToastView
    │   ├── InfoView
    │   ├── SliderView
    │   ├── ContextMenuView
    │   ├── QRCodeView
    │   ├── CanvasView
    │   └── WizardView (nicht gefunden)
    │
    └── cdc_os_ui (OS-Level Views)
        ├── LockScreenView
        ├── PinChangeView
        ├── BlePairingPromptView
        └── (Module-specific views)
```

---

## 2. Kern-Komponenten

### 2.1 IView Interface (`components/cdc_ui/include/cdc_ui/IView.h`)

**Zweck:** Abstrakte Basisklasse für alle Views.

**Wichtige Methoden:**

```cpp
// Lifecycle
virtual void onEnter(void* context = nullptr) = 0;  // Beim Pushen
virtual void onExit() = 0;                          // Beim Poppen
virtual void onResume() = 0;                        // Nach Pop eines Kindes

// Rendering
virtual void render(bool partial) = 0;              // Zeichnen
virtual bool needsRender() const = 0;               // Dirty-Check
virtual void markDirty() = 0;                       // View aktualisieren
virtual void clearDirty() = 0;                      // Nach Render

// Input
virtual InputResult onKey(char key) = 0;            // Tastendruck
virtual InputResult onLongPress(char key);          // Langdrücken
virtual void onTick(uint32_t nowMs);                // Periodischer Tick

// Footer
virtual const char* getFooterHint() const;          // Footer-Text

// Identity
virtual const char* getName() const = 0;            // Debug-Name
```

**InputResult Enum:**
- `CONSUMED` - Input wurde verarbeitet
- `IGNORED` - Input wurde ignoriert
- `REQUEST_POP` - View möchte vom Stack entfernt werden
- `REQUEST_PUSH` - View möchte eine Child-View hinzufügen

### 2.2 ViewStack (`components/cdc_ui/include/cdc_ui/ViewStack.h`)

**Zweck:** Singleton-Navigations-Stack für View-Verwaltung.

**Wichtige Eigenschaften:**
- **Maximale Tiefe:** 8 Views
- **Thread-Safe:** Rekursives Mutex für Multi-Task-Zugriff
- **Modal Support:** Einzelnes Modal über der aktuellen View
- **Exclusive Lock:** Für FIDO2-Prompts etc.

**Lifecycle-Methoden:**

| Methode | Beschreibung |
|---------|------------|
| `push(view, context)` | View auf Stack legen |
| `pop()` | Top-View entfernen |
| `replace(view)` | Top-View ersetzen |
| `popToRoot()` | Alle bis auf Root entfernen |
| `popToAnchor(anchor)` | Bis zur Anchor-View poppen |
| `popToDepth(depth)` | Bis zur Tiefe poppen |

**Modal-Methoden:**

| Methode | Beschreibung |
|---------|------------|
| `showModal(modal)` | Modal anzeigen |
| `hideModal()` | Modal verstecken |
| `hasModal()` | Check ob Modal aktiv |

**Input-Dispatch:**

```cpp
void dispatchKey(char key);           // Tastendruck verteilen
void dispatchLongPress(char key);     // Langdruck verteilen
void dispatchTick(uint32_t nowMs);    // Tick verteilen
```

**Render-Pipeline:**

```cpp
void render(bool synchronous = false); // View rendern und Display flush
bool needsRender() const;              // Prüfen ob Render nötig
```

**Exclusive Lock (für FIDO2-Prompts):**

```cpp
bool acquireExclusive(const void* owner);  // Exklusiven Lock holen
bool releaseExclusive(const void* owner);  // Lock freigeben
const void* exclusiveOwner() const;        // Aktuelle Owner
```

**Inactivity Timeout:**

```cpp
void setInactivityTimeout(Callback, uint32_t ms);  // Timeout setzen
void resetInactivityTimer();                        // Timer zurücksetzen
void checkInactivity(uint32_t nowMs);               // Prüfen (aufrufen im Loop)
```

### 2.3 I18n (`components/cdc_ui/include/cdc_ui/I18n.h`)

**Zweck:** Internationalisierung mit Englisch-Fallback und Overlay-Translations.

**Architektur:**

```
Englisch (in-code, rodata)
    ↓ lookup
Overlay (Deutsch, /plugins/i18n/lang_de.json, PSRAM)
```

**Wichtige Methoden:**

```cpp
// Singleton
static I18n& instance();

// Initialisierung
bool init();                          // Initialisieren
bool loadOverlay();                   // Overlay-Dateien scannen und laden

// Registrierung (von Modulen aufgerufen)
void registerEnglishTable(const I18nEntry* entries, std::size_t count);

// Translation lookup
const char* tr(const char* key) const;         // Haupt-Translation
const char* overlayTr(const char* key) const;  // Nur Overlay (kein Fallback)

// Sprache wechseln
bool setLanguageCode(const char* code);        // Sprache setzen (z.B. "de")
const std::string& getLanguageCode() const;    // Aktuelle Sprache

// Sprachen-Liste
const std::vector<OverlayLanguage>& availableOverlayLanguages() const;
const char* languageName(const char* code) const;  // Anzeigename

// Callback bei Sprachwechsel
void setOnLanguageChanged(LanguageChangedCallback cb);
```

**i18n Keys Convention:**
- `core.*` - Firmware-weite Strings
- `mod_<name>.*` - Modulspezifische Strings
- Format: snake_case, ASCII, keine Leerzeichen

**Beispiel-Entry:**

```cpp
constexpr I18nEntry kCoreStrings[] = {
    {"core.save", "Save"},
    {"core.cancel", "Cancel"},
    {"core.main_menu", "Main Menu"},
};
```

**Lade-Pipeline:**

1. Scan `/plugins/i18n/lang_*.json` Dateien
2. Parse JSON mit cJSON (PSRAM-Allokation)
3. Konvertiere UTF-8 → CP437
4. Speichere in PSRAM-blob mit binärer Suche
5. Sprache über NVS persistent speichern

---

## 3. Wiederverwendbare Views (cdc_views)

### 3.1 ListView (`components/cdc_views/include/cdc_views/ListView.h`)

**Zweck:** Scrollbares Auswahlmenü mit Icons.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `title_` | `const char*` | Titel-Text |
| `items_` | `const ListItem*` | Item-Array |
| `itemCount_` | `uint16_t` | Anzahl Items |
| `selection_` | `uint16_t` | Aktuelle Auswahl |
| `scrollPos_` | `uint16_t` | Scroll-Position |
| `onSelect_` | `SelectCallback` | Auswahl-Callback |
| `onMenu_` | `MenuCallback` | Kontextmenü-Callback |
| `editMutex_` | `SemaphoreHandle_t` | Optionaler Mutex für Thread-Sicherheit |

**Callbacks:**

```cpp
using SelectCallback = void(*)(uint16_t index, void* userData);
using MenuCallback = void(*)(uint16_t index, void* userData);
using ItemRenderCallback = bool(*)(Gdey029T94* gfx, const ListItem&, uint16_t, int x, int y, int w, int h, bool selected, void* userCtx);
```

**Keys:**
- `2` (UP) - Nach oben
- `8` (DOWN) - Nach unten
- `Y` - Auswählen (triggert onSelect)
- `N` - Zurück (REQUEST_POP)
- `3` - Kontextmenü (triggert onMenu)

**Wichtige Methoden:**

```cpp
void init(const char* title, const ListItem* items, uint16_t count);
void setOnSelect(SelectCallback callback);
void setOnMenu(MenuCallback callback);
void setEditMutex(SemaphoreHandle_t mutex);  // Thread-Sicherheit
void setItemRenderer(ItemRenderCallback cb); // Custom Renderer
void setHint(const char* hint);              // Footer-Override
void setEmptyText(const char* text);         // Platzhalter bei leerer Liste
void setSelection(uint16_t index);           // Auswahl setzen
void updateItem(uint16_t index);             // Ein Item neu zeichnen
void insertItem(uint16_t index);             // Item eingefügt (notify)
void removeItem(uint16_t index);             // Item entfernt (notify)
void repaintPartial();                       // Teilweise Neuzeichnung
```

**ListItem-Struktur:**

```cpp
struct ListItem {
    const char* label;          // Anzeigetext
    uint8_t icon = 0;           // Icon-Typ (0 = none)
    bool iconDisabled = false;  // Icon durchgestrichen
    void* userData = nullptr;   // User data für Callbacks
};
```

### 3.2 T9InputView (`components/cdc_views/include/cdc_views/T9InputView.h`)

**Zweck:** Multi-Tap Texteingabe (wie alte Handys).

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `titleBuf_` | `char[48]` | Titel-Text |
| `text_` | `char[MAX_TEXT_LEN]` | Eingabe-Buffer |
| `len_` | `uint16_t` | Aktuelle Länge |
| `maxLen_` | `uint16_t` | Maximale Länge |
| `lastKey_` | `char` | Letzter gedrückter Key |
| `charIndex_` | `uint8_t` | Zeichen-Index (für Multi-Tap) |
| `lastPressMs_` | `uint32_t` | Zeit des letzten Key-Press |
| `cursorActive_` | `bool` | Ist Cursor aktiv (Multi-Tap) |

**T9-Mapping-Tabelle:**

```cpp
static const char* t9_chars[] = {
    " 0",                                  // 0: Leerzeichen + 0
    ".?!,;:'\"()-_@#$%&*+=/\\<>[]{}|^~`1" + Sonderzeichen,  // 1
    "abcABC2" + Umlaute (äáâàåæÄÅÆçÇ),    // 2
    "defDEF3" + Akzente (éèêëÉ),          // 3
    "ghiGHI4" + Akzente (íìîï),           // 4
    "jklJKL5",                             // 5
    "mnoMNO6" + Umlaute (óòôöÖñÑ),        // 6
    "pqrsPQRS7" + ß,                      // 7
    "tuvTUV8" + Umlaute (üÜúùû),          // 8
    "wxyzWXYZ9" + ÿ,                      // 9
};
```

**Keys:**
- `0-9` - Zeichen-Eingabe (Multi-Tap)
- `Long-press 0-9` - Ziffer direkt einfügen
- `N` (kurz) - Backspace
- `N` (lang) - Alles löschen
- `Y` - Bestätigen (triggert onSave)

**Wichtige Methoden:**

```cpp
void init(const char* title, const char* initialText, uint16_t maxLen);
void setOnSave(SaveCallback callback);
void setPlaceholder(const char* placeholder);
void setHint(const char* hint);
void forceDigit(char key);           // Ziffer erzwingen
uint16_t appendRaw(const char* text); // Roh-Text anhängen (für Serial)
const char* getText() const;
uint16_t getLength() const;
```

**Timeout-Logik:**

```cpp
static constexpr uint32_t TIMEOUT_MS = 2000;  // 2 Sekunden bis Commit
```

**State Machine:**

```
Kein Text → Key drücken → Zeichen einfügen
             ↓
        Gleicher Key → Zeichen wechseln (Multi-Tap)
             ↓
        Neuer Key/Timeout → Zeichen commiten
```

### 3.3 PinEntryView (`components/cdc_views/include/cdc_views/PinEntryView.h`)

**Zweck:** Sichere PIN-Eingabe mit Maskierung und Lockout.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `buffer_[8]` | `char` | PIN-Buffer |
| `length_` | `uint8_t` | Aktuelle Länge |
| `maxLength_` | `uint8_t` | Maximale Länge |
| `minLength_` | `uint8_t` | Minimale Länge |
| `attempts_` | `uint8_t` | Versuchs-Zähler |
| `maxAttempts_` | `uint8_t` | Max. Versuche |
| `lockedOut_` | `bool` | Lockout-Status |
| `lockoutStartMs_` | `uint32_t` | Lockout-Zeitpunkt |

**Callbacks:**

```cpp
using VerifyCallback = bool(*)(const char* pin);   // PIN prüfen
using SuccessCallback = void(*)();                 // Erfolg
using CancelCallback = void(*)();                  // Abbruch
using FailureCallback = void(*)(bool lockedOut);   // Fehler
```

**Keys:**
- `0-9` - Ziffer hinzufügen
- `N` - Backspace / Abbruch (wenn leer)
- `Y` - PIN bestätigen

**Wichtige Methoden:**

```cpp
void init(const char* title, uint8_t maxPinLength, uint8_t maxAttempts);
void setOnVerify(VerifyCallback callback);
void setOnSuccess(SuccessCallback callback);
void setOnCancel(CancelCallback callback);
void setOnFailure(FailureCallback callback);
void setMinLength(uint8_t minLen);
void clear();
void resetAttempts();
bool isLockedOut() const;
uint32_t getLockoutRemaining() const;
```

### 3.4 ConfirmView (`components/cdc_views/include/cdc_views/ConfirmView.h`)

**Zweck:** Y/N Bestätigungsdialog.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `message_[96]` | `char` | Dialog-Nachricht |
| `icon_` | `Icon` | Icon-Typ (NONE, QUESTION, WARNING, ERROR) |
| `onConfirm_` | `ConfirmCallback` | Bestätigungs-Callback |
| `onCancel_` | `CancelCallback` | Abbruch-Callback |

**Callbacks:**

```cpp
using ConfirmCallback = void(*)(void* userData);
using CancelCallback = void(*)(void* userData);
```

**Keys:**
- `Y` - Bestätigen
- `N` - Abbrechen

**Wichtige Methoden:**

```cpp
void init(const char* message, Icon icon = Icon::QUESTION);
void setOnConfirm(ConfirmCallback callback, void* userData = nullptr);
void setOnCancel(CancelCallback callback, void* userData = nullptr);
```

**Hilfsfunktionen:**

```cpp
void showConfirm(...);   // Modal anzeigen
void askConfirm(...);    // Kurzform für "Are you sure?"
```

### 3.5 ToastView (`components/cdc_views/include/cdc_views/ToastView.h`)

**Zweck:** Temporäres Overlay mit automatischem Timeout.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `message_[64]` | `char` | Toast-Nachricht |
| `icon_` | `Icon` | Icon-Typ |
| `durationMs_` | `uint16_t` | Dauer (0 = bis Abbruch) |
| `startMs_` | `uint32_t` | Startzeitpunkt |
| `expired_` | `bool` | Abgelaufen? |
| `dismissible_` | `bool` | Durch Tastendruck schließbar? |

**Icons:**
- `NONE` - Kein Icon
- `SUCCESS` - Haken
- `ERROR` - X
- `INFO` - (i)
- `TASK` - Sanduhr
- `ALERT` - Warnung

**Wichtige Methoden:**

```cpp
void init(const char* message, Icon icon, uint16_t durationMs, bool dismissible);
bool isExpired() const;
```

**Hilfsfunktionen:**

```cpp
void showToast(...);
void showToastSuccess(...);
void showToastError(...);
void showToastInfo(...);
void showToastTask(...);      // Unendlich (bis Abbruch)
void showToastAlert(...);
void showToastAlertSticky(...); // Nicht schließbar
```

### 3.6 InfoView (`components/cdc_views/include/cdc_views/InfoView.h`)

**Zweck:** Scrollbarer Text-Display (Hilfe, About).

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `titleBuf_[64]` | `char` | Titel |
| `textBuf_` | `PsramUniquePtr<char>` | Text-Buffer (PSRAM) |
| `scrollLine_` | `uint16_t` | Scroll-Position |
| `totalLines_` | `uint16_t` | Gesamtzeilen |

**Wichtige Methoden:**

```cpp
void init(const char* title, const char* text);
void setHint(const char* hint);
void setYesNoCallbacks(YesNoCallback onYes, YesNoCallback onNo, void* userData);
```

**Keys:**
- `2` (UP) - Nach oben scrollen
- `8` (DOWN) - Nach unten scrollen
- `N` - Zurück

### 3.7 SliderView (`components/cdc_views/include/cdc_views/SliderView.h`)

**Zweck:** Wert-Einstellung mit visuellem Balken.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `title_` | `const char*` | Titel |
| `value_` | `uint16_t` | Aktueller Wert |
| `minValue_` | `uint16_t` | Minimum |
| `maxValue_` | `uint16_t` | Maximum |
| `step_` | `uint16_t` | Schrittweite |
| `displayOffset_` | `int16_t` | Anzeige-Offset |
| `repeatStartMs_` | `uint32_t` | Key-Repeat Start |

**Callbacks:**

```cpp
using SaveCallback = void(*)(uint16_t value);       // Speichern
using ChangeCallback = void(*)(uint16_t value);     // Live-Änderung
using StepCallback = uint16_t(*)(uint16_t currentValue, bool increasing); // Dynamische Schrittweite
```

**Keys:**
- `4` - Verringern (links)
- `6` - Erhöhen (rechts)
- `Y` - Speichern
- `N` - Abbrechen

**Wichtige Methoden:**

```cpp
void init(const char* title, uint16_t minVal, uint16_t maxVal, uint16_t initial, uint16_t step, const char* unit);
void setOnSave(SaveCallback callback);
void setOnChange(ChangeCallback callback);
void setStepCallback(StepCallback callback);
void setDisplayOffset(int16_t offset);
void setZeroLabel(const char* label);  // Spezial-Label für 0 (z.B. "Never")
```

### 3.8 ContextMenuView (`components/cdc_views/include/cdc_views/ContextMenuView.h`)

**Zweck:** Popup-Kontextmenü als Modal.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `title_` | `const char*` | Menü-Titel |
| `items_[8]` | `ContextMenuItem` | Menü-Items |
| `itemCount_` | `uint8_t` | Anzahl Items |
| `selection_` | `uint8_t` | Auswahl |
| `scrollPos_` | `uint8_t` | Scroll-Position |
| `lastActivityMs_` | `uint32_t` | Letzte Aktivität (für Timeout) |

**Struktur:**

```cpp
struct ContextMenuItem {
    const char* label;
    void (*callback)();
};
```

**Wichtige Methoden:**

```cpp
void init(const char* title, const ContextMenuItem* items, uint8_t count);
```

**Keys:**
- `2` (UP) - Nach oben
- `8` (DOWN) - Nach unten
- `Y` - Auswählen
- `N` - Schließen

**Auto-Dismiss:**

```cpp
static constexpr uint32_t kMenuTimeoutMs = 60000;  // 60 Sekunden
```

### 3.9 QRCodeView (`components/cdc_views/include/cdc_views/QRCodeView.h`)

**Zweck:** QR-Code Anzeige mit Titel/Subtitle.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `data_` | `const char*` | QR-Daten |
| `title_` | `const char*` | Titel |
| `subtitle_` | `const char*` | Untertitel |
| `qrModuleCount_` | `int` | QR-Größe (Module) |
| `qrScale_` | `int` | Skalierung |
| `qrOffsetX_`, `qrOffsetY_` | `int` | Position |

**Wichtige Methoden:**

```cpp
void init(const char* data, const char* title, const char* subtitle);
void calculateLayout();       // QR-Größe berechnen
void renderQrCode();          // QR zeichnen
void renderText();            // Text zeichnen
```

**Two-Pass Rendering:**
1. **Sizing Pass:** QR-Größe ermitteln (ohne Zeichnen)
2. **Render Pass:** QR bei optimaler Größe zeichnen

### 3.10 CanvasView (`components/cdc_views/include/cdc_views/CanvasView.h`)

**Zweck:** Programmierbarer Canvas für Plugins (WASM).

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `widgets_[8]` | `Widget` | Widget-Liste |
| `widgetCount_` | `uint8_t` | Widget-Anzahl |
| `focused_` | `uint32_t` | Fokussiertes Widget ID |
| `cmds_[96]` | `DrawCmd` | Zeichen-Befehle |
| `cmdCount_` | `uint16_t` | Befehls-Anzahl |
| `textArena_[2048]` | `char` | Text-Arena |
| `textArenaUsed_` | `uint16_t` | Arena-Verbrauch |

**Widget-Typen:**

```cpp
enum class WidgetType {
    None = 0,
    Slider = 1,
    Text = 2,
    Button = 3,
};
```

**Callbacks:**

```cpp
using KeyCallback = void(*)(char key, uint32_t focused_widget);
using WidgetCallback = void(*)(uint32_t widget_id, WidgetEvent event);
```

**Wichtige Methoden:**

```cpp
void init(const char* title);
void setKeyCallback(KeyCallback cb);
void setWidgetCallback(WidgetCallback cb);
void setFooter(const char* hint);
void setKeyRepeat(uint16_t initial_ms, uint16_t repeat_ms);
void getBodySize(uint16_t* w, uint16_t* h);

// Zeichen-Primitive
void clearBody();
void setTextSize(uint8_t size);
void setFontId(uint8_t font_id);
void drawText(int16_t x, int16_t y, const char* text);
void drawTextAligned(int16_t x, int16_t y, int16_t w, const char* text, uint8_t align);
void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, bool filled);
void drawHLine(int16_t x, int16_t y, int16_t w);
void drawVLine(int16_t x, int16_t y, int16_t h);
void commit(bool full_refresh);

// Widget-Verwaltung
bool addSlider(uint32_t id, int32_t min, int32_t max, int32_t initial, int32_t step);
bool addText(uint32_t id, uint16_t max_len, const char* initial);
bool addButton(uint32_t id);
bool removeWidget(uint32_t id);

// Widget-Zugriff
bool setValue(uint32_t id, int32_t value);
bool getValue(uint32_t id, int32_t* out);
bool setText(uint32_t id, const char* text);
int getText(uint32_t id, char* out, size_t cap);
bool setFocus(uint32_t id);
uint32_t getFocus() const;
```

---

## 4. OS-Level UI (cdc_os_ui)

### 4.1 AppUi (`components/cdc_os_ui/src/AppUi.cpp`)

**Zweck:** Haupt-Steuerung der UI mit Menü-Verwaltung.

**Wichtige Funktionen:**

```cpp
void ui_init(const UiDeps& deps);        // Initialisierung
void ui_process(uint32_t nowMs);         // Haupt-Loop (Tick)
void ui_on_modules_ready();              // Module fertig
void ui_rebuild_menus();                 // Menüs neu bauen
void prepareForBootloaderReset();        // Bootloader-Vorbereitung
```

**Menu-Struktur:**

```
Main Menu
├── Module Items (dynamisch)
├── Plugins
├── Tools
│   ├── WiFi
│   ├── Bluetooth
│   ├── Expert
│   └── Module Tools (dynamisch)
└── Settings
    ├── Brightness
    ├── Language
    ├── Timezone
    ├── Auto Sleep
    ├── Badge Text
    ├── Set Date
    ├── Set Time
    └── Change PIN
```

**Status-Icons (LockScreen):**

```cpp
enum class StatusIcon {
    NONE,
    LOCK,           // Padlock
    DEEP_SLEEP,     // zzZ
    LIGHT_SLEEP,    // z
    BACKLIGHT,      // Sun
    USB,            // USB connected
    BLE,            // Bluetooth
    WIFI,           // WiFi connected
    SAO,            // SAO detected
    CHARGING,       // Charging
    NO_BATTERY,     // No battery
    CAFFEINATED,    // Sleep inhibited
    BACKGROUND,     // Background plugin
};
```

### 4.2 LockScreenView (`components/cdc_os_ui/include/cdc_os_ui/views/LockScreenView.h`)

**Zweck:** Haupt-Sperre mit Uhrzeit, Datum und Status-Icons.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `name_[64]` | `char` | Anzeige-Name |
| `info_[64]` | `char` | Info-Zeile 1 |
| `info2_[64]` | `char` | Info-Zeile 2 |
| `clock_[8]` | `char` | Uhrzeit |
| `date_[12]` | `char` | Datum |
| `batteryPercent_` | `uint8_t` | Batterie-Prozent |
| `statusIcons_` | `StatusIcon` | Icons-Bitmaske |
| `nPressStartMs_` | `uint32_t` | N-Key-Start (Deep Sleep) |
| `deepSleepMode_` | `bool` | Deep-Sleep-Modus |

**Callbacks:**

```cpp
using UnlockCallback = void(*)();
using PreRenderCallback = void (*)();  // Vor jedem Render
```

**Keys:**
- `3` - Kontextmenü (Light, WiFi, Module-Items)
- **Beliebig** - Entsperren

**Deep Sleep Trigger:**
- **Lang-press N** (5 Sekunden) → Deep Sleep

**Wichtige Methoden:**

```cpp
void init();
void setOnUnlock(UnlockCallback callback);
void setDisplayName(const char* name);
void setInfo(const char* info);
void setInfo2(const char* info2);
void setClock(const char* clock);
void setDate(const char* date);
void setBatteryPercent(uint8_t percent);
void setStatusIcons(StatusIcon icons);
void addStatusIcon(StatusIcon icon);
void removeStatusIcon(StatusIcon icon);
void toggleBacklight();
void setPreRenderCallback(PreRenderCallback callback);
```

### 4.3 SleepManager (`components/cdc_os_ui/include/cdc_os_ui/SleepManager.h`)

**Zweck:** Light-Sleep-Verwaltung für Lock Screen.

**State-Variable:**

| Variable | Typ | Beschreibung |
|----------|-----|--------------|
| `sleep_` | `ISleepController*` | Sleep-Controller |
| `power_` | `IPowerManager*` | Power-Manager |
| `lockScreen_` | `LockScreenView*` | LockScreen-View |
| `lockScreenEnteredMs_` | `uint32_t` | Lock-Screen-Startzeit |
| `inLightSleep_` | `bool` | Im Light Sleep? |
| `inhibitors_[8]` | `const char*` | Sleep-Inhibitoren |
| `inhibitorCount_` | `uint8_t` | Inhibitor-Anzahl |

**Timeout:**

```cpp
static constexpr uint32_t LIGHT_SLEEP_TIMEOUT_MS = 120 * 1000;  // 2 Minuten
```

**Wichtige Methoden:**

```cpp
static SleepManager& instance();
void init(hal::ISleepController* sleep, hal::IPowerManager* power, LockScreenView* lockScreen);
void checkLockScreenSleep(uint32_t nowMs);   // Prüfen (aufrufen im Loop)
void resetTimer(uint32_t nowMs);             // Timer zurücksetzen
void resetTimer();                           // Mit aktueller Zeit
bool isSleepInhibited() const;
bool addSleepInhibitor(const char* reason);  // Inhibitor hinzufügen
bool removeSleepInhibitor(const char* reason); // Inhibitor entfernen
```

**Sleep-Inhibitoren:**
- Module können Sleep verhindern (z.B. Plugin mit `prevent_sleep`)
- USB-Verbindung verhindert Sleep automatisch
- Maximal 8 Inhibitoren

---

## 5. Render-Pipeline

### 5.1 Überblick

```
ViewStack::render()
    ↓
View::render(partial)
    ↓
hal::IDisplay::getNativeHandle() → Gdey029T94*
    ↓
gfx->fillScreen() / gfx->fillRect() / gfx->drawRect()
    ↓
gfx->setFont() / gfx->setTextSize() / gfx->setTextColor()
    ↓
render::printText(gfx, text)  // CP437-konform
    ↓
hal::IDisplay::flush(mode) / flushSync(mode)
    ↓
E-Paper Panel (Gdey029T94)
```

### 5.2 CP437 vs UTF-8

**WICHTIG:** Display verwendet **CP437**, nicht UTF-8!

```cpp
// FALSCH - UTF-8 direkt drucken
gfx->print("Größe");  // Korrupte Ausgabe!

// RICHTIG - CP437 über render::printText
render::printText(gfx, "Größe");  // Korrekt!

// ODER - Per Char zeichnen (für einzelne Zeichen)
gfx->print('G');
gfx->print('r');
gfx->print('ö');  // CP437-Byte (0x82)
```

**CP437-Mapping:**
- `ä` = `0x84`
- `ö` = `0x94`
- `ü` = `0x81`
- `Ä` = `0x8E`
- `Ö` = `0x99`
- `Ü` = `0x9A`
- `ß` = `0xE1`

**i18n-Pipeline:**

```
lang_de.json (UTF-8)
    ↓ parse
cdc::core::cp437::fromUtf8()
    ↓
CP437-String in PSRAM
    ↓
render::printText()
    ↓
Display (korrekte Glyphen)
```

### 5.3 Refresh-Modi

```cpp
enum class RefreshMode {
    PARTIAL,  // Nur geänderte Bereiche (schnell)
    FULL      // Ganzes Display (für Korrekturen)
};
```

**Auto-Refresh-Logik:**

```cpp
bool needsFullRefresh_ = true;  // Nach View-Wechsel
...
hal::RefreshMode mode = needsFullRefresh_ 
    ? hal::RefreshMode::FULL 
    : hal::RefreshMode::PARTIAL;
```

---

## 6. Navigation-Flow

### 6.1 Haupt-Navigations-Pfade

```
Boot → LockScreenView
    ↓ (any key)
PinEntryView
    ↓ (PIN korrekt)
ListView (Main Menu)
    ↓ (select)
    ├── Module View
    ├── PluginListView
    ├── Tools Menu
    │   ├── WifiMenu
    │   ├── BluetoothMenu
    │   └── ExpertMenu
    └── Settings Menu
        ├── SliderView (Brightness)
        ├── ListView (Language)
        ├── SliderView (Timezone)
        ├── SliderView (Auto Sleep)
        ├── T9InputView (Badge Text)
        ├── DateInputView
        ├── TimeInputView
        └── PinChangeView
```

### 6.2 Modal-Flows

```
Any View
    ↓ showConfirm() / showToast() / showModal()
ConfirmView / ToastView / ContextMenuView
    ↓ (Y/N/Timeout)
Back to previous View
```

### 6.3 Lock-Flow

```
LockScreenView
    ↓ (any key)
PinEntryView
    ↓ (PIN falsch)
ToastView (Wrong PIN) → PinEntryView (retry)
    ↓ (PIN korrekt)
Main Menu
    ↓ (timeout/inactivity)
LockScreenView
```

---

## 7. T9-Input-Logik (Detailanalyse)

### 7.1 State Machine

```
Idle (lastKey_ = 0)
    ↓
Key pressed (e.g., '2')
    ↓
Check: same key? → No → Insert first char ('a')
    ↓
lastKey_ = '2', charIndex_ = 0, cursorActive_ = true
    ↓
Timeout (2000ms) OR different key
    ↓
Commit character, lastKey_ = 0
    ↓
Back to Idle
```

### 7.2 Multi-Tap-Logik

```cpp
if (sameKey && !timeout && len_ > 0) {
    // Gleicher Key innerhalb von TIMEOUT_MS → Zyklisch wechseln
    charIndex_++;
    if (charIndex_ >= charCount) {
        charIndex_ = 0;
    }
    text_[len_ - 1] = getChar(key, charIndex_);  // Letztes Zeichen ersetzen
} else {
    // Neuer Key oder Timeout → Neues Zeichen einfügen
    text_[len_++] = getChar(key, 0);
    charIndex_ = 0;
}
```

### 7.3 Zeichen-Tabelle (CP437)

```
Key '2': a b c A B C 2 ä á â à å æ Ä Å Æ ç Ç
Key '3': d e f D E F 3 é è ê ë É
Key '6': m n o M N O 6 ó ò ô ö Ö ñ Ñ
Key '8': t u v T U V 8 ü Ü ú ù û
```

---

## 8. i18n-Pipeline (Detailanalyse)

### 8.1 Lookup-Reihenfolge

```cpp
const char* I18n::tr(const char* key) const {
    if (currentLang_ != "en") {
        if (const char* s = overlayLookup(key)) return s;  // 1. Overlay (Deutsch)
    }
    if (const char* s = enLookup(key)) return s;           // 2. Englisch (Fallback)
    static thread_local char missing[64];
    std::snprintf(missing, sizeof(missing), "?%s", key);
    return missing;                                        // 3. Fehlt (?key)
}
```

### 8.2 Speicherverwaltung

**Englisch (in-code):**
- `std::vector<I18nEntry> en_` (internal RAM)
- Binäre Suche für schnelles Lookup

**Overlay (Deutsch):**
- `PsramUniquePtr<char> overlayBlob_` (PSRAM) - Packed "key\0value\0..."
- `PsramUniquePtr<OverlayRef> overlayRefs_` (PSRAM) - Sorted index
- Binäre Suche für schnelles Lookup

### 8.3 Dateistruktur

**lang_de.json:**

```json
{
  "core.lang_name": "Deutsch",
  "core.save": "Speichern",
  "core.cancel": "Abbrechen",
  "core.main_menu": "Hauptmenü",
  ...
}
```

**Lade-Pfad:**

```
/plugins/i18n/lang_de.json
    ↓ fopen/fread
UTF-8 String in PSRAM
    ↓ cJSON_Parse
JSON tree in PSRAM
    ↓ Loop: cp437::fromUtf8()
CP437 Strings in PSRAM blob
    ↓ Sortieren
Binary search ready
```

---

## 9. Key-Constants (`components/cdc_views/include/cdc_views/KeyCodes.h`)

```cpp
// Keypad-Matrix (TCA9535)
#define KEY_NONE  0
#define KEY_0     '0'
#define KEY_1     '1'
...
#define KEY_9     '9'
#define KEY_YES   'Y'
#define KEY_NO    'N'
#define KEY_MENU  '3'  // Kontextmenü

// Navigation
#define KEY_UP    '2'
#define KEY_DOWN  '8'
#define KEY_LEFT  '4'
#define KEY_RIGHT '6'
```

---

## 10. Footer-Hints (i18n-Keys)

**Zentrale Hints (`components/cdc_ui/src/I18n.cpp`):**

| Key | Default English | Beschreibung |
|-----|----------------|--------------|
| `core.hint_back` | `[N] Back` | Zurück |
| `core.hint_select` | `[Y] Select` | Auswählen |
| `core.hint_ok_back` | `[Y] OK [N] Back` | OK/Back |
| `core.hint_approve_deny` | `[Y] Approve  [N] Deny` | Approve/Deny |
| `core.hint_brightness` | `<4 6> Adjust [Y] Save` | Slider |
| `core.hint_pin_input` | `[0-9] Input [Y] OK` | PIN |
| `core.hint_t9_input` | `[0-9] T9 [Y] OK` | T9 |
| `core.hint_list_menu` | `[3] Menu` | Liste |
| `core.hint_scroll_back` | `[2/8] Scroll [N] Back` | Scroll |
| `core.hint_field_nav` | `[4] <  [6] >` | Feld-Navigation |
| `core.hint_plugin_list` | `[Y] Start [3] Menu [N] Back` | Plugin-Liste |

---

## 11. Memory-Management

### 11.1 PSRAM vs Internal RAM

**PSRAM (Octal, 80 MHz) - Default für große Buffers:**

```cpp
// PSRAM allocation
char* buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);

// RAII wrapper
PramUniquePtr<char> buf = cdc::core::psramAlloc<char>(size);

// Static large buffers
EXT_RAM_BSS_ATTR static char large_array[1024];
```

**Internal RAM - Nur für kritische Daten:**

```cpp
// Small stacks, ISR data, BLE/WiFi internals
static char small_buffer[64];
```

### 11.2 View-Spezifische Allokation

| View | Buffer | Speicher |
|------|--------|----------|
| `T9InputView` | `text_[320]` | Stack |
| `InfoView` | `textBuf_[2048]` | **PSRAM** |
| `CanvasView` | `textArena_[2048]` | Stack |
| `I18n` | `overlayBlob_` | **PSRAM** |

---

## 12. Thread-Safety

### 12.1 ViewStack Mutex

```cpp
// Recursive Mutex (für re-entrant calls)
static constexpr uint8_t MAX_DEPTH = 8;
SemaphoreHandle_t mutex_ = nullptr;

// StackLock (RAII guard)
class StackLock {
    explicit StackLock(SemaphoreHandle_t m) {
        xSemaphoreTakeRecursive(m_, portMAX_DELAY);
    }
    ~StackLock() {
        xSemaphoreGiveRecursive(m_);
    }
};
```

### 12.2 ListView EditMutex

```cpp
// Für cross-task buffer access
void setEditMutex(SemaphoreHandle_t mutex);

// Usage in render/onKey:
cdc::core::RecursiveMutexGuard guard(editMutex_);
```

---

## 13. Zusammenfassung

**Stärken des UI-Systems:**

1. **Modular:** Views sind wiederverwendbar und selbstständig
2. **Thread-Safe:** Recursive Mutex für Multi-Task-Unterstützung
3. **i18n-ready:** Vollständige Internationalisierung mit CP437
4. **Memory-efficient:** PSRAM für große Buffers
5. **Flexible:** Modal-System, Exclusive Locks, Inactivity Timeouts

**Wichtige Design-Entscheidungen:**

1. **CP437 statt UTF-8** für Display (GFX-Font-Kompatibilität)
2. **PSRAM-first** für große Arrays (Internal RAM ist limitiert)
3. **Singleton Views** (shared instances) für Speicheroptimierung
4. **RAII-Pattern** für Resources (Mutex, File, NVS)
5. **Callback-basiert** für lose Kopplung

**Known Limitations:**

1. **Max Stack Depth:** 8 Views (kann bei tiefer Navigation erreicht werden)
2. **No Undo:** Pop-Operationen sind nicht rückgängig zu machen
3. **Blocking Sleep:** `enterLightSleep()` blockiert den Main-Task
4. **Single Modal:** Nur ein Modal gleichzeitig

---

**Ende der Analyse**
