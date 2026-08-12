#!/usr/bin/env python3
"""
apply_espnn_patch.py
====================
Instala una nueva exportación de Edge Impulse y aplica el parche de ESP-NN
que hace que los kernels S3 funcionen correctamente.

USO
---
  python scripts/apply_espnn_patch.py <ruta_al_zip_de_EI>

  Ejemplo:
    python scripts/apply_espnn_patch.py ~/Downloads/ei-separadortapas2-arduino-1.0.8.zip

PASOS QUE HACE
--------------
  1. Extrae el zip de EI en un directorio temporal.
  2. Localiza la carpeta de la librería dentro del zip.
  3. Hace backup de lib/SeparadorTapas2_inferencing/ con timestamp.
  4. Reemplaza lib/SeparadorTapas2_inferencing/ por la nueva exportación.
  5. Sobrescribe el ESP-NN de la nueva librería con backup/ESP-NN.orig/
     (la versión de espressif/esp-nn que funciona en el S3).
  6. Imprime un resumen de verificación.
"""

import sys
import os
import stat
import shutil
import zipfile
import tempfile
import hashlib
from pathlib import Path
from datetime import datetime


def _force_remove(func, path, exc_info):
    """Handler para shutil.rmtree: quita read-only y reintenta (necesario en Windows)."""
    os.chmod(path, stat.S_IWRITE)
    func(path)

# ── Rutas relativas a la raíz del proyecto ────────────────────────────────────
PROJECT_ROOT  = Path(__file__).resolve().parent.parent
LIB_DIR       = PROJECT_ROOT / "lib"
LIB_NAME      = "SeparadorTapas2_inferencing"
BACKUP_ESPNN  = PROJECT_ROOT / "backup" / "ESP-NN.orig"
BACKUP_LIB    = PROJECT_ROOT / "backup"

EI_ESPNN_REL  = Path("src/edge-impulse-sdk/porting/espressif/ESP-NN")


def md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def find_lib_root(extracted: Path) -> Path:
    """Localiza la carpeta con library.properties dentro del zip extraído."""
    for p in extracted.rglob("library.properties"):
        return p.parent
    raise FileNotFoundError("No se encontró library.properties en el zip de EI. "
                            "¿Es un export de Arduino?")


def backup_current_lib(lib_path: Path) -> Path:
    """Mueve la librería actual a backup/ con timestamp."""
    ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
    dest = BACKUP_LIB / f"{LIB_NAME}.bak.{ts}"
    print(f"  Backup → {dest.relative_to(PROJECT_ROOT)}")
    shutil.copytree(lib_path, dest)
    return dest


def replace_lib(new_lib_root: Path, lib_path: Path):
    """Elimina la librería actual e instala la nueva."""
    if lib_path.exists():
        shutil.rmtree(lib_path, onexc=_force_remove)
    shutil.copytree(new_lib_root, lib_path)
    print(f"  Librería instalada → {lib_path.relative_to(PROJECT_ROOT)}")


def apply_espnn_patch(lib_path: Path):
    """Sobreescribe el ESP-NN de la nueva librería con el backup validado."""
    target = lib_path / EI_ESPNN_REL
    if not target.exists():
        raise FileNotFoundError(f"No encontré ESP-NN en la nueva librería: {target}")

    if not BACKUP_ESPNN.exists():
        raise FileNotFoundError(f"No encontré el backup de ESP-NN: {BACKUP_ESPNN}\n"
                                "Debe existir backup/ESP-NN.orig/ con los kernels validados.")

    # Sobreescribir archivo por archivo
    replaced = 0
    for src in BACKUP_ESPNN.rglob("*"):
        if src.is_file():
            rel  = src.relative_to(BACKUP_ESPNN)
            dst  = target / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            replaced += 1

    print(f"  ESP-NN parcheado → {replaced} archivos copiados de backup/ESP-NN.orig/")
    return replaced


def verify(lib_path: Path):
    """Comprueba que los checksums del ESP-NN coinciden con los del backup."""
    target   = lib_path / EI_ESPNN_REL
    mismatches = []

    for src in BACKUP_ESPNN.rglob("*"):
        if src.is_file():
            rel = src.relative_to(BACKUP_ESPNN)
            dst = target / rel
            if not dst.exists():
                mismatches.append(f"  FALTA: {rel}")
            elif md5(src) != md5(dst):
                mismatches.append(f"  DIFF:  {rel}")

    if mismatches:
        print("\n  ⚠  DIFERENCIAS detectadas:")
        for m in mismatches:
            print(m)
        return False
    else:
        print(f"  Verificación OK — todos los checksums coinciden.")
        return True


def print_build_reminder(lib_path: Path):
    lp = lib_path / "library.properties"
    version = "?"
    if lp.exists():
        for line in lp.read_text().splitlines():
            if line.startswith("version="):
                version = line.split("=", 1)[1].strip()
    print(f"""
┌─────────────────────────────────────────────────────────────────┐
│  SIGUIENTE PASO                                                  │
│                                                                  │
│  1. En PlatformIO seleccioná el entorno  esp32-s3               │
│  2. Full Clean Build  (ícono de escoba o pio run --target clean) │
│  3. Compilar y subir al ESP32-S3                                 │
│  4. Monitor serie → verificar scores con tapa buena conocida     │
│                                                                  │
│  Librería: {LIB_NAME} v{version:<34}│
│  ESP-NN:   backup/ESP-NN.orig aplicado                          │
└─────────────────────────────────────────────────────────────────┘
""")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    zip_path = Path(sys.argv[1]).resolve()
    if not zip_path.exists():
        print(f"ERROR: No existe el archivo: {zip_path}")
        sys.exit(1)

    lib_path = LIB_DIR / LIB_NAME

    print(f"\n=== apply_espnn_patch.py ===")
    print(f"  Proyecto:  {PROJECT_ROOT}")
    print(f"  Zip de EI: {zip_path}")
    print()

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        # 1. Extraer
        print("[1] Extrayendo zip de EI...")
        if zip_path.suffix == ".zip":
            with zipfile.ZipFile(zip_path) as zf:
                zf.extractall(tmp_path)
        else:
            # Si ya es una carpeta descomprimida
            tmp_path = zip_path

        new_lib_root = find_lib_root(tmp_path)
        print(f"  Librería encontrada en: {new_lib_root.name}/")

        # 2. Backup de la actual
        print("[2] Haciendo backup de la librería actual...")
        if lib_path.exists():
            backup_current_lib(lib_path)
        else:
            print("  (no había librería previa)")

        # 3. Instalar nueva librería
        print("[3] Instalando nueva librería de EI...")
        replace_lib(new_lib_root, lib_path)

        # 4. Aplicar parche ESP-NN
        print("[4] Aplicando parche ESP-NN...")
        apply_espnn_patch(lib_path)

    # 5. Verificar
    print("[5] Verificando checksums...")
    ok = verify(lib_path)

    if ok:
        print_build_reminder(lib_path)
    else:
        print("\nERROR: la verificación falló. Revisá los archivos marcados.")
        sys.exit(1)


if __name__ == "__main__":
    main()
