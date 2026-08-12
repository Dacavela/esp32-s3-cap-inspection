#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "Camera.h"
#include "Classifier.h"
#include "WiFiManager.h"
#include "Peripherals.h"
#include "WebHandlers.h"

// ── Constantes de aplicación ──────────────────────────────────────────────────
#define LED_IO2   2

// Buenas centradas: 2.5-4.4 (picos ~4.9) | Malas centradas: 6.8-8.9
// 5.5 = punto medio con margen a ambos lados
#define ANOMALY_THRESHOLD    5.5f

// Tiempo que tarda la tapa en recorrer sensor→cámara (ajustar físicamente)
// Debe coincidir con el valor de pictaker.cpp para que la tapa quede en el
// mismo encuadre que durante la captura del dataset.
#define SENSOR_TO_CAMERA_MS  200

// Tiempo que tarda la tapa en recorrer cámara→servo (ajustar físicamente)
#define CAMERA_TO_SERVO_MS   1500

#define AP_NAME "ESP32-CamConfig"
#define AP_PASS "12345678"

// ── Máquina de estados ────────────────────────────────────────────────────────
//
//   IDLE ──(sensor)──► APPROACH ──(tiempo)──► INSPECTING ──(inferencia)──► RELEASING ──(tiempo)──► COOLDOWN
//    ▲                                                                                                  │
//    └──────────────────────────────────────(sensor libre)────────────────────────────────────────────┘
//
//  Ventaja sobre el loop bloqueante anterior: ningún estado usa delay() fijo.
//  El web server y el sensor se chequean en cada iteración, sin importar en
//  qué estado esté el sistema.

enum class State {
    IDLE,        // banda corriendo, esperando tapa en el sensor
    APPROACH,    // tapa detectada, esperando que llegue a la cámara
    INSPECTING,  // banda parada, 1 inference descartada + mediana de 3
    RELEASING,   // banda corriendo, tapa viajando cámara→servo
    COOLDOWN     // servo reset, esperando que el sensor se libere
};

// ── Instancias globales ───────────────────────────────────────────────────────
Camera      camera;
Classifier  classifier(camera);
WebServer   server(80);
WiFiManager wifiManager;
Peripherals peripherals;
WebHandlers handlers(camera, classifier, server, peripherals, ANOMALY_THRESHOLD);

// ── Variables de estado ───────────────────────────────────────────────────────
static State    fsmState    = State::IDLE;
static uint32_t stateStart  = 0;   // millis() al entrar en el estado actual
static uint32_t stopTime    = 0;   // millis() al parar la banda
static uint32_t lastInfTs   = 0;   // timestamp de la última inference procesada
static int      inferCount  = 0;   // inferences válidas recibidas en INSPECTING
static float    infScores[3] = {0}; // scores de las inferences 2-4 (para mediana)
static bool     capBad      = false;

static const char *stateName(State s) {
    switch (s) {
        case State::IDLE:       return "IDLE";
        case State::APPROACH:   return "APPROACH";
        case State::INSPECTING: return "INSPECTING";
        case State::RELEASING:  return "RELEASING";
        case State::COOLDOWN:   return "COOLDOWN";
    }
    return "?";
}

static void enterState(State next) {
    Serial.printf("[FSM] %s → %s\n", stateName(fsmState), stateName(next));
    fsmState   = next;
    stateStart = millis();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);
    pinMode(LED_IO2, OUTPUT); digitalWrite(LED_IO2, LOW);
    disableCore0WDT();

    Serial.println("\n=== ANOMALY DETECTION TAPAS ===");
    Serial.printf("[MEM] Heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());

    if (!camera.init()) {
        Serial.println("[FATAL] Camara");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    wifiManager.registerRoutes(server);
    handlers.registerRoutes();
    server.onNotFound([]{ server.send(404, "text/plain", "Not found"); });
    server.begin();

    wifiManager.connect(server, AP_NAME, AP_PASS);

    peripherals.begin();
    peripherals.motorRun();
    peripherals.ledOn();

    classifier.start();

    Serial.printf("[MEM] Heap: %u  PSRAM: %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.println("[OK] Listo - Core 0: inference | Core 1: web + FSM");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop()
{
    server.handleClient();
    uint32_t now = millis();

    switch (fsmState) {

    // ── IDLE: banda corriendo, esperando tapa ─────────────────────────────────
    case State::IDLE:
        if (peripherals.isCapPresent()) {
            enterState(State::APPROACH);
        }
        break;

    // ── APPROACH: tapa detectada, dejar que viaje sensor→cámara ──────────────
    case State::APPROACH:
        if (now - stateStart >= SENSOR_TO_CAMERA_MS) {
            peripherals.motorStop();
            stopTime   = now;
            lastInfTs  = now;   // solo aceptar inferences DESPUÉS de parar
            inferCount = 0;
            enterState(State::INSPECTING);
        }
        break;

    // ── INSPECTING: banda parada, decidir por MEDIANA de 3 inferences ─────────
    //   Inference 1: se DESCARTA (su frame pudo capturarse antes de parar —
    //   en el log las tapas malas daban ~3 en la 1ª y 7-8 en las siguientes).
    //   Inferences 2-4: tapa garantizada quieta → mediana de las 3.
    //   La mediana ignora un glitch aislado (pico espurio en tapa buena) pero
    //   rechaza cuando la anomalía es sostenida (tapa mala real).
    case State::INSPECTING: {
        // Timeout de seguridad: 4 s sin inference → dejar pasar la tapa
        if (now - stopTime > 4000) {
            Serial.println("[FSM] TIMEOUT inferencia — tapa descartada");
            capBad = false;
            peripherals.motorRun();
            enterState(State::RELEASING);
            break;
        }

        uint32_t infTs = classifier.getLastScoreTime();
        if (infTs > lastInfTs) {
            lastInfTs = infTs;
            inferCount++;
            float score = classifier.getAnomalyScore();

            if (inferCount == 1) {
                Serial.printf("[FSM] Inference 1/4  score=%.4f (descartada: frame pre-parada)\n", score);
            } else {
                infScores[inferCount - 2] = score;
                Serial.printf("[FSM] Inference %d/4  score=%.4f\n", inferCount, score);
            }

            if (inferCount >= 4) {
                // Mediana de 3: ordenar y tomar el del medio
                float a = infScores[0], b = infScores[1], c = infScores[2];
                float median = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
                capBad = median >= ANOMALY_THRESHOLD;
                Serial.printf("[Decision] mediana=%.4f (%.2f, %.2f, %.2f)  %s\n",
                              median, a, b, c, capBad ? "ANOMALIA" : "BUENA");

                if (capBad) peripherals.servoReject();
                peripherals.motorRun();
                enterState(State::RELEASING);
            }
        }
        break;
    }

    // ── RELEASING: banda corriendo, tapa viajando cámara→servo ───────────────
    case State::RELEASING:
        if (now - stateStart >= CAMERA_TO_SERVO_MS) {
            if (capBad) peripherals.servoPass();
            enterState(State::COOLDOWN);
        }
        break;

    // ── COOLDOWN: servo resetado, esperando que el sensor se libere ───────────
    //   Evita que la misma tapa (o una muy pegada) dispare un nuevo ciclo.
    case State::COOLDOWN:
        if (!peripherals.isCapPresent()) {
            enterState(State::IDLE);
        }
        break;
    }

    // IP cada 10 s
    static uint32_t lastPrint = 0;
    if (now - lastPrint > 10000) {
        lastPrint = now;
        if (WiFi.status() == WL_CONNECTED)
            Serial.printf("[IP] http://%s\n", WiFi.localIP().toString().c_str());
        else
            Serial.println("[WiFi] Sin conexion");
    }

    delay(5);
}
