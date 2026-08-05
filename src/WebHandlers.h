#pragma once
#include <WebServer.h>
#include "Camera.h"
#include "Classifier.h"
#include "Peripherals.h"

// ── WebHandlers ───────────────────────────────────────────────────────────────
// Encapsula todos los handlers HTTP de la aplicación.
// Recibe referencias a los objetos compartidos; no posee ninguno.
// Uso:
//   WebHandlers handlers(camera, classifier, server, peripherals, ANOMALY_THRESHOLD);
//   handlers.registerRoutes();   // registrar antes de server.begin()

class WebHandlers {
public:
    WebHandlers(Camera& camera, Classifier& classifier,
                WebServer& server, Peripherals& peripherals, float threshold);

    // Registra las rutas en el WebServer.
    // Llamar después de WiFi.mode() y antes de server.begin().
    void registerRoutes();

private:
    Camera&      _camera;
    Classifier&  _classifier;
    WebServer&   _server;
    Peripherals& _peripherals;
    float        _threshold;

    void onRoot();
    void onSnapshot();
    void onResult();
    void onEIFrame();
};
