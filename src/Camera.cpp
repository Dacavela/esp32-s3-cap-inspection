#include "Camera.h"
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_heap_caps.h"
#include <Arduino.h>

// Cache_WriteBack_Addr existe en el ROM/HAL del ESP32-S3 para todas las versiones
// de IDF. La declaramos directamente para no depender de headers privados.
extern "C" void Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

// Definición del puntero estático (obligatorio en el .cpp)
Camera *Camera::_instance = nullptr;

// ── Constructor / Destructor ──────────────────────────────────────────────────
Camera::Camera() : _initialised(false), _buf(nullptr), _bufMutex(nullptr) {
    _instance = this;   // registrar esta instancia para el callback
}

Camera::~Camera() {
    if (_buf) {
        free(_buf);
        _buf = nullptr;
    }
}

// ── init() ────────────────────────────────────────────────────────────────────
bool Camera::init() {
    if (_initialised) return true;

    camera_config_t config = {};
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = CAM_Y2_GPIO;
    config.pin_d1        = CAM_Y3_GPIO;
    config.pin_d2        = CAM_Y4_GPIO;
    config.pin_d3        = CAM_Y5_GPIO;
    config.pin_d4        = CAM_Y6_GPIO;
    config.pin_d5        = CAM_Y7_GPIO;
    config.pin_d6        = CAM_Y8_GPIO;
    config.pin_d7        = CAM_Y9_GPIO;
    config.pin_xclk      = CAM_XCLK_GPIO;
    config.pin_pclk      = CAM_PCLK_GPIO;
    config.pin_vsync     = CAM_VSYNC_GPIO;
    config.pin_href      = CAM_HREF_GPIO;
    config.pin_sccb_sda  = CAM_SIOD_GPIO;
    config.pin_sccb_scl  = CAM_SIOC_GPIO;
    config.pin_pwdn      = CAM_PWDN_GPIO;
    config.pin_reset     = CAM_RESET_GPIO;
    config.xclk_freq_hz  = 10000000;
    config.pixel_format  = PIXFORMAT_JPEG;
    config.frame_size    = FRAMESIZE_QVGA;
    config.jpeg_quality  = 10;
    config.fb_count      = 2;
    config.grab_mode     = CAMERA_GRAB_LATEST;
    config.fb_location   = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Camera] init failed: 0x%x\n", err);
        return false;
    }

    // Configuración del sensor (igual config que la recolección de datos)
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s,     FRAMESIZE_QVGA);
    s->set_brightness(s,    0);
    s->set_contrast(s,      1);
    s->set_saturation(s,    0);
    s->set_sharpness(s,     1);
    s->set_denoise(s,       2);
    s->set_raw_gma(s,       1);
    s->set_lenc(s,          1);
    s->set_hmirror(s,       0);
    s->set_vflip(s,         1);

    // ── Warm-up: autoexposición libre → luego bloquear ───────────────────────
    // El problema no es la autoexposición en sí, sino que cambia entre frames
    // durante la inferencia. Solución: dejar que AEC/AGC se estabilicen con
    // la iluminación real y luego bloquear esos valores. Así las imágenes de
    // inferencia son consistentes entre sí Y similares a las de entrenamiento
    // (también capturadas con AEC activo).
    s->set_whitebal(s,      1);
    s->set_awb_gain(s,      1);
    s->set_exposure_ctrl(s, 1);   // AEC ON para warm-up
    s->set_gain_ctrl(s,     1);   // AGC ON para warm-up

    // Descartar frames hasta que AEC/AGC converjan (~30 frames x 66ms = 2s)
    Serial.print("[Camera] Calibrando exposicion");
    for (int i = 0; i < 30; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        if (i % 5 == 0) Serial.print(".");
        delay(66);
    }
    Serial.println(" OK");

    // Bloquear AEC y AGC en los valores que el sensor encontró
    s->set_exposure_ctrl(s, 0);
    s->set_gain_ctrl(s,     0);
    // OV2640 retiene los registros internos de AEC/AGC al desactivarlos

    // Alocar buffer RGB888 en PSRAM, con fallback a DRAM si falla
    size_t bufsize = FRAME_COLS * FRAME_ROWS * FRAME_BPP;

    // Intento 1: PSRAM (preferido)
    _buf = (uint8_t *)heap_caps_malloc(bufsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (_buf) {
        Serial.printf("[Camera] Buffer %u bytes en PSRAM OK\n", bufsize);
    } else {
        // Intento 2: DRAM (fallback)
        Serial.println("[Camera] PSRAM malloc failed, intentando DRAM...");
        _buf = (uint8_t *)malloc(bufsize);
        if (_buf) {
            Serial.printf("[Camera] Buffer %u bytes en DRAM OK (degraded mode)\n", bufsize);
        } else {
            Serial.println("[Camera] DRAM malloc also failed");
            return false;
        }
    }

    // Mutex para proteger _buf entre Core 0 (inferencia) y Core 1 (web server)
    _bufMutex = xSemaphoreCreateMutex();
    if (!_bufMutex) {
        Serial.println("[Camera] Error creando mutex");
        return false;
    }

    _initialised = true;
    Serial.println("[Camera] OK");
    return true;
}

// ── capture() ─────────────────────────────────────────────────────────────────
bool Camera::capture(uint32_t out_width, uint32_t out_height) {
    if (!_initialised) return false;

    // ── Sección crítica: cubre el driver (fb_get) Y la escritura de _buf ──────
    // esp_camera_fb_get no es thread-safe: handleSnapshot (Core 1) usa el mismo
    // driver, así que serializamos todo el acceso con el mutex.
    xSemaphoreTake(_bufMutex, portMAX_DELAY);

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(_bufMutex);
        return false;
    }

    bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, _buf);
    esp_camera_fb_return(fb);

    if (!ok) {
        xSemaphoreGive(_bufMutex);
        return false;
    }

    // Resize solo si el modelo pide distinta resolución a 320×240
    if (out_width != FRAME_COLS || out_height != FRAME_ROWS) {
        ei::image::processing::crop_and_interpolate_rgb888(
            _buf, FRAME_COLS, FRAME_ROWS,
            _buf, out_width, out_height);
    }

    // ── SIN rotación ──────────────────────────────────────────────────────────
    // El dataset (pictaker) se capturó SIN rotar, así que el feed de inferencia
    // debe ir igual: misma orientación que las imágenes de entrenamiento.
    // (Antes había una rotación de 90° aquí — descalibraba el anomaly score
    // porque el modelo veía todo girado respecto al entrenamiento.)
    // La rotación del preview web se hace en CSS, no en el pipeline.

    // Flush de caché PSRAM: Core 0 escribió _buf, Core 1 lo leerá en el web server.
    // Sin esto, Core 1 puede ver líneas de caché obsoletas → rayas en el preview.
    Cache_WriteBack_Addr((uint32_t)_buf, out_width * out_height * 3);

    xSemaphoreGive(_bufMutex);
    // ── Fin de sección crítica ────────────────────────────────────────────────

    return true;
}

// ── copyFrame() — copia thread-safe para el web server (Core 1) ───────────────
bool Camera::copyFrame(uint8_t *dst, size_t len) {
    if (!_initialised || !_buf || !dst) return false;

    // Timeout de 200 ms: si la inferencia tiene el lock, no bloqueamos el web
    // server indefinidamente — devolvemos false y el handler responde 503.
    if (xSemaphoreTake(_bufMutex, pdMS_TO_TICKS(200)) != pdTRUE)
        return false;

    memcpy(dst, _buf, len);
    xSemaphoreGive(_bufMutex);
    return true;
}

// ── lock()/unlock() — serializar acceso al driver desde Core 1 ────────────────
bool Camera::lock(uint32_t timeout_ms) {
    if (!_bufMutex) return false;
    return xSemaphoreTake(_bufMutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Camera::unlock() {
    if (_bufMutex) xSemaphoreGive(_bufMutex);
}

// ── getDataCallback() — callback estático para EI ─────────────────────────────
// Formato requerido por Edge Impulse: cada float contiene el píxel RGB
// EMPAQUETADO como entero 0xRRGGBB. El DSP de EI extrae los canales y
// normaliza internamente (también convierte a grayscale si el modelo lo pide).
// ⚠ NO enviar valores normalizados 0-1: el modelo recibiría basura.
int Camera::getDataCallback(size_t offset, size_t length, float *out_ptr) {
    if (!_instance || !_instance->_buf) return -1;

    uint8_t *buf      = _instance->_buf;
    size_t   pixel_ix = offset * 3;      // 3 bytes por píxel (RGB888)
    size_t   out_ix   = 0;

    while (length--) {
        // ⚠ fmt2rgb888 entrega BGR (no RGB) — el byte +2 es el ROJO.
        // Mismo orden que usa el ejemplo oficial de Edge Impulse para ESP32.
        out_ptr[out_ix++] = (float)(((uint32_t)buf[pixel_ix + 2] << 16) |
                                    ((uint32_t)buf[pixel_ix + 1] << 8)  |
                                     (uint32_t)buf[pixel_ix + 0]);
        pixel_ix += 3;
    }
    return 0;
}
