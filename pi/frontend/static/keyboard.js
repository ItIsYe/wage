/* Bildschirmtastatur fuer den Touch-Kiosk.
 *
 * Eigene Datei, damit sie auch in die ESP-Seiten (Waage-Config ueber /esp-proxy/) eingefuegt werden kann.
 *
 * Wichtig fuer Touchscreens:
 * - Die Tasten verhindern touchstart/mousedown, damit das Eingabefeld den Fokus behaelt. Dadurch
 *   erzeugt der Browser bei Touch aber KEIN click-Ereignis mehr. Tastendruecke werden deshalb ueber
 *   pointerup ausgewertet (frueher click: auf dem Touchscreen kam kein einziger Tastendruck an).
 * - Zahlenfelder (type=number) unterstuetzen keine Cursor-Position und verwerfen Zwischenstaende wie
 *   "335." -> solange die Tastatur am Feld haengt, wird es voruebergehend zu type=text.
 * - Die Tastatur oeffnet sich auch beim erneuten Antippen eines bereits fokussierten Feldes.
 * - Beim Oeffnen steht der Cursor am Ende des Feldes.
 * - Ausblenden erst kurz nach dem Antippen von etwas anderem, damit sich das Layout nicht unter dem
 *   Finger verschiebt, bevor der Klick ankommt.
 */
(function initTouchKeyboard() {
  if (window.__wageKeyboard) return;
  window.__wageKeyboard = true;

  const textRows = [
    ['1','2','3','4','5','6','7','8','9','0'],
    ['q','w','e','r','t','z','u','i','o','p'],
    ['a','s','d','f','g','h','j','k','l','-'],
    ['SHIFT','y','x','c','v','b','n','m','_','@'],
    ['.',',','SPACE','BACKSPACE','OK','CLOSE']
  ];
  const numericRows = [
    ['1','2','3'],
    ['4','5','6'],
    ['7','8','9'],
    ['.','0','BACKSPACE'],
    ['-','OK','CLOSE']
  ];
  const LABELS = { SHIFT: '⇧', SPACE: 'Leer', BACKSPACE: '⌫', OK: 'OK', CLOSE: '✕' };
  const escHtml = (v) => String(v ?? '').replace(/[&<>'"]/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));

  let activeInput = null;
  let shiftEnabled = false;
  let hideTimer = null;
  let pressedKey = null;

  const keyboard = document.createElement('div');
  keyboard.id = 'touch-keyboard';
  keyboard.className = 'touch-keyboard hidden';
  keyboard.setAttribute('aria-hidden', 'true');
  document.body.appendChild(keyboard);

  // Fokus im Eingabefeld halten (kein Blur durch Antippen der Tastatur)
  ['touchstart', 'mousedown'].forEach((name) => keyboard.addEventListener(name, (ev) => ev.preventDefault(), { passive: false }));
  keyboard.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    const btn = ev.target.closest('button[data-key]');
    pressedKey = btn ? btn.dataset.key : null;
    if (btn) btn.classList.add('is-pressed');
  });
  keyboard.addEventListener('pointerup', (ev) => {
    ev.preventDefault();
    const btn = ev.target.closest('button[data-key]');
    keyboard.querySelectorAll('.is-pressed').forEach((b) => b.classList.remove('is-pressed'));
    if (btn && btn.dataset.key === pressedKey) onKeyPress(btn.dataset.key);
    pressedKey = null;
  });
  keyboard.addEventListener('pointercancel', () => {
    pressedKey = null;
    keyboard.querySelectorAll('.is-pressed').forEach((b) => b.classList.remove('is-pressed'));
  });

  function isSupportedInput(el) {
    if (!el || el.disabled || el.readOnly) return false;
    if (el.tagName === 'TEXTAREA') return true;
    if (el.tagName !== 'INPUT') return false;
    const type = (el.dataset.kbOrigType || el.getAttribute('type') || 'text').toLowerCase();
    return ['text', 'password', 'number', 'search', 'email', 'url', 'tel', ''].includes(type);
  }

  function isNumericLayout(el) {
    const type = (el.dataset.kbOrigType || el.getAttribute('type') || '').toLowerCase();
    if (type === 'number' || type === 'tel') return true;
    const mode = (el.getAttribute('inputmode') || '').toLowerCase();
    if (mode === 'numeric' || mode === 'decimal') return true;
    const probe = `${el.id || ''} ${el.name || ''}`.toLowerCase();
    return ['ip', 'dhcp', 'port', 'address', 'gateway', 'subnet', 'dns'].some((k) => probe.includes(k)) && !probe.includes('ssid');
  }

  // type=number kennt keine Cursor-Position und verwirft "335." -> waehrend der Eingabe Textfeld
  function attachInput(el) {
    if (el.getAttribute('type') === 'number') {
      el.dataset.kbOrigType = 'number';
      el.setAttribute('type', 'text');
      el.setAttribute('inputmode', 'none');
    }
  }
  function detachInput(el) {
    if (el && el.dataset.kbOrigType) {
      el.setAttribute('type', el.dataset.kbOrigType);
      el.removeAttribute('inputmode');
      delete el.dataset.kbOrigType;
    }
  }

  function fire(el) {
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }
  function selection(el) {
    let start = el.value.length, end = el.value.length;
    try {
      if (typeof el.selectionStart === 'number') { start = el.selectionStart; end = el.selectionEnd; }
    } catch (_) { /* Feldtyp ohne Cursor */ }
    return [start, end];
  }
  function setCursor(el, pos) {
    try { el.setSelectionRange(pos, pos); } catch (_) { /* Feldtyp ohne Cursor */ }
  }
  function insertText(el, text) {
    const max = el.maxLength > 0 ? el.maxLength : Infinity;
    const [start, end] = selection(el);
    const next = el.value.slice(0, start) + text + el.value.slice(end);
    if (next.length > max) return;
    el.value = next;
    setCursor(el, start + text.length);
    fire(el);
  }
  function backspace(el) {
    const [start, end] = selection(el);
    if (start === end && start === 0) return;
    const from = start === end ? start - 1 : start;
    el.value = el.value.slice(0, from) + el.value.slice(end);
    setCursor(el, from);
    fire(el);
  }

  function keyboardHeight() { return Math.round(keyboard.getBoundingClientRect().height || Math.min(window.innerHeight * 0.42, 340)); }
  function setBodyPadding(show) {
    document.body.classList.toggle('touch-keyboard-open', show);
    document.body.style.setProperty('--touch-keyboard-height', show ? `${keyboardHeight()}px` : '0px');
  }

  function render() {
    if (!activeInput) return;
    const layout = isNumericLayout(activeInput) ? numericRows : textRows;
    keyboard.classList.toggle('touch-keyboard-numeric', layout === numericRows);
    keyboard.innerHTML = layout.map((row) => `<div class="touch-key-row" style="grid-template-columns:repeat(${row.length + (row.includes('SPACE') ? 2 : 0)},minmax(0,1fr))">` + row.map((key) => {
      const classes = ['touch-key'];
      if (['BACKSPACE', 'OK', 'CLOSE', 'SHIFT'].includes(key)) classes.push('touch-key-action');
      if (key === 'SPACE') classes.push('touch-key-space');
      if (key === 'SHIFT' && shiftEnabled) classes.push('is-active');
      let label = LABELS[key] || key;
      if (key.length === 1 && /[a-z]/.test(key) && shiftEnabled) label = key.toUpperCase();
      return `<button type="button" tabindex="-1" class="${classes.join(' ')}" data-key="${escHtml(key)}">${escHtml(label)}</button>`;
    }).join('') + '</div>').join('');
  }

  function show(el) {
    clearTimeout(hideTimer);
    if (activeInput && activeInput !== el) detachInput(activeInput);
    const changed = activeInput !== el;
    activeInput = el;
    attachInput(el);
    // Cursor ans Ende: am Touchscreen laesst er sich kaum genau setzen (und nach dem Typwechsel
    // eines Zahlenfelds stuende er sonst am Anfang -> "7335.5" statt "335.7")
    if (changed) setCursor(el, el.value.length);
    if (changed || keyboard.classList.contains('hidden')) render();
    keyboard.classList.remove('hidden');
    keyboard.setAttribute('aria-hidden', 'false');
    setBodyPadding(true);
    setTimeout(() => el.scrollIntoView({ block: 'center', inline: 'nearest' }), 60);
  }

  function hide(blur) {
    clearTimeout(hideTimer);
    keyboard.classList.add('hidden');
    keyboard.setAttribute('aria-hidden', 'true');
    setBodyPadding(false);
    const target = activeInput;
    activeInput = null;
    detachInput(target);
    if (blur && target) target.blur();
  }
  function hideLater() {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => hide(false), 350);
  }

  function onKeyPress(key) {
    const el = activeInput;
    if (!el) return;
    if (key === 'SHIFT') { shiftEnabled = !shiftEnabled; render(); return; }
    if (key === 'OK' || key === 'CLOSE') { hide(true); return; }
    if (key === 'BACKSPACE') { backspace(el); return; }
    if (key === 'SPACE') { insertText(el, ' '); return; }
    insertText(el, shiftEnabled && /[a-z]/.test(key) ? key.toUpperCase() : key);
    if (shiftEnabled) { shiftEnabled = false; render(); }
  }

  document.addEventListener('focusin', (ev) => {
    if (keyboard.contains(ev.target)) return;
    if (isSupportedInput(ev.target)) show(ev.target);
    else if (activeInput) hideLater();
  });
  // Erneutes Antippen eines schon fokussierten Feldes (z.B. nach "✕" oder autofocus) oeffnet die Tastatur
  document.addEventListener('pointerup', (ev) => {
    if (keyboard.contains(ev.target)) return;
    const el = ev.target.closest ? ev.target.closest('input, textarea') : null;
    if (el && isSupportedInput(el) && document.activeElement === el) show(el);
    else if (activeInput && !el) hideLater();
  });
  // Vor dem Absenden eines Formulars den Originaltyp wiederherstellen (Browser-Pruefung, Server)
  document.addEventListener('submit', () => detachInput(activeInput), true);
})();
