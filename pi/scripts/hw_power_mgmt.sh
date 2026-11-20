#!/usr/bin/env bash
# /home/wage/wage/pi/scripts/hw_power_mgmt.sh
# Hardware-Energieverwaltung für wage-pi
# Wird beim Boot via systemd ausgeführt

# Kein "set -e": jede Einstellung ist optional. Kennt der Kernel z.B. den Governor "ondemand"
# nicht (neuere Pi-Kernel bieten nur schedutil/performance/powersave) oder fehlt ein sysfs-Eintrag,
# soll nur dieser Schritt entfallen, statt dass der ganze Dienst als fehlgeschlagen gilt.
set -uo pipefail

log() { echo "[$(date -Iseconds)] [hw_power] $*"; }
# Wert in eine sysfs-Datei schreiben; Fehler werden ignoriert. Rückgabe 0 nur bei Erfolg.
wr() { [ -e "$2" ] && echo "$1" 2>/dev/null > "$2"; }

# === CPU Governor: ondemand ===
# Kerne schlafen bei Idle, skalieren automatisch bei Last
# Fallback schedutil, wenn ondemand im Kernel fehlt
log "Setze CPU-Governor..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] || continue
    if grep -qw ondemand "${cpu%/*}/scaling_available_governors" 2>/dev/null && wr ondemand "$cpu"; then
        log "  $cpu -> ondemand"
    elif wr schedutil "$cpu"; then
        log "  $cpu -> schedutil"
    else
        log "  $cpu: Governor unverändert ($(cat "$cpu" 2>/dev/null))"
    fi
done

# ondemand Schwellen optimieren: schnell hochskalieren, langsam runter
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/ondemand; do
    wr 60 "$cpu/up_threshold"
    wr 5 "$cpu/sampling_down_factor"
done

# === CPU-Frequenz Grenzen ===
# Minimum: 600MHz (nicht zu tief -> Reaktionszeit), Maximum: voll (Lastspitzen)
for cpu in /sys/devices/system/cpu/cpu*/cpufreq; do
    wr 600000 "$cpu/scaling_min_freq"
    # Maximum nicht begrenzen -> volle 1500MHz bei Bedarf verfügbar
done
log "CPU Frequenz: min=600MHz max=auto"

# === Alle 4 CPU-Kerne aktiv ===
for i in 1 2 3; do
    cpu="/sys/devices/system/cpu/cpu${i}/online"
    wr 1 "$cpu" && log "  CPU${i} aktiviert"
done

# === Bluetooth deaktivieren (nicht genutzt) ===
if command -v rfkill &>/dev/null; then
    rfkill block bluetooth 2>/dev/null && log "Bluetooth deaktiviert" || true
fi

# === USB Power Management ===
# USB-Devices dürfen schlafen wenn inaktiv
for dev in /sys/bus/usb/devices/*/power/autosuspend_delay_ms; do
    wr 2000 "$dev"
done
for dev in /sys/bus/usb/devices/*/power/control; do
    wr auto "$dev"
done
log "USB Auto-Suspend aktiviert"

# === RAM / Swap Optimierung ===
# Weniger auf SD-Karte auslagern (0=nie, 100=aggressiv)
sysctl -w vm.swappiness=10 >/dev/null 2>&1 || log "  swappiness nicht gesetzt"
# Mehr RAM für Filesystem-Cache
sysctl -w vm.vfs_cache_pressure=50 >/dev/null 2>&1 || log "  vfs_cache_pressure nicht gesetzt"
log "RAM: swappiness=10, vfs_cache_pressure=50"

# === SD-Karte: Scheduler optimieren ===
for dev in /sys/block/mmcblk*/queue/scheduler; do
    wr mq-deadline "$dev" || wr deadline "$dev"
done
log "SD-Karte: mq-deadline Scheduler"

# === GPU Speicher ===
# GPU braucht für Kiosk etwas RAM, aber nicht zu viel
# (wird in /boot/config.txt mit gpu_mem=128 gesetzt — hier nur Logging)
GPU_MEM=$(vcgencmd get_mem gpu 2>/dev/null | grep -oE '[0-9]+' | head -1)
log "GPU-Speicher: ${GPU_MEM:-?}MB"

log "Hardware Power Management abgeschlossen"

# Zeitstempel im gleichen Format wie das Backend (ISO mit Zeitzone); ein naiver Wert
# liess die Power-Save-Berechnung mit TypeError scheitern.
# led_shutdown Flag zurücksetzen (wird beim Auto-Shutdown gesetzt)
DB="/home/wage/wage/pi/data/wage_pi.sqlite3"
if [ -f "$DB" ]; then
    sqlite3 "$DB" "UPDATE app_state SET value='0' WHERE key='led_shutdown'; \
        INSERT INTO app_state(key,value) VALUES('last_run_received_at',strftime('%Y-%m-%dT%H:%M:%S+00:00','now')) \
        ON CONFLICT(key) DO UPDATE SET value=strftime('%Y-%m-%dT%H:%M:%S+00:00','now');" 2>/dev/null || true
    log "led_shutdown und last_run_received_at zurückgesetzt"
fi

exit 0
