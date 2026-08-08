#!/usr/bin/env bash
# /home/wage/wage/pi/scripts/hw_power_mgmt.sh
# Hardware-Energieverwaltung für wage-pi
# Wird beim Boot via systemd ausgeführt

set -euo pipefail

log() { echo "[$(date -Iseconds)] [hw_power] $*"; }

# === CPU Governor: ondemand ===
# Kerne schlafen bei Idle, skalieren automatisch bei Last
log "Setze CPU-Governor auf ondemand..."
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu" ] && echo ondemand > "$cpu" && log "  $cpu -> ondemand"
done

# ondemand Schwellen optimieren: schnell hochskalieren, langsam runter
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/ondemand; do
    [ -f "$cpu/up_threshold" ]       && echo 60  > "$cpu/up_threshold"
    [ -f "$cpu/sampling_down_factor" ] && echo 5  > "$cpu/sampling_down_factor"
done

# === CPU-Frequenz Grenzen ===
# Minimum: 600MHz (nicht zu tief -> Reaktionszeit), Maximum: voll (Lastspitzen)
for cpu in /sys/devices/system/cpu/cpu*/cpufreq; do
    [ -f "$cpu/scaling_min_freq" ] && echo 600000  > "$cpu/scaling_min_freq" || true
    # Maximum nicht begrenzen -> volle 1500MHz bei Bedarf verfügbar
done
log "CPU Frequenz: min=600MHz max=auto"

# === Alle 4 CPU-Kerne aktiv ===
for i in 1 2 3; do
    cpu="/sys/devices/system/cpu/cpu${i}/online"
    [ -f "$cpu" ] && echo 1 > "$cpu" && log "  CPU${i} aktiviert"
done

# === Bluetooth deaktivieren (nicht genutzt) ===
if command -v rfkill &>/dev/null; then
    rfkill block bluetooth 2>/dev/null && log "Bluetooth deaktiviert" || true
fi

# === USB Power Management ===
# USB-Devices dürfen schlafen wenn inaktiv
for dev in /sys/bus/usb/devices/*/power/autosuspend_delay_ms; do
    [ -f "$dev" ] && echo 2000 > "$dev" || true
done
for dev in /sys/bus/usb/devices/*/power/control; do
    [ -f "$dev" ] && echo auto > "$dev" || true
done
log "USB Auto-Suspend aktiviert"

# === RAM / Swap Optimierung ===
# Weniger auf SD-Karte auslagern (0=nie, 100=aggressiv)
sysctl -w vm.swappiness=10 >/dev/null
# Mehr RAM für Filesystem-Cache
sysctl -w vm.vfs_cache_pressure=50 >/dev/null
log "RAM: swappiness=10, vfs_cache_pressure=50"

# === SD-Karte: Scheduler optimieren ===
for dev in /sys/block/mmcblk*/queue/scheduler; do
    [ -f "$dev" ] && echo mq-deadline > "$dev" 2>/dev/null || \
                     echo deadline    > "$dev" 2>/dev/null || true
done
log "SD-Karte: mq-deadline Scheduler"

# === GPU Speicher ===
# GPU braucht für Kiosk etwas RAM, aber nicht zu viel
# (wird in /boot/config.txt mit gpu_mem=128 gesetzt — hier nur Logging)
GPU_MEM=$(vcgencmd get_mem gpu 2>/dev/null | grep -oP '\d+' || echo "?")
log "GPU-Speicher: ${GPU_MEM}MB"

log "Hardware Power Management abgeschlossen"
