// ── Librería de inferencia: SeparadorTapas2 con anomaly detection habilitado ──
#include <SeparadorTapas2_inferencing.h>
#include "Classifier.h"
#include "esp_heap_caps.h"
#include <Arduino.h>

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Modelo invalido para este sensor"
#endif

// ⚠ TODO: Habilitar anomaly detection en Edge Impulse
#if !defined(EI_CLASSIFIER_HAS_ANOMALY) || EI_CLASSIFIER_HAS_ANOMALY != 1
#warning "⚠ SeparadorTapas2 NO tiene anomaly detection."
#endif

// ── EI memory: pool dedicado en PSRAM con caché por tamaño ───────────────────
// El SDK aloca ~620 KB en ~16 bloques POR INFERENCIA (frees intercalados) y
// escribe en memoria ya liberada: con el heap normal, el web server reutiliza
// esos bloques y termina corrupto (CORRUPT HEAP solo con tráfico web).
// Este pool aísla TODA la memoria de EI del heap del sistema. Como la
// secuencia de tamaños se repite idéntica en cada inferencia, un caché por
// tamaño estabiliza el pool en ~1 ciclo de memoria (~620 KB).
// NOTA: solo la tarea de inferencia (Core 0) llama a estas funciones.
static uint8_t     *ei_pool      = nullptr;
static size_t       ei_pool_off  = 0;
static const size_t EI_POOL_SIZE = 3 * 1024 * 1024;  // 3 MB (PSRAM tiene 8 MB)
static const size_t EI_GUARD     = 32;               // colchón anti-overflow

struct EiBlock { void *ptr; size_t size; bool in_use; };
static EiBlock ei_blocks[96];   // side-table: la metadata vive FUERA del pool,
static int     ei_nblocks = 0;  // así ninguna escritura tardía puede pisarla

static void *ei_pool_alloc(size_t size, bool zero) {
    if (!ei_pool) {
        ei_pool = (uint8_t *)heap_caps_malloc(EI_POOL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ei_pool) return nullptr;
    }

    // 1. Reutilizar un bloque libre del MISMO tamaño
    for (int i = 0; i < ei_nblocks; i++) {
        if (!ei_blocks[i].in_use && ei_blocks[i].size == size) {
            ei_blocks[i].in_use = true;
            if (zero) memset(ei_blocks[i].ptr, 0, size);
            return ei_blocks[i].ptr;
        }
    }

    // 2. Bloque nuevo desde el pool
    size_t aligned = (size + 15) & ~(size_t)15;
    if (ei_pool_off + aligned + EI_GUARD > EI_POOL_SIZE || ei_nblocks >= 96) {
        // No debería pasar: avisar y caer al heap
        Serial.printf("[EI] POOL/TABLA LLENA (off=%u, nblocks=%d), fallback heap %u B\n",
                      (unsigned)ei_pool_off, ei_nblocks, (unsigned)size);
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p && zero) memset(p, 0, size);
        return p;
    }
    void *p = ei_pool + ei_pool_off;
    ei_pool_off += aligned + EI_GUARD;
    ei_blocks[ei_nblocks].ptr    = p;
    ei_blocks[ei_nblocks].size   = size;
    ei_blocks[ei_nblocks].in_use = true;
    ei_nblocks++;
    if (zero) memset(p, 0, size);
    return p;
}

void *ei_malloc(size_t size)           { return ei_pool_alloc(size, false); }
void *ei_calloc(size_t n, size_t size) { return ei_pool_alloc(n * size, true); }
void  ei_free(void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < ei_nblocks; i++) {
        if (ei_blocks[i].ptr == ptr) { ei_blocks[i].in_use = false; return; }
    }
    free(ptr);   // bloque de fallback del heap (raro)
}

// ── Definición del puntero estático ──────────────────────────────────────────
Classifier *Classifier::_instance = nullptr;

// ── Constructor ───────────────────────────────────────────────────────────────
Classifier::Classifier(Camera &camera)
    : _camera(camera), _mutex(nullptr), _anomalyScore(0.0f), _lastScoreTime(0)
{
    _instance = this;
}

// ── start() — crea la tarea FreeRTOS en Core 0 ───────────────────────────────
void Classifier::start() {
    _mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(
        _taskEntry,
        "inference",
        49152,   // 48 KB: fmt2rgb888 (JPEG dec) + crop + TFLite/ESP-NN kernels
                 // necesitan mucho más que los 16 KB originales; el overflow
                 // silencioso corrompe memoria y se manifiesta como TG1WDT
        nullptr,
        2,      // prioridad baja: no rivaliza con WiFi/lwIP ni tareas del sistema
        nullptr,
        0   // Core 0 (loop/WebServer corre en Core 1)
    );
}

// ── _taskEntry() — puente estático → método de instancia ─────────────────────
void Classifier::_taskEntry(void *pv) {
    _instance->_loop();
    vTaskDelete(nullptr);
}

// ── _loop() — bucle principal de inferencia ───────────────────────────────────
void Classifier::_loop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // 1. Capturar frame
        if (!_camera.capture(EI_CLASSIFIER_INPUT_WIDTH,
                             EI_CLASSIFIER_INPUT_HEIGHT)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 2. Construir señal para EI
        ei::signal_t signal;
        signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
        signal.get_data     = &Camera::getDataCallback;

        // 3. Ejecutar inferencia
        ei_impulse_result_t result = {};
        EI_IMPULSE_ERROR rc = run_classifier(&signal, &result, false);
        if (rc != EI_IMPULSE_OK) {
            const char *err_msg = "UNKNOWN";
            switch (rc) {
                case EI_IMPULSE_CANCELED:      err_msg = "CANCELED"; break;
                case EI_IMPULSE_ALLOC_FAILED:  err_msg = "OUT_OF_MEMORY"; break;
                case EI_IMPULSE_INVALID_SIZE:  err_msg = "INVALID_SIZE"; break;
                default: break;
            }
            uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            uint32_t dram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            Serial.printf("[Classifier] ERROR: %s (code=%d)  PSRAM=%u DRAM=%u\n",
                          err_msg, (int)rc, psram_free, dram_free);
            continue;
        }

        // 4. Leer anomaly score
        // FOMO-AD (visual anomaly) guarda el resultado en visual_ad_result,
        // NO en result.anomaly (ese campo es para GMM/K-means clásico).
        // max_value = celda más anómala del grid 5×5 — es lo que usábamos al
        // calibrar el umbral y detecta defectos localizados que el promedio
        // diluiría entre las 25 celdas.
#if defined(EI_CLASSIFIER_HAS_VISUAL_ANOMALY) && EI_CLASSIFIER_HAS_VISUAL_ANOMALY == 1
        float score = result.visual_ad_result.max_value;
#else
        float score = result.anomaly;
#endif

        // 5. Actualizar estado compartido (protegido por mutex)
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _anomalyScore  = score;
        _lastScoreTime = millis();   // timestamp de esta inferencia
        xSemaphoreGive(_mutex);

        Serial.printf("[Anomaly] score=%.4f  [DSP:%dms Inf:%dms Anomaly:%dms]\n",
                      score,
                      result.timing.dsp,
                      result.timing.classification,
                      result.timing.anomaly);
    }
}

// ── getAnomalyScore() — getter thread-safe ───────────────────────────────────
float Classifier::getAnomalyScore() const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float s = _anomalyScore;
    xSemaphoreGive(_mutex);
    return s;
}

uint32_t Classifier::getLastScoreTime() const {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    uint32_t t = _lastScoreTime;
    xSemaphoreGive(_mutex);
    return t;
}
