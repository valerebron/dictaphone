#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ESP_I2S.h>

#if !defined(ARDUINO_USB_MODE)
#error ARDUINO_USB_MODE is not defined
#elif ARDUINO_USB_MODE == 1
#error USBMode must be USB-OTG (TinyUSB): USBMode=default
#endif

#include <USB.h>
#include <USBMSC.h>

// ============================================================
// XIAO ESP32-S3 SENSE - DICTAPHONE
// Arduino-ESP32 Core 3.3.11
//
// MIC PDM:
//   CLK  = GPIO42
//   DATA = GPIO41
//
// SD:
//   CS   = GPIO21
//   SCK  = GPIO7
//   MISO = GPIO8
//   MOSI = GPIO9
//
// PTT:
//   GPIO1
//   active LOW
//
// LED:
//   GPIO2
//
// USB:
//   USB MSC exposes SD card.
//   When recording, MSC media is removed.
// ============================================================


// ============================================================
// PINS
// ============================================================

#define SD_CS_PIN       21
#define SD_SCK_PIN      7
#define SD_MISO_PIN     8
#define SD_MOSI_PIN     9

#define MIC_CLK_PIN     42
#define MIC_DATA_PIN    41

#define PTT_PIN         1
#define LED_MIC_PIN     2


// ============================================================
// SD / USB MSC
// ============================================================

#define SD_SPI_FREQUENCY 40000000UL
#define MSC_SECTOR_SIZE  512


// ============================================================
// AUDIO
// ============================================================

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS        16
#define AUDIO_CHANNELS    1

#define AUDIO_BYTES_PER_SECOND \
    (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS / 8))

#define AUDIO_CHUNK_SIZE        (16 * 1024UL)
#define AUDIO_I2S_READ_SIZE     4096UL
#define AUDIO_FLUSH_INTERVAL_MS 2000UL


// ============================================================
// USB / PTT TAKEOVER
// ============================================================

#define MSC_IDLE_BEFORE_TAKEOVER_MS  1500UL
#define TAKEOVER_HOLD_MS             1200UL


// ============================================================
// OBJECTS
// ============================================================

SPIClass sdSPI(FSPI);

USBMSC MSC;

I2SClass I2S;


// ============================================================
// STATES
// ============================================================

enum RecorderState : uint8_t
{
    STATE_USB_MSC,
    STATE_IDLE,
    STATE_RECORDING
};

RecorderState state = STATE_IDLE;


// ============================================================
// RUNTIME FLAGS
// ============================================================

volatile bool usbMounted = false;
volatile bool mscHostActive = false;
volatile bool mscEjected = false;

// IMPORTANT:
// USB callback runs from another task.
// It only sets this flag.
// loop() performs the actual state change.
volatile bool usbStopPending = false;
volatile bool usbStartPending = false;

bool mscConfigured = false;
bool sdReady = false;
bool i2sReady = false;

bool mscMediaAvailable = false;


// ============================================================
// MSC ACTIVITY
// ============================================================

volatile uint32_t lastMscActivityMs = 0;


// ============================================================
// RECORDING
// ============================================================

File recordFile;

uint8_t *audioBuffer = nullptr;

uint32_t recordDataBytes = 0;
uint32_t recordStartMs = 0;
uint32_t recordLastFlushMs = 0;


// ============================================================
// FORWARD DECLARATIONS
// ============================================================

bool initializeSD();
bool configureMSC();

void setRecordLED(bool on);

void enableMSCMedium();
void disableMSCMedium();

void processPendingUSBEvents();
void synchronizeUSBState();

bool initializeI2S();

bool startRecording();
void stopRecording();

void takeoverSDForPTT();

void processRecording();

void handleButton();
void handleSerial();

bool createNextFilename(char *filename, size_t size);

void writeWavHeader(File &file, uint32_t dataSize);

void printHelp();
void printStatus();
void printSDInfo();
void printUSBArbitration();

static void usbEventCallback(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);

static int32_t mscRead(
    uint32_t lba,
    uint32_t offset,
    void *buffer,
    uint32_t bufsize
);

static int32_t mscWrite(
    uint32_t lba,
    uint32_t offset,
    uint8_t *buffer,
    uint32_t bufsize
);

static bool mscStartStop(
    uint8_t power_condition,
    bool start,
    bool load_eject
);


// ============================================================
// LED
// ============================================================

void setRecordLED(bool on)
{
    digitalWrite(
        LED_MIC_PIN,
        on ? HIGH : LOW
    );
}


// ============================================================
// MSC MEDIUM CONTROL
// ============================================================

void enableMSCMedium()
{
    if (!mscConfigured)
    {
        return;
    }

    MSC.mediaPresent(true);

    mscMediaAvailable = true;

    lastMscActivityMs = millis();

    Serial.println(
        "[MSC] MEDIUM PRESENT"
    );
}


void disableMSCMedium()
{
    if (!mscConfigured)
    {
        return;
    }

    MSC.mediaPresent(false);

    mscMediaAvailable = false;

    mscHostActive = false;
    mscEjected = true;

    Serial.println(
        "[MSC] MEDIUM REMOVED FROM HOST"
    );
}


// ============================================================
// USB EVENT CALLBACK
//
// IMPORTANT:
//
// Ce callback ne manipule PAS directement le SD ou MSC.
// Il tourne dans une tâche USB séparée.
// Il pose uniquement un drapeau.
//
// Arduino-ESP32 documente STARTED/STOPPED comme événements
// de montage/démontage TinyUSB.
// ============================================================

static void usbEventCallback(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != ARDUINO_USB_EVENTS)
    {
        return;
    }

    switch (event_id)
    {
        case ARDUINO_USB_STARTED_EVENT:

            usbStartPending = true;

            break;


        case ARDUINO_USB_STOPPED_EVENT:

            usbStopPending = true;

            break;


        case ARDUINO_USB_SUSPEND_EVENT:

            // Ne pas modifier l'état MSC ici.
            break;


        case ARDUINO_USB_RESUME_EVENT:

            // Ne pas modifier l'état MSC ici.
            break;


        default:
            break;
    }
}


// ============================================================
// PROCESS USB EVENTS
//
// Cette fonction est exécutée depuis loop().
// C'est ici que l'on modifie l'état MSC/SD.
// ============================================================

void processPendingUSBEvents()
{
    // --------------------------------------------------------
    // USB STOPPED
    // --------------------------------------------------------

    if (usbStopPending)
    {
        usbStopPending = false;

        Serial.println();
        Serial.println(
            "[USB] DISCONNECTED"
        );

        usbMounted = false;

        mscHostActive = false;
        mscEjected = false;

        // Le PC n'a plus le bus USB.
        // On remet immédiatement le média dans un état
        // cohérent pour le mode dictaphone.
        disableMSCMedium();

        if (state != STATE_RECORDING)
        {
            state = STATE_IDLE;

            setRecordLED(false);
        }

        Serial.println(
            "[USB] SD LIBERATED"
        );

        Serial.println(
            "[STATE] IDLE / PTT READY"
        );
    }


    // --------------------------------------------------------
    // USB STARTED
    //
    // On traite START après STOP pour éviter qu'une séquence
    // rapide débranchement/rebranchement laisse un état
    // incohérent.
    // --------------------------------------------------------

    if (usbStartPending)
    {
        usbStartPending = false;

        Serial.println();
        Serial.println(
            "[USB] CONNECTED"
        );

        usbMounted = true;

        mscHostActive = false;
        mscEjected = false;

        if (state != STATE_RECORDING)
        {
            enableMSCMedium();

            state = STATE_USB_MSC;

            setRecordLED(false);

            Serial.println(
                "[STATE] USB MSC / PTT LOCKED"
            );
        }
    }
}


// ============================================================
// USB STATE SYNCHRONIZATION
//
// IMPORTANT:
//
// On ne fait plus:
//
//     if (!USB) ...
//
// car USB.operator bool() représente l'état du périphérique
// USB démarré, pas une autorisation suffisante pour décider
// que le PTT doit rester bloqué.
//
// Les événements STARTED/STOPPED sont prioritaires.
// ============================================================

void synchronizeUSBState()
{
    if (!mscConfigured)
    {
        return;
    }

    // Rien de destructif ici.
    //
    // Le changement de connexion USB est maintenant traité
    // par processPendingUSBEvents().
}


// ============================================================
// SD INITIALIZATION
// ============================================================

bool initializeSD()
{
    Serial.println();
    Serial.println(
        "=========================================="
    );
    Serial.println(
        "SD INITIALIZATION"
    );
    Serial.println(
        "=========================================="
    );

    Serial.printf(
        "[SD] SCK=%d MISO=%d MOSI=%d CS=%d\n",
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    if (!sdSPI.begin(
            SD_SCK_PIN,
            SD_MISO_PIN,
            SD_MOSI_PIN,
            SD_CS_PIN))
    {
        Serial.println(
            "[SD] ERROR SPI begin"
        );

        return false;
    }

    if (!SD.begin(
            SD_CS_PIN,
            sdSPI,
            SD_SPI_FREQUENCY))
    {
        Serial.println(
            "[SD] ERROR SD.begin"
        );

        return false;
    }

    uint32_t sectorSize =
        SD.sectorSize();

    if (sectorSize != MSC_SECTOR_SIZE)
    {
        Serial.printf(
            "[SD] ERROR sector size=%lu\n",
            (unsigned long)sectorSize
        );

        SD.end();

        return false;
    }

    sdReady = true;

    Serial.printf(
        "[SD] sectors=%lu\n",
        (unsigned long)SD.numSectors()
    );

    Serial.printf(
        "[SD] size=%llu bytes\n",
        SD.cardSize()
    );

    Serial.println(
        "[SD] OK"
    );

    return true;
}


// ============================================================
// MSC READ
// ============================================================

static int32_t mscRead(
    uint32_t lba,
    uint32_t offset,
    void *buffer,
    uint32_t bufsize)
{
    lastMscActivityMs = millis();

    mscHostActive = true;

    if (!sdReady)
    {
        return -1;
    }

    if (offset % MSC_SECTOR_SIZE != 0)
    {
        return -1;
    }

    if (bufsize % MSC_SECTOR_SIZE != 0)
    {
        return -1;
    }

    uint32_t sectors =
        bufsize / MSC_SECTOR_SIZE;

    if (lba >= SD.numSectors())
    {
        return -1;
    }

    if (sectors > SD.numSectors() - lba)
    {
        return -1;
    }

    uint8_t *dst =
        static_cast<uint8_t *>(buffer);

    for (
        uint32_t i = 0;
        i < sectors;
        i++)
    {
        if (!SD.readRAW(
                dst + i * MSC_SECTOR_SIZE,
                lba + i))
        {
            return -1;
        }
    }

    return (int32_t)bufsize;
}


// ============================================================
// MSC WRITE
// ============================================================

static int32_t mscWrite(
    uint32_t lba,
    uint32_t offset,
    uint8_t *buffer,
    uint32_t bufsize)
{
    lastMscActivityMs = millis();

    mscHostActive = true;

    if (!sdReady)
    {
        return -1;
    }

    if (offset % MSC_SECTOR_SIZE != 0)
    {
        return -1;
    }

    if (bufsize % MSC_SECTOR_SIZE != 0)
    {
        return -1;
    }

    uint32_t sectors =
        bufsize / MSC_SECTOR_SIZE;

    if (lba >= SD.numSectors())
    {
        return -1;
    }

    if (sectors > SD.numSectors() - lba)
    {
        return -1;
    }

    for (
        uint32_t i = 0;
        i < sectors;
        i++)
    {
        if (!SD.writeRAW(
                buffer + i * MSC_SECTOR_SIZE,
                lba + i))
        {
            return -1;
        }
    }

    return (int32_t)bufsize;
}


// ============================================================
// MSC START / STOP
// ============================================================

static bool mscStartStop(
    uint8_t power_condition,
    bool start,
    bool load_eject)
{
    lastMscActivityMs = millis();

    Serial.printf(
        "[MSC] STARTSTOP power=%u start=%u eject=%u\n",
        power_condition,
        start,
        load_eject
    );

    if (load_eject)
    {
        if (!start)
        {
            Serial.println(
                "[MSC] HOST EJECT"
            );

            mscEjected = true;
            mscHostActive = false;
        }
        else
        {
            Serial.println(
                "[MSC] HOST LOAD"
            );

            mscEjected = false;
            mscHostActive = true;
        }
    }
    else if (start)
    {
        mscHostActive = true;
        mscEjected = false;

        Serial.println(
            "[MSC] HOST ACTIVE"
        );
    }
    else
    {
        mscHostActive = false;

        Serial.println(
            "[MSC] HOST STOP"
        );
    }

    return true;
}


// ============================================================
// MSC CONFIGURATION
// ============================================================

bool configureMSC()
{
    if (!sdReady)
    {
        return false;
    }

    Serial.println();
    Serial.println(
        "=========================================="
    );
    Serial.println(
        "USB MSC INITIALIZATION"
    );
    Serial.println(
        "=========================================="
    );

    MSC.vendorID(
        "SEEED"
    );

    MSC.productID(
        "XIAO-DICTAPHONE"
    );

    MSC.productRevision(
        "1.0"
    );

    MSC.onRead(
        mscRead
    );

    MSC.onWrite(
        mscWrite
    );

    MSC.onStartStop(
        mscStartStop
    );

    MSC.isWritable(
        true
    );

    MSC.mediaPresent(
        false
    );

    if (!MSC.begin(
            SD.numSectors(),
            MSC_SECTOR_SIZE))
    {
        Serial.println(
            "[MSC] ERROR begin"
        );

        return false;
    }

    mscConfigured = true;

    // Register callback BEFORE USB.begin().
    USB.onEvent(
        usbEventCallback
    );

    USB.begin();

    lastMscActivityMs =
        millis();

    Serial.println(
        "[MSC] OK"
    );

    Serial.println(
        "[USB] STACK STARTED"
    );

    return true;
}


// ============================================================
// FILENAME
// ============================================================

bool createNextFilename(
    char *filename,
    size_t size)
{
    if (!filename ||
        size < 20)
    {
        return false;
    }

    for (
        uint32_t n = 1;
        n <= 99999;
        n++)
    {
        snprintf(
            filename,
            size,
            "/REC%05lu.WAV",
            (unsigned long)n
        );

        if (!SD.exists(filename))
        {
            return true;
        }
    }

    return false;
}


// ============================================================
// WAV HELPERS
// ============================================================

void writeLE16(
    uint8_t *p,
    uint16_t value)
{
    p[0] =
        value & 0xFF;

    p[1] =
        (value >> 8) & 0xFF;
}


void writeLE32(
    uint8_t *p,
    uint32_t value)
{
    p[0] =
        value & 0xFF;

    p[1] =
        (value >> 8) & 0xFF;

    p[2] =
        (value >> 16) & 0xFF;

    p[3] =
        (value >> 24) & 0xFF;
}


void writeWavHeader(
    File &file,
    uint32_t dataSize)
{
    uint8_t h[44];

    memset(
        h,
        0,
        sizeof(h)
    );

    memcpy(
        h + 0,
        "RIFF",
        4
    );

    writeLE32(
        h + 4,
        36 + dataSize
    );

    memcpy(
        h + 8,
        "WAVE",
        4
    );

    memcpy(
        h + 12,
        "fmt ",
        4
    );

    writeLE32(
        h + 16,
        16
    );

    writeLE16(
        h + 20,
        1
    );

    writeLE16(
        h + 22,
        AUDIO_CHANNELS
    );

    writeLE32(
        h + 24,
        AUDIO_SAMPLE_RATE
    );

    writeLE32(
        h + 28,
        AUDIO_BYTES_PER_SECOND
    );

    writeLE16(
        h + 32,
        AUDIO_CHANNELS *
        (AUDIO_BITS / 8)
    );

    writeLE16(
        h + 34,
        AUDIO_BITS
    );

    memcpy(
        h + 36,
        "data",
        4
    );

    writeLE32(
        h + 40,
        dataSize
    );

    file.seek(0);

    file.write(
        h,
        sizeof(h)
    );
}


// ============================================================
// I2S
// ============================================================

bool initializeI2S()
{
    I2S.end();

    Serial.printf(
        "[I2S] PDM CLK=%d DATA=%d\n",
        MIC_CLK_PIN,
        MIC_DATA_PIN
    );

    I2S.setPinsPdmRx(
        MIC_CLK_PIN,
        MIC_DATA_PIN
    );

    if (!I2S.begin(
            I2S_MODE_PDM_RX,
            AUDIO_SAMPLE_RATE,
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO))
    {
        Serial.println(
            "[I2S] ERROR"
        );

        i2sReady = false;

        return false;
    }

    i2sReady = true;

    Serial.println(
        "[I2S] OK"
    );

    return true;
}


// ============================================================
// TAKEOVER SD FOR PTT
// ============================================================

void takeoverSDForPTT()
{
    Serial.println();
    Serial.println(
        "=========================================="
    );
    Serial.println(
        "[PTT] TAKING OVER SD FROM MSC"
    );
    Serial.println(
        "=========================================="
    );

    disableMSCMedium();

    mscHostActive = false;

    mscEjected = true;

    // Important:
    // the logical USB state no longer prevents recording.
    usbMounted = false;

    state = STATE_IDLE;

    Serial.println(
        "[PTT] SD NOW OWNED BY RECORDER"
    );
}


// ============================================================
// START RECORDING
// ============================================================

bool startRecording()
{
    if (state == STATE_RECORDING)
    {
        return true;
    }

    if (!sdReady ||
        !audioBuffer)
    {
        Serial.println(
            "[REC] ERROR resources"
        );

        return false;
    }

    // Always remove the SD from MSC before opening the file.
    disableMSCMedium();

    char filename[32];

    if (!createNextFilename(
            filename,
            sizeof(filename)))
    {
        Serial.println(
            "[REC] ERROR filename"
        );

        return false;
    }

    Serial.println();
    Serial.println(
        "=========================================="
    );
    Serial.println(
        ">>> START RECORDING <<<"
    );
    Serial.println(
        "=========================================="
    );

    Serial.printf(
        "[REC] %s\n",
        filename
    );

    if (!initializeI2S())
    {
        return false;
    }

    recordFile =
        SD.open(
            filename,
            FILE_WRITE
        );

    if (!recordFile)
    {
        Serial.println(
            "[REC] ERROR open"
        );

        I2S.end();

        i2sReady = false;

        return false;
    }

    uint8_t blank[44];

    memset(
        blank,
        0,
        sizeof(blank)
    );

    if (recordFile.write(
            blank,
            sizeof(blank)) != sizeof(blank))
    {
        recordFile.close();

        I2S.end();

        i2sReady = false;

        return false;
    }

    recordDataBytes = 0;

    recordStartMs =
        millis();

    recordLastFlushMs =
        recordStartMs;

    state =
        STATE_RECORDING;

    setRecordLED(
        true
    );

    Serial.println(
        "[REC] RECORDING"
    );

    return true;
}


// ============================================================
// STOP RECORDING
// ============================================================

void stopRecording()
{
    if (state != STATE_RECORDING)
    {
        return;
    }

    setRecordLED(
        false
    );

    if (i2sReady)
    {
        I2S.end();

        i2sReady = false;
    }

    if (recordFile)
    {
        writeWavHeader(
            recordFile,
            recordDataBytes
        );

        recordFile.flush();

        recordFile.close();
    }

    float seconds =
        (millis() - recordStartMs)
        / 1000.0f;

    Serial.println();
    Serial.println(
        ">>> STOP RECORDING <<<"
    );

    Serial.printf(
        "[REC] %.2f sec\n",
        seconds
    );

    Serial.printf(
        "[REC] %lu bytes\n",
        (unsigned long)recordDataBytes
    );

    state =
        STATE_IDLE;

    Serial.println(
        "[STATE] IDLE / PTT READY"
    );
}


// ============================================================
// PROCESS RECORDING
// ============================================================

void processRecording()
{
    if (state != STATE_RECORDING)
    {
        return;
    }

    if (!recordFile ||
        !i2sReady)
    {
        Serial.println(
            "[REC] ERROR resources lost"
        );

        stopRecording();

        return;
    }

    size_t bytesRead =
        I2S.readBytes(
            (char *)audioBuffer,
            AUDIO_I2S_READ_SIZE
        );

    if (bytesRead == 0)
    {
        return;
    }

    size_t written =
        recordFile.write(
            audioBuffer,
            bytesRead
        );

    if (written != bytesRead)
    {
        Serial.printf(
            "[REC] WRITE ERROR %lu/%lu\n",
            (unsigned long)written,
            (unsigned long)bytesRead
        );

        stopRecording();

        return;
    }

    recordDataBytes +=
        written;

    uint32_t now =
        millis();

    if (now - recordLastFlushMs >=
        AUDIO_FLUSH_INTERVAL_MS)
    {
        recordLastFlushMs =
            now;

        recordFile.flush();

        Serial.printf(
            "[REC] %.1f sec %lu KiB\n",
            (now - recordStartMs)
            / 1000.0f,
            (unsigned long)
            (recordDataBytes / 1024)
        );
    }
}


// ============================================================
// PTT
// ============================================================

void handleButton()
{
    static bool lastRaw = HIGH;
    static bool stable = HIGH;

    static uint32_t lastChangeMs = 0;
    static uint32_t pressStartMs = 0;

    static bool takeoverDone = false;

    bool raw = digitalRead(PTT_PIN);

    // --------------------------------------------------------
    // DEBOUNCE
    // --------------------------------------------------------

    if (raw != lastRaw)
    {
        lastRaw = raw;
        lastChangeMs = millis();
    }

    if (millis() - lastChangeMs < 35)
    {
        return;
    }

    // --------------------------------------------------------
    // PTT STATE CHANGE
    // --------------------------------------------------------

    if (raw != stable)
    {
        stable = raw;

        // ----------------------------------------------------
        // PRESSED
        // ----------------------------------------------------

        if (stable == LOW)
        {
            Serial.println("[PTT] PRESSED");

            pressStartMs = millis();
            takeoverDone = false;

            // Si USB/MSC est clairement absent :
            // démarrage immédiat.
            if (!usbMounted && !mscHostActive)
            {
                Serial.println("[PTT] USB INACTIVE -> RECORD");

                if (startRecording())
                {
                    takeoverDone = true;
                }

                return;
            }

            Serial.println("[PTT] USB/MSC appears active");
            Serial.println("[PTT] HOLD ~1.2s TO FORCE TAKEOVER");
        }

        // ----------------------------------------------------
        // RELEASED
        // ----------------------------------------------------

        else
        {
            Serial.println("[PTT] RELEASED");

            takeoverDone = false;

            if (state == STATE_RECORDING)
            {
                stopRecording();
            }
        }
    }

    // --------------------------------------------------------
    // LONG HOLD TAKEOVER
    //
    // IMPORTANT:
    // On NE vérifie PLUS lastMscActivityMs.
    //
    // C'est volontaire :
    // TinyUSB peut rester "actif" après débranchement USB.
    // Le PTT devient alors l'autorité pour reprendre le SD.
    // --------------------------------------------------------

    if (stable == LOW &&
        state != STATE_RECORDING &&
        !takeoverDone)
    {
        uint32_t held = millis() - pressStartMs;

        if (held >= TAKEOVER_HOLD_MS)
        {
            Serial.println();
            Serial.println("==========================================");
            Serial.println("[PTT] FORCE TAKEOVER");
            Serial.println("==========================================");

            Serial.printf(
                "[PTT] held=%lu ms\n",
                (unsigned long)held
            );

            Serial.printf(
                "[PTT] usbMounted=%s\n",
                usbMounted ? "YES" : "NO"
            );

            Serial.printf(
                "[PTT] mscHostActive=%s\n",
                mscHostActive ? "YES" : "NO"
            );

            Serial.printf(
                "[PTT] usbLive=%s\n",
                static_cast<bool>(USB) ? "YES" : "NO"
            );

            takeoverDone = true;

            // ------------------------------------------------
            // FORCE SD OWNERSHIP
            // ------------------------------------------------

            takeoverSDForPTT();

            // ------------------------------------------------
            // START RECORDING
            // ------------------------------------------------

            if (!startRecording())
            {
                Serial.println(
                    "[PTT] ERROR: RECORDING FAILED"
                );
            }
        }
    }
}

// ============================================================
// SERIAL
// ============================================================

void printHelp()
{
    Serial.println();
    Serial.println(
        "Commands:"
    );

    Serial.println(
        " r = record"
    );

    Serial.println(
        " s = stop"
    );

    Serial.println(
        " i = SD info"
    );

    Serial.println(
        " t = status"
    );

    Serial.println(
        " u = USB info"
    );

    Serial.println(
        " f = force PTT takeover"
    );

    Serial.println(
        " h = help"
    );
}


void printStatus()
{
    Serial.println();
    Serial.println(
        "========== STATUS =========="
    );

    Serial.printf(
        "USB flag       : %s\n",
        usbMounted ? "YES" : "NO"
    );

    Serial.printf(
        "USB object     : %s\n",
        static_cast<bool>(USB)
        ? "STARTED" : "STOPPED"
    );

    Serial.printf(
        "USB stop pend. : %s\n",
        usbStopPending
        ? "YES" : "NO"
    );

    Serial.printf(
        "USB start pend.: %s\n",
        usbStartPending
        ? "YES" : "NO"
    );

    Serial.printf(
        "MSC host       : %s\n",
        mscHostActive
        ? "YES" : "NO"
    );

    Serial.printf(
        "MSC medium     : %s\n",
        mscMediaAvailable
        ? "PRESENT" : "ABSENT"
    );

    Serial.printf(
        "SD             : %s\n",
        sdReady
        ? "READY" : "ERROR"
    );

    Serial.printf(
        "I2S            : %s\n",
        i2sReady
        ? "READY" : "OFF"
    );

    switch (state)
    {
        case STATE_USB_MSC:

            Serial.println(
                "STATE          : USB_MSC"
            );

            break;

        case STATE_IDLE:

            Serial.println(
                "STATE          : IDLE"
            );

            break;

        case STATE_RECORDING:

            Serial.println(
                "STATE          : RECORDING"
            );

            break;
    }

    Serial.printf(
        "MSC idle       : %lu ms\n",
        (unsigned long)(
            millis() -
            lastMscActivityMs
        )
    );

    Serial.println(
        "============================"
    );
}


void printSDInfo()
{
    if (!sdReady)
    {
        return;
    }

    Serial.printf(
        "[SD] size=%llu used=%llu sectors=%lu\n",
        SD.totalBytes(),
        SD.usedBytes(),
        (unsigned long)
        SD.numSectors()
    );
}


void printUSBArbitration()
{
    printStatus();
}


void handleSerial()
{
    while (Serial.available())
    {
        char c =
            Serial.read();

        switch (c)
        {
            case 'r':
            case 'R':

                takeoverSDForPTT();

                startRecording();

                break;


            case 's':
            case 'S':

                stopRecording();

                break;


            case 'i':
            case 'I':

                printSDInfo();

                break;


            case 't':
            case 'T':

                printStatus();

                break;


            case 'u':
            case 'U':

                printUSBArbitration();

                break;


            case 'f':
            case 'F':

                if (state !=
                    STATE_RECORDING)
                {
                    takeoverSDForPTT();

                    startRecording();
                }

                break;


            case 'h':
            case 'H':

                printHelp();

                break;


            default:
                break;
        }
    }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );

    delay(
        1500
    );

    Serial.println();
    Serial.println();

    Serial.println(
        "########################################"
    );

    Serial.println(
        "# XIAO ESP32-S3 DICTAPHONE"
    );

    Serial.println(
        "# USB MSC + PTT RECORDER"
    );

    Serial.println(
        "########################################"
    );

    pinMode(
        PTT_PIN,
        INPUT_PULLUP
    );

    pinMode(
        LED_MIC_PIN,
        OUTPUT
    );

    setRecordLED(
        false
    );


    // --------------------------------------------------------
    // AUDIO BUFFER
    // --------------------------------------------------------

    audioBuffer =
        (uint8_t *)ps_malloc(
            AUDIO_CHUNK_SIZE
        );

    if (!audioBuffer)
    {
        audioBuffer =
            (uint8_t *)malloc(
                AUDIO_CHUNK_SIZE
            );
    }

    if (!audioBuffer)
    {
        Serial.println(
            "[FATAL] NO AUDIO MEMORY"
        );

        while (true)
        {
            delay(1000);
        }
    }


    // --------------------------------------------------------
    // SD
    // --------------------------------------------------------

    if (!initializeSD())
    {
        Serial.println(
            "[FATAL] SD"
        );

        while (true)
        {
            setRecordLED(
                true
            );

            delay(100);

            setRecordLED(
                false
            );

            delay(900);
        }
    }


    // --------------------------------------------------------
    // USB MSC
    // --------------------------------------------------------

    if (!configureMSC())
    {
        Serial.println(
            "[FATAL] USB MSC"
        );

        while (true)
        {
            delay(1000);
        }
    }


    lastMscActivityMs =
        millis();

    printHelp();

    Serial.println();

    Serial.println(
        "[READY] USB -> MSC"
    );

    Serial.println(
        "[READY] USB unplug -> PTT READY"
    );

    Serial.println(
        "[READY] Long PTT remains as fallback"
    );
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
    // IMPORTANT:
    // USB events are consumed here, not inside the USB callback.
    processPendingUSBEvents();

    synchronizeUSBState();

    handleSerial();

    handleButton();

    if (state ==
        STATE_RECORDING)
    {
        processRecording();
    }

    delay(1);
}
