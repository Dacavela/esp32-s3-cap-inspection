#pragma once
#include <stdint.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// image.hpp se incluye en Camera.cpp, no aquí, para evitar que los headers
// de EI se propaguen a main.cpp y generen símbolos duplicados.

// ── Pines ESP32-S3 Eye ────────────────────────────────────────────────────────
#define CAM_PWDN_GPIO  -1
#define CAM_RESET_GPIO -1
#define CAM_XCLK_GPIO  15
#define CAM_SIOD_GPIO   4
#define CAM_SIOC_GPIO   5
#define CAM_Y2_GPIO    11
#define CAM_Y3_GPIO     9
#define CAM_Y4_GPIO     8
#define CAM_Y5_GPIO    10
#define CAM_Y6_GPIO    12
#define CAM_Y7_GPIO    18
#define CAM_Y8_GPIO    17
#define CAM_Y9_GPIO    16
#define CAM_VSYNC_GPIO  6
#define CAM_HREF_GPIO   7
#define CAM_PCLK_GPIO  13

class Camera {
public:
    static const uint32_t FRAME_COLS = 320;
    static const uint32_t FRAME_ROWS = 240;
    static const uint32_t FRAME_BPP  = 3;

    Camera();
    ~Camera();

    // Inicializa el hardware y aloca el buffer en PSRAM
    bool init();

    // Captura un frame, lo decodifica y lo redimensiona a out_width x out_height
    bool capture(uint32_t out_width, uint32_t out_height);

    // Puntero al buffer RGB888 (PSRAM)
    // ⚠ NO usar directamente desde Core 1 — usar copyFrame() (thread-safe)
    uint8_t *buf()   const { return _buf; }
    bool     ready() const { return _initialised; }

    // Copia thread-safe del frame para el web server (Core 1).
    // Devuelve false si no hay frame o timeout del mutex.
    bool copyFrame(uint8_t *dst, size_t len);

    // Lock manual para serializar acceso al driver de la cámara
    // (esp_camera_fb_get NO es thread-safe entre cores).
    bool lock(uint32_t timeout_ms);
    void unlock();

    // Callback estático para EI (signal.get_data)
    // Accede al buffer a través de _instance
    static int getDataCallback(size_t offset, size_t length, float *out_ptr);

private:
    bool              _initialised;
    uint8_t          *_buf;
    SemaphoreHandle_t _bufMutex;   // protege _buf entre Core 0 (capture) y Core 1 (web)

    // Puntero a la instancia activa, necesario para el callback estático
    static Camera *_instance;
};
