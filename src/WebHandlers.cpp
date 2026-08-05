#include "WebHandlers.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <Arduino.h>

// ── Constructor ───────────────────────────────────────────────────────────────
WebHandlers::WebHandlers(Camera& camera, Classifier& classifier,
                         WebServer& server, Peripherals& peripherals, float threshold)
    : _camera(camera), _classifier(classifier),
      _server(server), _peripherals(peripherals), _threshold(threshold)
{}

// ── registerRoutes() ──────────────────────────────────────────────────────────
void WebHandlers::registerRoutes() {
    _server.on("/",         [this]() { onRoot();     });
    _server.on("/snapshot", [this]() { onSnapshot(); });
    _server.on("/result",   [this]() { onResult();   });
    _server.on("/ei_frame", [this]() { onEIFrame();  });
}

// ── /snapshot — frame JPEG 320×240 ───────────────────────────────────────────
void WebHandlers::onSnapshot() {
    // esp_camera_fb_get NO es thread-safe entre cores → serializar con mutex
    if (!_camera.lock(500)) {
        _server.send(503, "text/plain", "Camara ocupada");
        return;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        _camera.unlock();
        _server.send(500, "text/plain", "Camera error");
        return;
    }
    WiFiClient client = _server.client();
    _server.setContentLength(fb->len);
    _server.send(200, "image/jpeg", "");
    client.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    _camera.unlock();
}

// ── /result — anomaly score actual en JSON ────────────────────────────────────
void WebHandlers::onResult() {
    float score   = _classifier.getAnomalyScore();
    bool  ok      = score < _threshold;
    bool  capPresent = _peripherals.isCapPresent();

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"anomaly_score\":%.4f,\"threshold\":%.4f,\"estado\":\"%s\",\"cap_present\":%s}",
             score, _threshold, ok ? "tapa_buena" : "anomalia",
             capPresent ? "true" : "false");
    _server.send(200, "application/json", buf);
}

// ── /ei_frame — crop 96×96 que ve el modelo (BMP, sin encoder JPEG) ──────────
// fmt2jpg corrompía el heap en el ESP32-S3 → se sirve como BMP raw.
// Buffers estáticos en PSRAM: se alocan una sola vez, cero malloc por request.
void WebHandlers::onEIFrame() {
    static const uint32_t W = 96, H = 96;
    static uint8_t *ei_copy = (uint8_t *)heap_caps_malloc(
                                  W * H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static uint8_t *bmp     = (uint8_t *)heap_caps_malloc(
                                  54 + W * H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ei_copy || !bmp) {
        _server.send(500, "text/plain", "Sin memoria");
        return;
    }

    if (!_camera.copyFrame(ei_copy, W * H * 3)) {
        _server.send(503, "text/plain", "Sin frame");
        return;
    }

    // Header BMP (BITMAPFILEHEADER + BITMAPINFOHEADER, 54 bytes)
    const uint32_t img_size  = W * H * 3;
    const uint32_t file_size = 54 + img_size;
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2]  = file_size        & 0xFF; bmp[3]  = (file_size >> 8)  & 0xFF;
    bmp[4]  = (file_size >> 16)& 0xFF; bmp[5]  = (file_size >> 24) & 0xFF;
    bmp[10] = 54;   // offset píxeles
    bmp[14] = 40;   // tamaño BITMAPINFOHEADER
    bmp[18] = W & 0xFF; bmp[19] = (W >> 8) & 0xFF;
    bmp[22] = H & 0xFF; bmp[23] = (H >> 8) & 0xFF;  // positivo = bottom-up
    bmp[26] = 1;    // planes
    bmp[28] = 24;   // bits/píxel
    bmp[34] = img_size        & 0xFF; bmp[35] = (img_size >> 8)  & 0xFF;
    bmp[36] = (img_size >> 16)& 0xFF; bmp[37] = (img_size >> 24) & 0xFF;

    // Píxeles: BMP bottom-up, buffer ya en BGR → solo invertir filas
    for (uint32_t y = 0; y < H; y++)
        memcpy(bmp + 54 + (H - 1 - y) * W * 3, ei_copy + y * W * 3, W * 3);

    WiFiClient client = _server.client();
    _server.setContentLength(file_size);
    _server.send(200, "image/bmp", "");
    client.write(bmp, file_size);
}

// ── / — página de monitoreo ───────────────────────────────────────────────────
void WebHandlers::onRoot() {
    _server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<title>Anomaly Detection — Tapas</title>
<style>
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;text-align:center;padding:20px;margin:0}
  h1{color:#e94560;margin-bottom:4px}
  .cards{display:flex;flex-wrap:wrap;justify-content:center;gap:16px}
  .card{background:#16213e;border-radius:12px;padding:16px}
  .card h3{margin:0 0 10px;font-size:.95em;color:#aaa;text-transform:uppercase;letter-spacing:1px}
  img{border:3px solid #e94560;border-radius:8px;display:block}
  #cam{transform:rotate(90deg);margin:40px -40px}
  #ei{image-rendering:pixelated;transform:rotate(90deg)}
  #estado{font-size:2em;font-weight:bold;margin:12px 0 4px;transition:color .3s}
  #score-val{font-size:1.3em;margin:0 0 12px;color:#aaa}
  .gauge-wrap{background:#0d0d0d;border-radius:8px;overflow:hidden;height:28px;position:relative}
  .gauge-fill{height:28px;width:0%;transition:width .4s,background .4s;border-radius:8px}
  .threshold-line{position:absolute;top:0;height:28px;width:2px;background:#fff;opacity:.6}
  .gauge-label{display:flex;justify-content:space-between;font-size:.8em;color:#888;margin-top:3px}
</style>
</head><body>
<h1>Anomaly Detection — Tapas</h1>

<div class="cards">
  <div class="card">
    <h3>Cámara (320×240)</h3>
    <img id="cam" src="/snapshot" width="320" height="240">
  </div>
  <div class="card">
    <h3>Input EI (96×96)</h3>
    <img id="ei" src="/ei_frame" width="192" height="192">
  </div>
</div>

<div class="card" style="max-width:400px;margin:16px auto">
  <p id="estado">---</p>
  <p id="score-val">Score: —</p>
  <div class="gauge-wrap">
    <div class="gauge-fill" id="gauge"></div>
    <div class="threshold-line" id="tline"></div>
  </div>
  <div class="gauge-label"><span>0 (normal)</span><span>alto (anomalía)</span></div>
</div>

<script>
const MAX_SCORE = 12.0;

// Score: se actualiza cada 600 ms
// Si hay tapa presente, también refresca las imágenes
setInterval(() => {
  fetch('/result')
    .then(r => r.json())
    .then(d => {
      const ok   = d.estado === 'tapa_buena';
      const pct  = Math.min(d.anomaly_score / MAX_SCORE * 100, 100);
      const tpct = Math.min(d.threshold     / MAX_SCORE * 100, 100);
      document.getElementById('estado').textContent    = ok ? '✅ Tapa OK' : '❌ Anomalía';
      document.getElementById('estado').style.color    = ok ? '#4caf50' : '#e94560';
      document.getElementById('score-val').textContent =
        `Score: ${d.anomaly_score.toFixed(4)}  (umbral: ${d.threshold.toFixed(2)})`;
      document.getElementById('gauge').style.width      = pct  + '%';
      document.getElementById('gauge').style.background = ok ? '#4caf50' : '#e94560';
      document.getElementById('tline').style.left       = tpct + '%';

      // Refrescar imágenes solo cuando hay tapa bajo la cámara
      if (d.cap_present) {
        const t = Date.now();
        document.getElementById('cam').src = '/snapshot?t=' + t;
        document.getElementById('ei').src  = '/ei_frame?t=' + t;
      }
    })
    .catch(() => {});
}, 600);
</script>
</body></html>
)rawliteral");
}
