// ═══════════════════════════════════════════════════════════════════════════════
//  DATASET CAPTURE — Modo banda automática
//
//  La banda corre, el sensor detecta la tapa, espera a que llegue bajo la
//  cámara, para la banda, captura la foto y reinicia. Igual que el main real
//  pero en vez de inferir, guarda la imagen.
//
//  USO:
//    Seleccionar entorno "pictaker" en PlatformIO y subir.
//    Abrir la IP en el navegador → la banda corre sola.
//    Cada tapa capturada aparece en pantalla con botón de descarga.
//
//  TIMING:
//    Ajustar SENSOR_TO_CAMERA_MS igual que en main.cpp hasta que la tapa
//    quede centrada en el frame 96×96 al parar.
// ═══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "Camera.h"
#include "WiFiManager.h"
#include "img_converters.h"

// ── Pines (igual que Peripherals.h) ──────────────────────────────────────────
#define MOTOR_IN1        41
#define MOTOR_IN2        40
#define SENSOR_PIN       42
#define MOTOR_LEDC_CH     2
#define MOTOR_LEDC_FREQ   25000
#define MOTOR_LEDC_BITS   8
#define MOTOR_SPEED      255
#define SENSOR_DETECTED  LOW

// ── Timing — ajustar físicamente ─────────────────────────────────────────────
#define SENSOR_TO_CAMERA_MS  200   // igual que en main.cpp
#define CAPTURE_HOLD_MS      600   // tiempo con banda parada tras capturar

#define AP_NAME "ESP32-Dataset"
#define AP_PASS "12345678"

// ── Motor / sensor ────────────────────────────────────────────────────────────
static void motorBegin() {
    ledcSetup(MOTOR_LEDC_CH, MOTOR_LEDC_FREQ, MOTOR_LEDC_BITS);
    ledcAttachPin(MOTOR_IN1, MOTOR_LEDC_CH);
    pinMode(MOTOR_IN2, OUTPUT);
    digitalWrite(MOTOR_IN2, LOW);
}
static void motorRun()  { ledcWrite(MOTOR_LEDC_CH, MOTOR_SPEED); }
static void motorStop() { ledcWrite(MOTOR_LEDC_CH, 0); }
static bool capDetected() { return digitalRead(SENSOR_PIN) == SENSOR_DETECTED; }

// ── FSM ───────────────────────────────────────────────────────────────────────
enum class PicState { IDLE, APPROACH, CAPTURED, COOLDOWN };
static PicState  picState   = PicState::IDLE;
static uint32_t  stateStart = 0;

static const char *stateStr() {
    switch (picState) {
        case PicState::IDLE:     return "IDLE";
        case PicState::APPROACH: return "APPROACH";
        case PicState::CAPTURED: return "CAPTURED";
        case PicState::COOLDOWN: return "COOLDOWN";
    }
    return "?";
}

// ── Buffer de imagen ──────────────────────────────────────────────────────────
static uint8_t *jpgBuf   = nullptr;
static size_t   jpgLen   = 0;
static int      capCount = 0;
static bool     newImage = false;   // flag: nueva imagen disponible

// ── Instancias ────────────────────────────────────────────────────────────────
Camera      camera;
WebServer   server(80);
WiFiManager wifiManager;

// ── Captura ───────────────────────────────────────────────────────────────────
static void doCapture() {
    if (!camera.capture(96, 96)) {
        Serial.println("[Pictaker] capture() falló");
        return;
    }
    if (jpgBuf) { free(jpgBuf); jpgBuf = nullptr; jpgLen = 0; }

    bool ok = fmt2jpg(camera.buf(), 96 * 96 * 3,
                      96, 96, PIXFORMAT_RGB888, 92,
                      &jpgBuf, &jpgLen);
    if (ok && jpgBuf) {
        capCount++;
        newImage = true;
        Serial.printf("[Pictaker] Captura #%d  (%u bytes)\n", capCount, (unsigned)jpgLen);
    } else {
        Serial.println("[Pictaker] fmt2jpg falló");
    }
}

// ── Handlers HTTP ─────────────────────────────────────────────────────────────

// /preview — live 320×240 para encuadrar
void handlePreview() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { server.send(503, "text/plain", "Camera error"); return; }
    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

// /ei_preview — crop 96×96 que verá el modelo (BMP)
void handleEIPreview() {
    static const uint32_t W = 96, H = 96;
    static uint8_t *bmp = (uint8_t *)heap_caps_malloc(
        54 + W * H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) { server.send(500, "text/plain", "Sin memoria"); return; }

    if (!camera.capture(W, H)) { server.send(503, "text/plain", "Capture error"); return; }

    const uint32_t img_size  = W * H * 3;
    const uint32_t file_size = 54 + img_size;
    memset(bmp, 0, 54);
    bmp[0]='B'; bmp[1]='M';
    bmp[2]=file_size&0xFF;       bmp[3]=(file_size>>8)&0xFF;
    bmp[4]=(file_size>>16)&0xFF; bmp[5]=(file_size>>24)&0xFF;
    bmp[10]=54; bmp[14]=40;
    bmp[18]=W&0xFF; bmp[19]=(W>>8)&0xFF;
    bmp[22]=H&0xFF; bmp[23]=(H>>8)&0xFF;
    bmp[26]=1; bmp[28]=24;
    bmp[34]=img_size&0xFF;       bmp[35]=(img_size>>8)&0xFF;
    bmp[36]=(img_size>>16)&0xFF; bmp[37]=(img_size>>24)&0xFF;

    uint8_t *src = camera.buf();
    for (uint32_t y = 0; y < H; y++)
        memcpy(bmp + 54 + (H - 1 - y) * W * 3, src + y * W * 3, W * 3);

    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    client.write(bmp, file_size);
}

// /latest — última imagen capturada (para mostrar en web)
void handleLatest() {
    if (!jpgBuf || jpgLen == 0) {
        server.send(204, "text/plain", "Sin captura");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(jpgLen);
    server.send(200, "image/jpeg", "");
    client.write(jpgBuf, jpgLen);
}

// /download?label=tapa_buena — descarga con nombre correcto para EI
void handleDownload() {
    if (!jpgBuf || jpgLen == 0) {
        server.send(204, "text/plain", "Sin captura");
        return;
    }
    String label = server.arg("label");
    if (label == "") label = "tapa_buena";
    String n = String(capCount);
    while (n.length() < 4) n = "0" + n;

    server.sendHeader("Content-Disposition",
        "attachment; filename=" + label + "." + n + ".jpg");
    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(jpgLen);
    server.send(200, "image/jpeg", "");
    client.write(jpgBuf, jpgLen);
}

// /status — JSON con estado de la FSM y contador
void handleStatus() {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"count\":%d,\"new_image\":%s}",
        stateStr(), capCount, newImage ? "true" : "false");
    newImage = false;   // consumido
    server.send(200, "application/json", buf);
}

// / — página principal
void handleRoot() {
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<title>Dataset — Banda Automática</title>
<style>
  *{box-sizing:border-box}
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:16px;margin:0}
  h1{color:#e94560;margin-bottom:2px}
  .sub{color:#aaa;font-size:.85em;margin-bottom:14px}

  .previews{display:flex;flex-wrap:wrap;justify-content:center;gap:14px;margin-bottom:14px}
  .pbox{background:#16213e;border-radius:10px;padding:12px}
  .pbox h3{margin:0 0 8px;font-size:.8em;color:#aaa;text-transform:uppercase;letter-spacing:1px}
  img{border:2px solid #444;border-radius:6px;display:block}
  #cam_prev{transform:rotate(90deg);margin:40px -40px}
  #ei_prev{image-rendering:pixelated}
  #last_cap{image-rendering:pixelated;border-color:#4caf50}

  .status-bar{display:flex;align-items:center;justify-content:center;gap:16px;
    background:#16213e;border-radius:10px;padding:12px 20px;margin-bottom:14px;
    max-width:480px;margin-left:auto;margin-right:auto}
  .badge{padding:5px 14px;border-radius:20px;font-weight:bold;font-size:.9em}
  .IDLE    {background:#333;color:#aaa}
  .APPROACH{background:#f57c00;color:#fff}
  .CAPTURED{background:#4caf50;color:#fff}
  .COOLDOWN{background:#1565c0;color:#fff}

  .count{font-size:1.8em;font-weight:bold;color:#4caf50}
  .count-label{font-size:.8em;color:#aaa}

  .ctrl{background:#16213e;border-radius:10px;padding:14px;
    max-width:480px;margin:0 auto 14px}
  select,button{font-size:1em;padding:9px 14px;border-radius:8px;border:none;margin:5px}
  select{background:#0d0d0d;color:#eee;width:100%}
  .btn-dl{background:#4caf50;color:#fff;cursor:pointer;width:100%}
  .btn-dl:disabled{background:#333;color:#666;cursor:default}
  .auto-row{display:flex;align-items:center;gap:8px;margin-top:8px;justify-content:center}
  .auto-row label{font-size:.85em;color:#aaa}
  #tip{font-size:.8em;color:#666;margin-top:8px}
</style>
</head><body>

<h1>Dataset — Banda Automática</h1>
<p class="sub">La banda corre sola · cada tapa activa el sensor y se captura</p>

<div class="previews">
  <div class="pbox">
    <h3>Cámara live (320×240)</h3>
    <img id="cam_prev" src="/preview" width="320" height="240">
  </div>
  <div class="pbox">
    <h3>Input modelo live (96×96)</h3>
    <img id="ei_prev" src="/ei_preview" width="192" height="192">
  </div>
  <div class="pbox">
    <h3>Última captura</h3>
    <img id="last_cap" src="/latest" width="192" height="192"
         onerror="this.style.opacity='.2'">
  </div>
</div>

<div class="status-bar">
  <div>
    <div id="state_badge" class="badge IDLE">IDLE</div>
  </div>
  <div>
    <div class="count" id="counter">0</div>
    <div class="count-label">capturas</div>
  </div>
</div>

<div class="ctrl">
  <select id="label_sel">
    <option value="tapa_buena">tapa_buena (entrenamiento)</option>
    <option value="tapa_mala">tapa_mala (validación)</option>
    <option value="tapa_sucia">tapa_sucia (validación)</option>
    <option value="tapa_deformada">tapa_deformada (validación)</option>
    <option value="sin_tapa">sin_tapa (validación)</option>
  </select>

  <button class="btn-dl" id="btn_dl" onclick="downloadLast()" disabled>
    ⬇ Descargar última captura
  </button>

  <div class="auto-row">
    <label>
      <input type="checkbox" id="auto_dl"> Auto-descargar cada captura
    </label>
  </div>
  <p id="tip">Alimenta tapas en la banda · el sistema captura automáticamente</p>
</div>

<script>
let lastCount = 0;

function downloadLast() {
  const label = document.getElementById('label_sel').value;
  const n     = String(lastCount).padStart(4, '0');
  const link  = document.createElement('a');
  link.href     = '/download?label=' + label + '&t=' + Date.now();
  link.download = label + '.' + n + '.jpg';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
}

// Polling de estado: cada 300 ms
setInterval(() => {
  fetch('/status')
    .then(r => r.json())
    .then(d => {
      // Estado FSM
      const badge = document.getElementById('state_badge');
      badge.textContent = d.state;
      badge.className   = 'badge ' + d.state;

      // Contador
      document.getElementById('counter').textContent = d.count;

      // Nueva imagen
      if (d.new_image) {
        const t = Date.now();
        document.getElementById('last_cap').src = '/latest?t=' + t;
        document.getElementById('btn_dl').disabled = false;
        lastCount = d.count;

        if (document.getElementById('auto_dl').checked) {
          setTimeout(downloadLast, 200);   // pequeño delay para que el buffer esté listo
        }
      }
    })
    .catch(() => {});
}, 300);

// Live preview — solo cuando está IDLE o COOLDOWN (banda corriendo)
setInterval(() => {
  document.getElementById('cam_prev').src = '/preview?t=' + Date.now();
}, 700);
setInterval(() => {
  document.getElementById('ei_prev').src = '/ei_preview?t=' + Date.now();
}, 900);
</script>
</body></html>
)rawliteral");
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== PICTAKER — Banda Automática ===");

    motorBegin();
    pinMode(SENSOR_PIN, INPUT_PULLUP);

    if (!camera.init()) {
        Serial.println("[FATAL] Camara");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    wifiManager.registerRoutes(server);
    server.on("/",           handleRoot);
    server.on("/preview",    handlePreview);
    server.on("/ei_preview", handleEIPreview);
    server.on("/latest",     handleLatest);
    server.on("/download",   handleDownload);
    server.on("/status",     handleStatus);
    server.onNotFound([]{ server.send(404, "text/plain", "Not found"); });
    server.begin();

    wifiManager.connect(server, AP_NAME, AP_PASS);

    motorRun();
    Serial.println("[OK] Banda corriendo — abre la IP en el navegador");
}

// ── Loop — FSM ────────────────────────────────────────────────────────────────
void loop()
{
    server.handleClient();
    uint32_t now = millis();

    switch (picState) {

    // Banda corriendo, esperando tapa en el sensor
    case PicState::IDLE:
        if (capDetected()) {
            stateStart = now;
            picState   = PicState::APPROACH;
            Serial.println("[FSM] IDLE → APPROACH");
        }
        break;

    // Tapa detectada: dejar que recorra sensor→cámara antes de parar
    case PicState::APPROACH:
        if (now - stateStart >= SENSOR_TO_CAMERA_MS) {
            motorStop();
            doCapture();
            stateStart = now;
            picState   = PicState::CAPTURED;
            Serial.println("[FSM] APPROACH → CAPTURED");
        }
        break;

    // Banda parada, imagen capturada — mantener parada un momento
    // para que el usuario pueda verla en la web antes de que avance
    case PicState::CAPTURED:
        if (now - stateStart >= CAPTURE_HOLD_MS) {
            motorRun();
            stateStart = now;
            picState   = PicState::COOLDOWN;
            Serial.println("[FSM] CAPTURED → COOLDOWN");
        }
        break;

    // Esperar que el sensor se libere antes de aceptar la siguiente tapa
    case PicState::COOLDOWN:
        if (!capDetected()) {
            picState = PicState::IDLE;
            Serial.println("[FSM] COOLDOWN → IDLE");
        }
        break;
    }

    delay(5);
}
