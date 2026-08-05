# Arquitectura del sistema

## Flujo actual

```text
Sensor IR
   │
   ▼
Detección de tapa
   │
   ├──────────────► Captura OV2640
   │                    │
   │                    ▼
   │              Preprocesamiento EI
   │                    │
   │                    ▼
   │              Inferencia/anomaly score
   │                    │
   ▼                    ▼
Lógica de decisión ── buena / anomalía
   │
   ├── Buena: paso libre
   └── Anomalía: accionamiento del servo
```

## Distribución de tareas

### Core 0

- Captura de imagen.
- Conversión y redimensionamiento.
- Ejecución del modelo de Edge Impulse.
- Actualización thread-safe del anomaly score.

### Core 1

- Servidor HTTP.
- Vista previa de la cámara y del frame de inferencia.
- Lectura del sensor.
- Decisión física de aceptación o rechazo.
- Control del motor, iluminación y servo.

## Sincronización

El buffer de imagen se protege mediante mutex porque el controlador de cámara y el frame compartido no deben utilizarse simultáneamente desde ambos núcleos.

## Evolución recomendada

Para una versión con banda continua:

```text
Sensor/encoder → ID de pieza → captura sincronizada
       │                              │
       └──────── posición ────────────┘
                                      ▼
                                 inferencia
                                      │
                                      ▼
                           cola de piezas/decisiones
                                      │
                                      ▼
                          expulsor sincronizado
```

Cada detección debe convertirse en un evento con, al menos:

- Identificador de pieza.
- Posición o contador del encoder.
- Marca de tiempo.
- Resultado de inferencia.
- Posición prevista de rechazo.
