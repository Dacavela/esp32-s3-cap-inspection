#pragma once
#include <Arduino.h>

// ── Pines ─────────────────────────────────────────────────────────────────────
// GPIO35/36/37 = líneas DQ4/DQ5/DQ6 del bus Octal PSRAM (ESP32-S3R8) → PROHIBIDOS
// GPIO38/39 son libres y se mantienen igual
#define LED_PIN      38   // LED de trabajo (TIP31C)
#define SERVO_PIN    39   // LEDC canal 4
#define MOTOR_IN1    41   // PWM de velocidad (LEDC canal 2) → L298N IN1
#define MOTOR_IN2    40   // dirección fija (LOW = adelante)  → L298N IN2
#define SENSOR_PIN   42   // E18-D80NK (NPN open-collector) — INPUT_PULLUP


// ── Motor PWM ─────────────────────────────────────────────────────────────────
// Canal 2 → LEDC_TIMER_1 (independiente de cámara en TIMER_0 y servo en TIMER_2)
#define MOTOR_LEDC_CH    2
#define MOTOR_LEDC_FREQ  25000  // 25 kHz — inaudible, TB6612FNG aguanta hasta 100 kHz
#define MOTOR_LEDC_BITS  8      // 0–255
#define MOTOR_SPEED      255    // ~90 % — más torque; bajar si la banda va muy rápido

// ── Servo: ángulos provisionales — ajustar con el sistema montado ───────────── 
#define SERVO_ANGLE_PASS    5  // tapa buena — paso libre
#define SERVO_ANGLE_REJECT  95   // tapa mala  — desvío

// ── Sensor E18-D80NK: NPN open-collector ─────────────────────────────────────
// OUTPUT LOW  = objeto detectado
// OUTPUT HIGH = sin objeto
#define SENSOR_DETECTED  LOW

class Peripherals {
public:
    // Inicializa pines y servo — llamar en setup()
    void begin();

    // Motor (una sola dirección)
    void motorRun();
    void motorStop();

    // Servo clasificador
    void servoPass();     // posición paso libre
    void servoReject();   // posición desvío

    // Sensor IR — devuelve true si hay una tapa frente al sensor
    bool isCapPresent();

    // LED de trabajo (TIP31C)
    void ledOn();
    void ledOff();
};
