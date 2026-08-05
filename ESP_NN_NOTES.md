# Notas sobre Edge Impulse y ESP-NN

## Contexto

Durante las pruebas, la librería de inferencia exportada por Edge Impulse produjo resultados de inferencia incorrectos al utilizar las optimizaciones ESP-NN del ESP32-S3.

La copia actualmente validada de la librería contiene:

- Archivos de ESP-NN actualizados con una versión de Espressif.
- Archivos destinados al ESP32-P4 reemplazados por stubs para evitar su inclusión en la compilación del ESP32-S3.

## Regla importante

No reemplazar directamente la carpeta de inferencia por una nueva exportación de Edge Impulse sin:

1. Guardar la versión funcional.
2. Comparar los cambios.
3. Reaplicar o actualizar el parche.
4. Compilar desde cero.
5. Validar la misma imagen en Studio y en el dispositivo.

## Prueba A/B recomendada

Comparar tres configuraciones con la misma imagen:

1. Edge Impulse Studio.
2. ESP32-S3 con kernels de referencia.
3. ESP32-S3 con ESP-NN parcheado.

Los resultados no tienen que ser idénticos bit a bit, pero deben conservar una escala y decisión equivalentes.

## Información que conviene registrar

- Versión de la exportación de Edge Impulse.
- Tipo de compilador: TensorFlow Lite o EON.
- Modelo float32 o int8.
- Versión del framework Arduino-ESP32.
- Versión del toolchain.
- Commit o versión de ESP-NN utilizada.
- Lista exacta de archivos modificados.
- Tiempo de DSP e inferencia.
- Scores de un conjunto fijo de imágenes de referencia.

## Mejora pendiente

Convertir las modificaciones manuales en un script dentro de `scripts/` o en un parche `.patch`. Así, una nueva librería podrá prepararse de forma reproducible y auditable.
