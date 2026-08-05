# ESP32-S3 Cap Inspection

Sistema embebido de inspección visual para detectar anomalías en tapas mediante una cámara OV2640, un modelo de Edge Impulse y un ESP32-S3.

> Estado: prototipo funcional en desarrollo. El repositorio se mantiene privado mientras se valida el rendimiento y se prepara una posible versión comercial.

## Funciones actuales

- Captura de imágenes con ESP32-S3 Eye y OV2640.
- Inferencia de anomalías con un modelo de Edge Impulse.
- Ejecución separada por núcleos:
  - Core 0: captura e inferencia.
  - Core 1: servidor web y control de periféricos.
- Detección de tapas mediante sensor infrarrojo.
- Accionamiento de motor y servo para aceptar o rechazar piezas.
- Panel web con:
  - Vista de la cámara.
  - Imagen procesada que recibe el modelo.
  - Anomaly score.
  - Umbral de decisión.
- Configuración Wi-Fi mediante un punto de acceso temporal.
- Uso de PSRAM y mutex para compartir imágenes de forma segura entre tareas.

## Hardware

- ESP32-S3 Eye.
- Cámara OV2640.
- Sensor infrarrojo E18-D80NK o equivalente.
- Servo para el mecanismo de rechazo.
- Motor y driver de motor para la banda transportadora.
- Iluminación LED controlada.
- Fuente de alimentación adecuada para lógica, motor y servo.

## Estructura recomendada

```text
.
├── include/
│   ├── Camera.h
│   ├── Classifier.h
│   ├── Peripherals.h
│   └── WiFiManager.h
├── src/
│   ├── main.cpp
│   ├── Camera.cpp
│   ├── Classifier.cpp
│   ├── Peripherals.cpp
│   └── WiFiManager.cpp
├── lib/
│   └── <edge-impulse-project>_inferencing/
├── docs/
│   ├── ARCHITECTURE.md
│   └── ESP_NN_NOTES.md
├── platformio.ini
├── .gitignore
└── README.md
```

La estructura real puede variar. No muevas archivos antes de comprobar cómo están declaradas las rutas de inclusión en PlatformIO.

## Compilación

### Requisitos

- Visual Studio Code.
- Extensión PlatformIO.
- Git.
- Toolchain y plataforma ESP32 configurados por `platformio.ini`.
- Librería de inferencia exportada desde Edge Impulse.

### Pasos

1. Clonar el repositorio.
2. Abrir la carpeta raíz en Visual Studio Code.
3. Esperar a que PlatformIO instale las dependencias.
4. Conectar el ESP32-S3.
5. Compilar:

```bash
pio run
```

6. Cargar el firmware:

```bash
pio run --target upload
```

7. Abrir el monitor serie:

```bash
pio device monitor
```

## Configuración

Los valores que deben calibrarse para cada montaje incluyen:

- Umbral de anomalía.
- Número de inferencias utilizadas para la decisión.
- Ángulos del servo.
- Tiempo de retención del servo.
- Parámetros de exposición e iluminación.
- Posiciones relativas del sensor, cámara y expulsor.
- Velocidad de la banda.

No publiques contraseñas Wi-Fi, tokens, claves de API ni archivos de credenciales. Utiliza un archivo local ignorado por Git cuando sea necesario.

## Modelo de Edge Impulse

El repositorio debe conservar la versión exacta de la librería de inferencia con la que se validó el firmware.

La librería actual contiene modificaciones relacionadas con ESP-NN para el ESP32-S3. Consulta [`docs/ESP_NN_NOTES.md`](docs/ESP_NN_NOTES.md) antes de reemplazarla por una nueva exportación.

## Validación mínima

Antes de considerar una versión estable:

- Comparar la misma imagen en Edge Impulse Studio y en el ESP32-S3.
- Verificar que el anomaly score sea estable al repetir la misma entrada.
- Medir captura, DSP, inferencia y ciclo total por separado.
- Probar tapas buenas y defectuosas que no hayan sido usadas en entrenamiento.
- Ejecutar una prueba prolongada para detectar reinicios, fugas o corrupción de memoria.
- Registrar falsos positivos y falsos negativos.
- Verificar el rechazo físico de cada pieza detectada.

## Limitaciones actuales

- La OV2640 utiliza rolling shutter.
- El sistema depende de iluminación y posicionamiento controlados.
- Los parámetros del modelo están ajustados al dataset y montaje actuales.
- El prototipo todavía no representa una máquina industrial certificada.
- La velocidad máxima debe determinarse experimentalmente.

## Próximos pasos

- Automatizar el parche o actualización de ESP-NN mediante un script reproducible.
- Añadir encoder para seguimiento de piezas.
- Separar captura, decisión y rechazo mediante una cola de eventos.
- Medir throughput y precisión con un protocolo documentado.
- Evaluar iluminación estroboscópica y cámara global shutter para una versión de mayor velocidad.
- Añadir esquemas eléctricos y fotografías del montaje en `docs/`.

## Autor

David — proyecto de ingeniería mecatrónica.

## Licencia

No se incluye una licencia de código abierto por el momento. El proyecto se mantiene para desarrollo privado y evaluación comercial.
