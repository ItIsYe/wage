#!/usr/bin/env bash
# Einmalig ausführen um /boot/config.txt zu optimieren
# sudo bash pi/scripts/setup_boot_config.sh

CONFIG="/boot/firmware/config.txt"
[ -f "$CONFIG" ] || CONFIG="/boot/config.txt"

backup="${CONFIG}.backup-$(date +%Y%m%d)"
cp "$CONFIG" "$backup"
echo "Backup: $backup"

add_if_missing() {
    grep -qF "$1" "$CONFIG" || echo "$1" >> "$CONFIG"
}

# GPU Speicher: 128MB für Chromium Kiosk (Hardware-GPU aktiv)
add_if_missing "gpu_mem=128"

# Bluetooth deaktivieren (spart ~10mA)
add_if_missing "dtoverlay=disable-bt"

# HDMI Power Save (nicht genutzt da DSI-Display)
add_if_missing "hdmi_blanking=1"

# CPU-Frequenz: Performance-Modus erlauben
add_if_missing "arm_boost=1"

echo "Boot-Konfiguration angepasst. Reboot erforderlich."
echo "Änderungen in $CONFIG:"
diff "$backup" "$CONFIG" || true
