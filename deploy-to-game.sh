#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# OptiScaler & DLSS-NR Game Deployment Script
# Targets: Linux/Proton/Wine gaming environments with Steam / Lutris / Heroic
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC_OPTI="${SCRIPT_DIR}/x64/out/OptiScaler.dll"
BIN_SRC_DLSSNR="${SCRIPT_DIR}/x64/out/nvngx.dll_dlssnr.dll"
INI_SRC="${SCRIPT_DIR}/OptiScaler.ini"

usage() {
    echo "Usage: $0 <game-target-directory> [proxy-dll-name]"
    echo ""
    echo "Arguments:"
    echo "  <game-target-directory>   Path to the directory where the game executable lives"
    echo "  [proxy-dll-name]          Name of proxy DLL (default: dxgi.dll)"
    echo "                            Options: dxgi.dll, version.dll, d3d12.dll, winmm.dll, OptiScaler.dll"
    echo ""
    echo "Examples:"
    echo "  $0 \"~/.local/share/Steam/steamapps/common/Cyberpunk 2077/bin/x64\" dxgi.dll"
    echo "  $0 \"~/.local/share/Steam/steamapps/common/Crimson Desert/bin64\" dxgi.dll"
    exit 1
}

if [[ $# -lt 1 ]]; then
    usage
fi

TARGET_DIR="$1"
PROXY_NAME="${2:-dxgi.dll}"

# Expand tilde if present
TARGET_DIR="${TARGET_DIR/#\~/$HOME}"

if [[ ! -d "${TARGET_DIR}" ]]; then
    echo "[-] Error: Target directory does not exist: ${TARGET_DIR}" >&2
    exit 1
fi

if [[ ! -f "${BIN_SRC_OPTI}" ]]; then
    echo "[-] Error: ${BIN_SRC_OPTI} not found! Building first..."
    "${SCRIPT_DIR}/build-local.sh" Release
fi

if [[ ! -f "${BIN_SRC_DLSSNR}" ]]; then
    echo "[-] Error: ${BIN_SRC_DLSSNR} not found!" >&2
    exit 1
fi

echo "[+] Deploying OptiScaler & DLSS-NR to: ${TARGET_DIR}"
echo "[+] Using proxy filename: ${PROXY_NAME}"

# 1. Copy OptiScaler as proxy DLL
cp -v "${BIN_SRC_OPTI}" "${TARGET_DIR}/${PROXY_NAME}"
# Also copy OptiScaler.dll if proxy is a renamed wrapper
if [[ "${PROXY_NAME}" != "OptiScaler.dll" ]]; then
    cp -v "${BIN_SRC_OPTI}" "${TARGET_DIR}/OptiScaler.dll"
fi

# 2. Copy DLSS-NR forwarder DLL
cp -v "${BIN_SRC_DLSSNR}" "${TARGET_DIR}/nvngx.dll_dlssnr.dll"

# 3. Copy OptiScaler.ini if not present (preserve user customized configs)
if [[ ! -f "${TARGET_DIR}/OptiScaler.ini" ]]; then
    if [[ -f "${INI_SRC}" ]]; then
        echo "[+] Installing default OptiScaler.ini"
        cp -v "${INI_SRC}" "${TARGET_DIR}/OptiScaler.ini"
    fi
else
    echo "[!] Preserving existing ${TARGET_DIR}/OptiScaler.ini"
fi

echo ""
echo "[✓] Deployment completed successfully!"
echo "    Files installed:"
echo "      - ${TARGET_DIR}/${PROXY_NAME}"
echo "      - ${TARGET_DIR}/nvngx.dll_dlssnr.dll"
echo "      - ${TARGET_DIR}/OptiScaler.ini"
echo ""
echo "    Proton / Steam Launch Option reminder (if needed):"
echo "      WINEDLLOVERRIDES=\"${PROXY_NAME%.*}=n,b\" %command%"
