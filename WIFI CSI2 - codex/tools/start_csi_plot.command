#!/bin/zsh

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

PYTHON_BIN=""

for CANDIDATE in \
    "$HOME/.espressif/python_env/idf5.5_py3.11_env/bin/python" \
    "$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/python" \
    "$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
    "$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"
do
    if [ -x "$CANDIDATE" ] && "$CANDIDATE" -c 'import serial' >/dev/null 2>&1; then
        PYTHON_BIN="$CANDIDATE"
        break
    fi
done

if [ -z "$PYTHON_BIN" ] && command -v python3 >/dev/null 2>&1; then
    if "$(command -v python3)" -c 'import serial' >/dev/null 2>&1; then
        PYTHON_BIN="$(command -v python3)"
    fi
fi

if [ -z "$PYTHON_BIN" ] && command -v python >/dev/null 2>&1; then
    if "$(command -v python)" -c 'import serial' >/dev/null 2>&1; then
        PYTHON_BIN="$(command -v python)"
    fi
fi

if [ -z "$PYTHON_BIN" ]; then
    echo "No usable Python interpreter found."
    echo "Install Python or activate the ESP-IDF environment first."
    read -r -k 1 "?Press any key to close..."
    exit 1
fi

"$PYTHON_BIN" tools/csi_web_plot.py --baud 115200
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "CSI plot exited with code $STATUS."
    read -r -k 1 "?Press any key to close..."
fi

exit "$STATUS"
