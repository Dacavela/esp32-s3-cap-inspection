#!/usr/bin/env python3
"""
fix_espnn.py
============
Fuerza la copia de backup/ESP-NN.orig (kernels espressif validados) sobre el
ESP-NN de lib/SeparadorTapas2_inferencing, sin importar su estado actual.

Este es el cambio que solucionó el bug de scores constantes (~54/~65/~71):
los kernels S3 que trae Edge Impulse por defecto están rotos.

USO (desde la raíz del proyecto):
    python scripts/fix_espnn.py

Después de correrlo: FULL CLEAN BUILD obligatorio.
"""

import hashlib
import shutil
import sys
from pathlib import Path

ROOT   = Path(__file__).resolve().parent.parent
BACKUP = ROOT / "backup" / "ESP-NN.orig"
TARGET = (ROOT / "lib" / "SeparadorTapas2_inferencing" / "src" /
          "edge-impulse-sdk" / "porting" / "espressif" / "ESP-NN")


def md5(p: Path) -> str:
    h = hashlib.md5()
    h.update(p.read_bytes())
    return h.hexdigest()


def main():
    if not BACKUP.exists():
        print(f"ERROR: no existe {BACKUP}")
        sys.exit(1)
    if not TARGET.exists():
        print(f"ERROR: no existe {TARGET} — ¿está instalada la librería?")
        sys.exit(1)

    # ── 1. Diagnóstico: qué archivos difieren AHORA ──────────────────────────
    print("[1] Comparando ESP-NN actual contra backup validado...")
    differed, missing, same = [], [], 0
    files = [f for f in sorted(BACKUP.rglob("*")) if f.is_file()]
    for src in files:
        rel = src.relative_to(BACKUP)
        dst = TARGET / rel
        if not dst.exists():
            missing.append(rel)
        elif md5(src) != md5(dst):
            differed.append(rel)
        else:
            same += 1

    print(f"    Iguales: {same}   Distintos: {len(differed)}   Faltantes: {len(missing)}")
    for r in differed:
        print(f"      DIFERENTE: {r}")
    for r in missing:
        print(f"      FALTABA:   {r}")

    if not differed and not missing:
        print("    → El ESP-NN ya era el correcto. Igual se re-copia por seguridad.")

    # ── 2. Borrar ESP-NN actual y copiar el validado completo ────────────────
    print("[2] Reemplazando ESP-NN completo con backup/ESP-NN.orig...")

    def _rw(func, path, exc_info):
        import os, stat
        os.chmod(path, stat.S_IWRITE)
        func(path)

    shutil.rmtree(TARGET, onexc=_rw)
    shutil.copytree(BACKUP, TARGET)
    print(f"    {len(files)} archivos copiados.")

    # ── 3. Verificación final ────────────────────────────────────────────────
    print("[3] Verificando...")
    bad = 0
    for src in files:
        rel = src.relative_to(BACKUP)
        dst = TARGET / rel
        if not dst.exists() or md5(src) != md5(dst):
            print(f"      ERROR: {rel}")
            bad += 1
    # También en dirección inversa: no debe sobrar nada
    for dst in sorted(TARGET.rglob("*")):
        if dst.is_file() and not (BACKUP / dst.relative_to(TARGET)).exists():
            print(f"      SOBRA: {dst.relative_to(TARGET)}")
            bad += 1

    if bad:
        print(f"\nERROR: {bad} problemas. NO compilar todavía.")
        sys.exit(1)

    print("""
    Verificación OK — ESP-NN validado instalado.

┌────────────────────────────────────────────────────────┐
│  AHORA: FULL CLEAN BUILD (obligatorio)                  │
│                                                         │
│    pio run -e esp32-s3 --target clean                   │
│    pio run -e esp32-s3 --target upload                  │
│                                                         │
│  Sin el clean, PlatformIO reutiliza los .o compilados   │
│  con los kernels rotos y el score sale ~65 constante.   │
└────────────────────────────────────────────────────────┘""")


if __name__ == "__main__":
    main()
