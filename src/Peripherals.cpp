#include "Peripherals.h"

// ── Servo via LEDC (sin ESP32Servo — funciona en cualquier GPIO del S3) ────────
// Servo estándar: pulso 1000–2000 µs a 50 Hz (período 20 ms)
// Con resolución 16 bits: duty = us * 65535 / 20000
// Canales 0-1 comparten LEDC_TIMER_0 con la cámara (XCLK 10 MHz)
// Canal 4 usa LEDC_TIMER_2 → sin conflicto
#define SERVO_LEDC_CH   4
#define SERVO_LEDC_BITS 13                // 2^13 = 8192 pasos
#define SERVO_LEDC_MAX  ((1 << SERVO_LEDC_BITS) - 1)  // 8191

static void servoWrite(int angle) {
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;
    // Servo estándar: 1000 µs = 0°, 2000 µs = 180°, período 20 000 µs
    uint32_t us   = (uint32_t)map(angle, 0, 180, 1000, 2000);
    uint32_t duty = us * SERVO_LEDC_MAX / 20000UL;
    ledcWrite(SERVO_LEDC_CH, duty);
}

// ── begin() ───────────────────────────────────────────────────────────────────
void Peripherals::begin() {
    // Motor — IN1 como PWM, IN2 como dirección
    pinMode(MOTOR_IN2, OUTPUT); digitalWrite(MOTOR_IN2, LOW);
    ledcSetup(MOTOR_LEDC_CH, MOTOR_LEDC_FREQ, MOTOR_LEDC_BITS);
    ledcAttachPin(MOTOR_IN1, MOTOR_LEDC_CH);
    ledcWrite(MOTOR_LEDC_CH, 0);   // arranca detenido

    // Sensor IR
    pinMode(SENSOR_PIN, INPUT_PULLUP);

    // LED de trabajo
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    // Servo — LEDC canal 1, 50 Hz, 13 bits (canal 0 reservado por cámara)
    ledcSetup(SERVO_LEDC_CH, 50, SERVO_LEDC_BITS);
    ledcAttachPin(SERVO_PIN, SERVO_LEDC_CH);
    servoWrite(SERVO_ANGLE_PASS);   // posición inicial: paso libre

    Serial.println("[Peripherals] OK");
}

// ── Motor ─────────────────────────────────────────────────────────────────────
void Peripherals::motorRun()  { digitalWrite(MOTOR_IN2, LOW); ledcWrite(MOTOR_LEDC_CH, MOTOR_SPEED); }
void Peripherals::motorStop() { ledcWrite(MOTOR_LEDC_CH, 0); }

// ── Servo ─────────────────────────────────────────────────────────────────────
void Peripherals::servoPass()   { servoWrite(SERVO_ANGLE_PASS);   }
void Peripherals::servoReject() { servoWrite(SERVO_ANGLE_REJECT); }

// ── Sensor IR ─────────────────────────────────────────────────────────────────
bool Peripherals::isCapPresent() {
    return digitalRead(SENSOR_PIN) == SENSOR_DETECTED;
}

// ── LED ───────────────────────────────────────────────────────────────────────
void Peripherals::ledOn()  { digitalWrite(LED_PIN, HIGH); }
void Peripherals::ledOff() { digitalWrite(LED_PIN, LOW);  }
