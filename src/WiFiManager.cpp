#include "WiFiManager.h"

WiFiManager *WiFiManager::_instance = nullptr;

WiFiManager::WiFiManager() : _server(nullptr) {
    _instance = this;
    // EEPROM.begin() NO va aquí: los constructores globales corren ANTES de que
    // Arduino inicialice NVS, así que la llamada fallaría con NVS_NOT_INITIALIZED.
    // Se hace en connect(), que se llama desde setup().
}

// ── Registro de rutas ─────────────────────────────────────────────────────────
void WiFiManager::registerRoutes(WebServer &server) {
    _server = &server;
    server.on("/setup", HTTP_GET,  _handleSetup);
    server.on("/wifi",  HTTP_POST, _handleWifi);
}

// ── connect() — bloquea hasta tener WiFi ─────────────────────────────────────
void WiFiManager::connect(WebServer &server, const char *apName, const char *apPass) {
    _server = &server;
    EEPROM.begin(512);  // aquí NVS ya está inicializado por Arduino

    if (_tryLastNetworks()) {
        Serial.printf("[WiFi] Conectado: http://%s\n",
                      WiFi.localIP().toString().c_str());
        return;
    }

    // Sin red guardada o ninguna disponible → lanzar AP de configuración
    WiFi.mode(WIFI_AP_STA);  // AP activo + STA listo para conectar
    WiFi.softAP(apName, apPass);
    Serial.printf("[WiFi] AP '%s' activo. Configurar en: http://192.168.4.1/setup\n", apName);

    // Bloqueamos aquí, sirviendo la página de configuración hasta conectar
    // TIMEOUT: máximo 5 minutos sin conexión → continuar sin WiFi para evitar hang infinito
    unsigned long start_ms = millis();
    const unsigned long WIFI_TIMEOUT_MS = 5 * 60 * 1000;  // 5 minutos

    while (WiFi.status() != WL_CONNECTED) {
        server.handleClient();
        delay(5);

        // Check timeout
        if (millis() - start_ms > WIFI_TIMEOUT_MS) {
            Serial.println("[WiFi] TIMEOUT: pasaron 5 min sin conexión. Continuando sin WiFi...");
            Serial.printf("[WiFi] Accesible solo en http://192.168.4.1 (AP)\n");
            // Dejar el AP activo, continuar sin desactivarlo
            return;
        }
    }

    // Ya conectado → desactivar AP y quedar solo en modo estación
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);

    Serial.printf("[WiFi] Conectado: http://%s\n",
                  WiFi.localIP().toString().c_str());
}

// ── EEPROM helpers ────────────────────────────────────────────────────────────
String WiFiManager::_readStr(int addr) {
    String s = "";
    for (int i = 0; i < 100; i++) {
        char c = (char)EEPROM.read(addr + i);
        if (c == '\0') break;
        s += c;
    }
    return s;
}

void WiFiManager::_writeStr(int addr, const String &s) {
    for (int i = 0; i < (int)s.length(); i++)
        EEPROM.write(addr + i, (uint8_t)s[i]);
    EEPROM.write(addr + s.length(), '\0');
    EEPROM.commit();
}

// ── Intento de conexión ───────────────────────────────────────────────────────
bool WiFiManager::_tryConnect(const String &ssid, const String &pass, int timeoutSec) {
    if (ssid.length() == 0) return false;
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    for (int i = 0; i < timeoutSec && WiFi.status() != WL_CONNECTED; i++)
        delay(1000);
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiManager::_tryLastNetworks() {
    WiFi.mode(WIFI_STA);
    // Prueba las 2 posiciones guardadas (pos 0 y pos 50, igual que el código del profesor)
    for (int pos = 0; pos <= 50; pos += 50) {
        String ssid = _readStr(0   + pos);
        String pass = _readStr(100 + pos);
        if (ssid.length() == 0) continue;
        Serial.printf("[WiFi] Probando EEPROM[%d]: '%s'\n", pos, ssid.c_str());
        if (_tryConnect(ssid, pass, 5)) return true;
    }
    return false;
}

void WiFiManager::_saveCredentials(const String &ssid, const String &pass) {
    // Alterna entre posición 0 y 50 para guardar las dos últimas redes
    String toggle = _readStr(300);
    int pos = (toggle == "a") ? 0 : 50;
    _writeStr(300,       (toggle == "a") ? "b" : "a");
    _writeStr(0   + pos, ssid);
    _writeStr(100 + pos, pass);
    Serial.printf("[WiFi] Credenciales guardadas en EEPROM[%d]\n", pos);
}

// ── Handler: GET /setup — formulario de configuración ────────────────────────
void WiFiManager::_handleSetup() {
    if (!_instance || !_instance->_server) return;
    _instance->_server->send(200, "text/html", R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<title>Configurar WiFi</title>
<style>
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;
       display:flex;flex-direction:column;align-items:center;
       justify-content:center;min-height:100vh;margin:0}
  h2{color:#e94560;margin-bottom:24px}
  input{display:block;margin:8px 0;padding:10px 14px;width:260px;
        border-radius:8px;border:none;font-size:1em}
  button{margin-top:18px;padding:10px 28px;background:#e94560;color:#fff;
         border:none;border-radius:8px;font-size:1em;cursor:pointer}
  .hint{font-size:.85em;color:#aaa;margin-top:10px}
</style></head><body>
<h2>Configurar Red WiFi</h2>
<form method="POST" action="/wifi">
  <input type="text"     name="ssid"     placeholder="Nombre de la red (SSID)">
  <input type="password" name="password" placeholder="Contraseña">
  <button type="submit">Conectar</button>
</form>
<p class="hint">Conectado al AP · IP: 192.168.4.1</p>
</body></html>
)rawliteral");
}

// ── Handler: POST /wifi — procesa credenciales ────────────────────────────────
void WiFiManager::_handleWifi() {
    if (!_instance || !_instance->_server) return;
    WebServer &srv = *_instance->_server;

    String ssid = srv.arg("ssid");
    String pass = srv.arg("password");
    Serial.printf("[WiFi] Intentando conectar a '%s'...\n", ssid.c_str());

    if (_instance->_tryConnect(ssid, pass, 10)) {
        _instance->_saveCredentials(ssid, pass);
        String ip = WiFi.localIP().toString();
        srv.send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<style>body{font-family:Arial;background:#1a1a2e;color:#eee;"
            "text-align:center;padding:60px}h2{color:#4CAF50}"
            "a{color:#e94560}</style></head><body>"
            "<h2>Conectado</h2>"
            "<p>IP: <strong>" + ip + "</strong></p>"
            "<p><a href='http://" + ip + "'>Abrir clasificador &rarr;</a></p>"
            "</body></html>");
        // WiFi.status() == WL_CONNECTED → el while en connect() saldrá
    } else {
        srv.send(200, "text/html",
            "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
            "<style>body{font-family:Arial;background:#1a1a2e;color:#eee;"
            "text-align:center;padding:60px}h2{color:#e94560}"
            "a{color:#e94560}</style></head><body>"
            "<h2>No se pudo conectar</h2>"
            "<p>Verifica el nombre y contraseña.</p>"
            "<p><a href='/setup'>Intentar de nuevo</a></p>"
            "</body></html>");
    }
}
