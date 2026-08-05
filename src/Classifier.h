#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// IMPORTANTE: el header de inferencia se incluye SOLO en Classifier.cpp
// para evitar definiciones duplicadas al linkear.
#include "Camera.h"

class Classifier {
public:
    explicit Classifier(Camera &camera);

    // Inicia la tarea de inferencia en Core 0
    void start();

    // Getter thread-safe: devuelve el último anomaly score
    float    getAnomalyScore() const;

    // Timestamp (millis) de cuándo se completó la última inferencia
    uint32_t getLastScoreTime() const;

private:
    Camera                    &_camera;
    mutable SemaphoreHandle_t  _mutex;
    float                      _anomalyScore;
    uint32_t                   _lastScoreTime;  // millis() al terminar inferencia

    static Classifier *_instance;
    static void        _taskEntry(void *pv);
    void               _loop();
};
