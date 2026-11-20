"""Strombegrenzung für den WS2812-Strip am Pi.

Der Strip hängt an derselben Stromquelle wie der Pi. Zieht er zu viel Strom, bricht die
Spannung ein und der Pi startet neu (Unterspannung). LimitedStrip schätzt vor jedem
show() den Strom aus den gesetzten Farben und senkt die Helligkeit so weit, dass die
Schätzung unter dem eingestellten Maximum bleibt. Die gewünschte Helligkeit (setBrightness)
bleibt erhalten und wird wieder erreicht, sobald das Muster weniger Strom braucht.

Schätzung (WS2812B, 5 V): je Farbkanal bei Wert 255 ca. MA_PER_CHANNEL mA, dazu
IDLE_MA je Pixel Ruhestrom. Die Werte sind eher großzügig, damit die Schätzung
nicht unter dem echten Strom liegt.
"""

MA_PER_CHANNEL = 16.0
IDLE_MA = 0.6


def _channel_sum(color: int) -> int:
    return ((color >> 16) & 255) + ((color >> 8) & 255) + (color & 255)


class LimitedStrip:
    """Hülle um rpi_ws281x.PixelStrip mit denselben Methoden, die der LED-Dienst nutzt."""

    def __init__(self, strip, max_current_ma: int):
        self._strip = strip
        self._n = strip.numPixels()
        self._colors = [0] * self._n
        self._sum = 0                      # Summe aller Kanalwerte (0..255) über alle Pixel
        self._target = strip.getBrightness()
        self._applied = self._target
        self.max_current_ma = max(0, int(max_current_ma))
        self.estimated_ma = 0.0

    # --- Weitergereichte Methoden ---
    def numPixels(self) -> int:
        return self._n

    def getPixelColor(self, i: int) -> int:
        return self._colors[i] if 0 <= i < self._n else 0

    def setPixelColor(self, i: int, color: int) -> None:
        if not 0 <= i < self._n:
            return
        old = self._colors[i]
        if old != color:
            self._sum += _channel_sum(color) - _channel_sum(old)
            self._colors[i] = color
        self._strip.setPixelColor(i, color)

    def setBrightness(self, value: int) -> None:
        self._target = max(0, min(255, int(value)))

    def getBrightness(self) -> int:
        return self._target

    def _cleanup(self) -> None:
        cleanup = getattr(self._strip, "_cleanup", None)
        if cleanup:
            cleanup()

    # --- Begrenzung ---
    def allowed_brightness(self) -> int:
        if self.max_current_ma <= 0 or self._sum == 0:
            return self._target
        budget = self.max_current_ma - self._n * IDLE_MA
        if budget <= 0:
            return 0
        cap = int(budget * 255 * 255 / (self._sum * MA_PER_CHANNEL))
        return max(0, min(self._target, cap))

    def show(self) -> None:
        brightness = self.allowed_brightness()
        if brightness != self._applied:
            self._strip.setBrightness(brightness)
            self._applied = brightness
        self.estimated_ma = self._n * IDLE_MA + self._sum * MA_PER_CHANNEL * brightness / (255 * 255)
        self._strip.show()
