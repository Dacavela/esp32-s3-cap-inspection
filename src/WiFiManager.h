#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>

// Gestión de WiFi con modo AP de configuración y persistencia en EEPROM.
// Algoritmo basado en el código del profesor (lab-microcon/APWifi).
//
// Flujo:
//   1. Al arrancar intenta conectar con las últimas 2 redes guardadas en EEPROM.
//   2. Si ninguna está disponible, lanza un AP y sirve un formulario en /setup.
//   3. El usuario ingresa credenciales → se guardan en EEPROM → se conecta.
//   4. connect() retorna en cuanto hay conexión; el resto del programa continúa.

class WiFiManager {
public:
    WiFiManager();

    // Registra las rutas /setup y /wifi en el servidor (llamar antes de server.begin())
    void registerRoutes(WebServer &server);

    // Intenta conectar. Bloquea hasta tener WiFi.
    // server debe estar iniciado (server.begin()) antes de llamar esto.
    void connect(WebServer &server, const char *apName, const char *apPass);

private:
    static WiFiManager *_instance;
    WebServer          *_server;

    // ── EEPROM (mismo layout que el código del profesor) ──────────────────
    // Posición 0..49  : SSID red A     | 100..149 : password red A
    // Posición 50..99 : SSID red B     | 150..199 : password red B
    // Posición 300    : indicador ("a" → próxima escritura en pos 0, "b" → pos 50)
    static String _readStr(int addr);
    static void   _writeStr(int addr, const String &s);

    bool _tryConnect(const String &ssid, const String &pass, int timeoutSec);
    bool _tryLastNetworks();
    void _saveCredentials(const String &ssid, const String &pass);

    // Handlers estáticos (usan _instance para acceder al server)
    static void _handleSetup();   // GET  /setup → formulario HTML
    static void _handleWifi();    // POST /wifi  → guarda y conecta
};
