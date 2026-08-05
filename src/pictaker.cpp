// ═══════════════════════════════════════════════════════════════════════════════
//  DATASET CAPTURE TOOL — Anomaly Detection Edition
//  ESP32-S3 Cap Inspection Project
//
//  CÓMO USAR:
//    En PlatformIO, selecciona el entorno "pictaker" (barra inferior de VS Code)
//    y sube. Para volver al clasificador, selecciona "esp32-s3".
//    NO es necesario renombrar ningún archivo.
//
//  FLUJO DE TRABAJO:
//    ┌─ ENTRENAMIENTO ──────────────────────────────────────────────────────┐
//    │  Captura solo "tapa_buena" (300–600 imágenes con variedad real)      │
//    │  → Súbelas a Edge Impulse como clase "tapa_buena"                   │
//    └──────────────────────────────────────────────────────────────────────┘
//    ┌─ VALIDACIÓN (no van al entrenamiento) ───────────────────────────────┐
//    │  Captura ~50 imágenes de cada anomalía para calibrar el umbral       │
//    │  → Úsalas en el "Live classification" de EI para fijar threshold     │
//    └──────────────────────────────────────────────────────────────────────┘
//
//  Resolución: 96×96 px RGB — el tamaño exacto que verá el modelo GMM.
// ═══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "Camera.h"
#include "WiFiManager.h"

#define AP_NAME "ESP32-Dataset"
#define AP_PASS "12345678"

Camera      camera;
WebServer   server(80);
WiFiManager wifiManager;

// ── /capture_preview — preview 320×240 para encuadrar ────────────────────────
void handlePreview()
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { server.send(500, "text/plain", "Camera error"); return; }
    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

// ── /ei_preview — crop 96×96 que verá el modelo (BMP, no JPEG) ───────────────
// fmt2jpg corrompe el heap en el ESP32-S3 → se sirve como BMP sin encoder.
// BMP = header 54 bytes + píxeles BGR bottom-up. Sin malloc dinámico.
void handleEIPreview()
{
    static const uint32_t W = 96, H = 96;
    static uint8_t *bmp = (uint8_t *)heap_caps_malloc(54 + W * H * 3,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) { server.send(500, "text/plain", "Sin memoria"); return; }

    if (!camera.capture(W, H)) { server.send(500, "text/plain", "Capture error"); return; }

    // Header BMP
    const uint32_t img_size  = W * H * 3;
    const uint32_t file_size = 54 + img_size;
    memset(bmp, 0, 54);
    bmp[0]='B'; bmp[1]='M';
    bmp[2]= file_size&0xFF; bmp[3]=(file_size>>8)&0xFF;
    bmp[4]=(file_size>>16)&0xFF; bmp[5]=(file_size>>24)&0xFF;
    bmp[10]=54; bmp[14]=40;
    bmp[18]=W&0xFF; bmp[19]=(W>>8)&0xFF;
    bmp[22]=H&0xFF; bmp[23]=(H>>8)&0xFF;
    bmp[26]=1; bmp[28]=24;
    bmp[34]=img_size&0xFF; bmp[35]=(img_size>>8)&0xFF;
    bmp[36]=(img_size>>16)&0xFF; bmp[37]=(img_size>>24)&0xFF;

    // Píxeles: BMP es bottom-up, el buffer ya está en BGR
    uint8_t *src = camera.buf();
    for (uint32_t y = 0; y < H; y++)
        memcpy(bmp + 54 + (H - 1 - y) * W * 3, src + y * W * 3, W * 3);

    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    client.write(bmp, file_size);
}

// ── /capture — descarga imagen 96×96 lista para Edge Impulse ─────────────────
void handleCapture()
{
    if (!camera.capture(96, 96)) { server.send(500, "text/plain", "Capture error"); return; }
    uint8_t *jpg = nullptr; size_t jpg_len = 0;
    if (!fmt2jpg(camera.buf(), 96*96*3, 96, 96, PIXFORMAT_RGB888, 92, &jpg, &jpg_len) || !jpg) {
        server.send(500, "text/plain", "JPEG error"); return;
    }
    String label = server.arg("label");
    String n     = server.arg("n");
    if (label == "") label = "imagen";
    if (n == "")     n     = String(millis());

    server.sendHeader("Content-Disposition",
                      "attachment; filename=" + label + "." + n + ".jpg");
    server.sendHeader("Cache-Control", "no-store");
    WiFiClient client = server.client();
    server.setContentLength(jpg_len);
    server.send(200, "image/jpeg", "");
    client.write(jpg, jpg_len);
    free(jpg);
}

// ── / — página web ────────────────────────────────────────────────────────────
void handleRoot()
{
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<title>Dataset — Anomaly Detection</title>
<style>
  *{box-sizing:border-box}
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:16px;margin:0}
  h1{color:#e94560;margin-bottom:2px}
  .sub{color:#aaa;font-size:.85em;margin-bottom:16px}
  .previews{display:flex;flex-wrap:wrap;justify-content:center;gap:14px;margin-bottom:16px}
  .pbox{background:#16213e;border-radius:10px;padding:12px}
  .pbox h3{margin:0 0 8px;font-size:.85em;color:#aaa;text-transform:uppercase;letter-spacing:1px}
  img{border:2px solid #e94560;border-radius:6px;display:block}
  #cam_prev{transform:rotate(90deg);margin:40px -40px}
  #ei_prev{image-rendering:pixelated}

  /* Tabs */
  .tabs{display:flex;gap:8px;justify-content:center;margin-bottom:12px}
  .tab{padding:8px 20px;border-radius:8px;border:2px solid #333;cursor:pointer;font-size:.95em;background:#16213e}
  .tab.active{border-color:#4caf50;color:#4caf50}
  .tab.val.active{border-color:#e94560;color:#e94560}

  .panel{background:#16213e;border-radius:10px;padding:16px;max-width:440px;margin:0 auto;display:none}
  .panel.active{display:block}

  select,button{font-size:1em;padding:10px 16px;border-radius:8px;border:none;margin:6px}
  select{background:#0d0d0d;color:#eee;width:100%}
  .btn-train{background:#4caf50;color:#fff;cursor:pointer;width:100%;font-size:1.1em}
  .btn-val{background:#e94560;color:#fff;cursor:pointer;width:100%;font-size:1.1em}
  button:active{opacity:.8}

  .counts{display:flex;gap:8px;justify-content:center;flex-wrap:wrap;margin-top:10px}
  .badge{background:#0d0d0d;border-radius:6px;padding:6px 12px;font-size:.9em}
  .badge span{font-weight:bold}
  .badge.train span{color:#4caf50}
  .badge.val   span{color:#e94560}

  .auto-row{display:flex;align-items:center;gap:8px;margin-top:10px;justify-content:center}
  .auto-row label{font-size:.9em;color:#aaa}
  .auto-row input[type=number]{width:60px;padding:6px;border-radius:6px;border:none;background:#0d0d0d;color:#eee;text-align:center}
  #status{color:#aaa;font-size:.9em;margin-top:8px;min-height:1.2em}
  .tip{font-size:.78em;color:#666;margin-top:6px}
</style>
</head><body>

<h1>Dataset Capture</h1>
<p class="sub">96×96 px · lo que verá el modelo GMM</p>

<div class="previews">
  <div class="pbox">
    <h3>Cámara (320×240)</h3>
    <img id="cam_prev" src="/capture_preview" width="320" height="240">
  </div>
  <div class="pbox">
    <h3>Input modelo (96×96)</h3>
    <img id="ei_prev" src="/ei_preview" width="192" height="192">
  </div>
</div>

<!-- Tabs -->
<div class="tabs">
  <div class="tab active"      id="tab_train" onclick="switchTab('train')">🟢 Entrenamiento</div>
  <div class="tab val"         id="tab_val"   onclick="switchTab('val')">🔴 Validación</div>
</div>

<!-- Panel entrenamiento: solo tapa_buena -->
<div class="panel active" id="panel_train">
  <p class="tip">Solo tapas buenas van aquí. Varía iluminación, posición y rotación leve.</p>
  <button class="btn-train" onclick="capture('tapa_buena','train')">📷 Capturar tapa_buena</button>

  <div class="auto-row">
    <label>Auto cada</label>
    <input type="number" id="interval_train" value="2" min="1" max="10">
    <label>s</label>
    <button class="btn-train" style="width:auto;padding:8px 14px" onclick="toggleAuto('train')">▶ Auto</button>
  </div>

  <div class="counts">
    <div class="badge train">tapa_buena: <span id="c_buena">0</span></div>
  </div>
  <p id="status_train" class="tip" style="color:#4caf50"> </p>
</div>

<!-- Panel validación: anomalías para calibrar el umbral -->
<div class="panel" id="panel_val">
  <p class="tip">Estas fotos <strong>no</strong> van al entrenamiento. Son para ajustar el umbral en Edge Impulse.</p>
  <select id="label_val">
    <option value="tapa_mala">tapa_mala</option>
    <option value="sin_tapa">sin_tapa</option>
    <option value="tapa_sucia">tapa_sucia</option>
    <option value="tapa_deformada">tapa_deformada</option>
    <option value="tapa_rota">tapa_rota</option>
  </select>
  <button class="btn-val" onclick="captureVal()">📷 Capturar anomalía</button>

  <div class="auto-row">
    <label>Auto cada</label>
    <input type="number" id="interval_val" value="2" min="1" max="10">
    <label>s</label>
    <button class="btn-val" style="width:auto;padding:8px 14px" onclick="toggleAuto('val')">▶ Auto</button>
  </div>

  <div class="counts" id="val_counts"></div>
  <p id="status_val" class="tip" style="color:#e94560"> </p>
</div>

<script>
const cnt = { tapa_buena: 0 };
const cntVal = {};
let autoTimer = { train: null, val: null };

function switchTab(t) {
  document.getElementById('tab_train').classList.toggle('active', t === 'train');
  document.getElementById('tab_val').classList.toggle('active',   t === 'val');
  document.getElementById('panel_train').classList.toggle('active', t === 'train');
  document.getElementById('panel_val').classList.toggle('active',   t === 'val');
}

function download(label, n) {
  const link = document.createElement('a');
  link.href = '/capture?label=' + label + '&n=' + String(n).padStart(4,'0') + '&t=' + Date.now();
  link.download = label + '.' + String(n).padStart(4,'0') + '.jpg';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
}

function capture(label, panel) {
  cnt[label] = (cnt[label] || 0) + 1;
  download(label, cnt[label]);
  document.getElementById('c_buena').textContent = cnt['tapa_buena'];
  document.getElementById('status_train').textContent = '✓ ' + label + '.' + String(cnt[label]).padStart(4,'0') + '.jpg';
}

function captureVal() {
  const label = document.getElementById('label_val').value;
  cntVal[label] = (cntVal[label] || 0) + 1;
  download(label, cntVal[label]);
  renderValCounts();
  document.getElementById('status_val').textContent = '✓ ' + label + '.' + String(cntVal[label]).padStart(4,'0') + '.jpg';
}

function renderValCounts() {
  let h = '';
  for (const [k, v] of Object.entries(cntVal)) {
    h += `<div class="badge val">${k}: <span>${v}</span></div>`;
  }
  document.getElementById('val_counts').innerHTML = h;
}

function toggleAuto(panel) {
  if (autoTimer[panel]) {
    clearInterval(autoTimer[panel]);
    autoTimer[panel] = null;
    const btn = panel === 'train'
      ? document.querySelectorAll('#panel_train button')[1]
      : document.querySelectorAll('#panel_val button')[1];
    btn.textContent = '▶ Auto';
    return;
  }
  const secs = parseInt(document.getElementById('interval_' + panel).value) * 1000;
  autoTimer[panel] = setInterval(() => {
    if (panel === 'train') capture('tapa_buena', 'train');
    else captureVal();
  }, secs);
  const btn = panel === 'train'
    ? document.querySelectorAll('#panel_train button')[1]
    : document.querySelectorAll('#panel_val button')[1];
  btn.textContent = '⏹ Stop';
}

// Refrescar previews
setInterval(() => {
  document.getElementById('cam_prev').src = '/capture_preview?t=' + Date.now();
}, 600);
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
    Serial.println("\n=== DATASET CAPTURE — Anomaly Detection ===");

    if (!camera.init()) {
        Serial.println("[FATAL] Camara");
        while (true) delay(1000);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    wifiManager.registerRoutes(server);
    server.on("/",                handleRoot);
    server.on("/capture_preview", handlePreview);
    server.on("/ei_preview",      handleEIPreview);
    server.on("/capture",         handleCapture);
    server.onNotFound([]{ server.send(404, "text/plain", "Not found"); });
    server.begin();

    wifiManager.connect(server, AP_NAME, AP_PASS);
    Serial.println("[OK] Listo — abre la IP en el navegador");
}

void loop()
{
    server.handleClient();
    delay(5);
}
