"""
wage-pi twinkle.py
Portierung des ESP32 sharedStandby Twinkle-Patterns für den Pi LED-Strip.

Zufällige Pixel leuchten in zufälligen Farben auf und verblassen wieder —
ruhiger Sternenhimmel-Effekt für den Power-Save Modus.
"""
import random
import math


class TwinkleState:
    def __init__(self, n: int):
        self.n = n
        # Pro Pixel: Hue (0-360), aktueller Wert (0-255), Zielwert, ob an
        self.hue = [random.uniform(0, 360) for _ in range(n)]
        self.value = [0.0] * n
        self.target = [0.0] * n
        self.on = [False] * n
        self.on_count = 0

        # Konfiguration
        self.value_min = 40     # Minimale Helligkeit leuchtender Pixel
        self.value_max = 200    # Maximale Helligkeit leuchtender Pixel
        self.on_min = 0.10      # Mindestanteil leuchtender Pixel (10%)
        self.on_max = 0.25      # Maximalanteil leuchtender Pixel (25%)
        self.fade_speed = 1.2   # Sanftes Fließen statt Sprünge
        self.change_every = 15  # Seltener wechseln = ruhigeres Bild

        self._frame = 0
        self._base = 8          # Basis-Helligkeit aller Pixel (sehr gedimmt)

        # Initial einige Pixel einschalten
        self._init_pixels()

    def _init_pixels(self):
        target_on = int(self.n * random.uniform(self.on_min, self.on_max))
        indices = random.sample(range(self.n), min(target_on, self.n))
        for i in indices:
            self.on[i] = True
            self.hue[i] = random.uniform(0, 360)
            self.target[i] = random.uniform(self.value_min, self.value_max)
            self.on_count += 1

    def _hsv_to_rgb(self, hue: float, value: float):
        """Einfache HSV->RGB Konvertierung (S=1)."""
        h = (hue % 360) / 60.0
        i = int(h) % 6
        f = h - int(h)
        v = value
        q = value * (1.0 - f)
        t = value * f
        if i == 0: return v, t, 0
        if i == 1: return q, v, 0
        if i == 2: return 0, v, t
        if i == 3: return 0, q, v
        if i == 4: return t, 0, v
        return v, 0, q

    def tick(self, strip, Color, max_brightness: int = 12):
        """Einen Frame berechnen und auf den Strip schreiben."""
        self._frame += 1

        # Alle X Frames: mehrere Pixel gleichzeitig ein-/ausschalten für gleichmäßigere Verteilung
        if self._frame % self.change_every == 0:
            target_on = int(self.n * random.uniform(self.on_min, self.on_max))
            current_on = sum(1 for x in self.on if x)
            diff = target_on - current_on
            step = max(1, abs(diff) // 4)  # Mehrere Pixel pro Wechsel, sanfter Übergang

            if diff > 0:
                off_pixels = [i for i in range(self.n) if not self.on[i]]
                random.shuffle(off_pixels)
                for idx in off_pixels[:step]:
                    self.on[idx] = True
                    self.hue[idx] = random.uniform(0, 360)
                    self.target[idx] = random.uniform(self.value_min, self.value_max)
            elif diff < 0:
                on_pixels = [i for i in range(self.n) if self.on[i]]
                random.shuffle(on_pixels)
                for idx in on_pixels[:step]:
                    self.on[idx] = False
                    self.target[idx] = 0

        # Alle Pixel in Richtung Zielwert bewegen (fließend, kein Abschneiden)
        scale = max_brightness / 255.0
        for i in range(min(self.n, strip.numPixels())):
            target = self.target[i] if self.on[i] else self._base
            delta = target - self.value[i]

            if abs(delta) <= self.fade_speed:
                self.value[i] = float(target)
            else:
                self.value[i] += self.fade_speed if delta > 0 else -self.fade_speed

            v = self.value[i] * scale
            if v < 0.5:
                strip.setPixelColor(i, Color(0, 0, 0))
            else:
                r, g, b = self._hsv_to_rgb(self.hue[i], v)
                strip.setPixelColor(i, Color(int(round(r)), int(round(g)), int(round(b))))

        strip.show()
