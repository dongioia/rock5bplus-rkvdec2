#!/bin/bash
DEVICE="${1:-$(ls /dev/tty.usb* 2>/dev/null | head -1)}"
BAUD="${2:-1500000}"
LOG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/logs/uart"
LOG_FILE="$LOG_DIR/uart_$(date +%Y%m%d_%H%M%S).log"
mkdir -p "$LOG_DIR"

if [ -z "$DEVICE" ]; then
    echo "❌ Nessun dispositivo USB seriale trovato."
    echo "   Uso: $0 /dev/tty.usbserial-XXXX [baud]"
    exit 1
fi

echo "📡 UART: $DEVICE @ ${BAUD}bps → $LOG_FILE"
echo "   Ctrl+A, K per uscire"
screen -L -Logfile "$LOG_FILE" "$DEVICE" "$BAUD"
