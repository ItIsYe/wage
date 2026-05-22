async function api(url, opts = {}) {
  const res = await fetch(url, opts);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.detail || `HTTP ${res.status}`);
  return data;
}

const byId = (id) => document.getElementById(id);
const esc = (v) => String(v ?? "").replace(/[&<>'"]/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;","\"":"&quot;"}[c]));
function flash(msg, ok = false) {
  const el = byId("msg");
  if (!el) return;
  el.textContent = msg || "";
  el.className = ok ? "msg msg-ok" : "msg msg-err";
}

function statusTone(v) {
  const s = String(v ?? "").toLowerCase();
  if (s === "ok" || s.includes("green") || s === "true") return "status-ok";
  if (s.includes("error") || s.includes("red")) return "status-err";
  if (s.includes("degraded") || s.includes("yellow") || s === "false") return "status-warn";
  return "status-info";
}

async function loadDashboard() {
  const root = byId("dash-main");
  if (!root) return;
  try {
    const s = await api("/api/v1/status");
    const last = s.last_run || {};
    root.innerHTML = `
      <div class="card ${statusTone(s.scale_online)}"><h3>Waage</h3><div class="kpi">${s.scale_online ? "Online" : "Offline"}</div><div>Letzter Kontakt: ${esc(s.last_contact_to_scale || "-")}</div></div>
      <div class="card ${statusTone(s.api_status)}"><h3>API / Datenbank</h3><div class="kpi">${esc(s.api_status)} / ${esc(s.database_status)}</div><div>${esc(s.pi_url || "-")}</div></div>
      <div class="card ${statusTone(s.led_status)}"><h3>LED</h3><div class="kpi">${esc(s.led_status || "-")}</div><div>last_event: ${esc(s.last_event || "-")}</div></div>
      <div class="card ${statusTone(s.oled_status)}"><h3>OLED</h3><div class="kpi">${esc(s.oled_status || "-")}</div><div>Aktive Person: ${esc(s.active_person?.name || "-")}</div></div>
      <div class="card status-info"><h3>Letzter Lauf</h3><div class="kpi">#${esc(last.id || "-")}</div><div>Zeit: ${esc(last.time_ms || "-")} ms · Start: ${esc(last.start_weight_g || "-")} g</div></div>
    `;
    byId("dash-runs").innerHTML = (s.recent_runs || []).map((r) =>
      `<div class="list-row">#${esc(r.id)} · Lauf ${esc(r.run_number)} · ${esc(r.person_name || "-")} · ${esc(r.start_weight_g)} g · ${esc(r.received_at)}</div>`
    ).join("") || "Keine Läufe vorhanden.";
    flash("", true);
  } catch (e) {
    flash(`Dashboard konnte nicht geladen werden: ${e.message}`);
  }
}

async function loadStatus() {
  const root = byId("status-grid");
  if (!root) return;
  try {
    const s = await api("/api/v1/status");
    byId("qr-url").textContent = s.pi_url || "-";
    root.innerHTML = `
      <div class="card ${statusTone(s.scale_online)}"><h3>Waagenstatus</h3><div class="kpi">${s.scale_online ? "Online" : "Offline"}</div><div>Kontakt: ${esc(s.last_contact_to_scale || "-")}</div></div>
      <div class="card ${statusTone(s.api_status)}"><h3>API/DB</h3><div class="kpi">${esc(s.api_status)} / ${esc(s.database_status)}</div></div>
      <div class="card"><h3>Pi</h3><div>IP: ${esc(s.pi_ip)}</div><div>URL: ${esc(s.pi_url)}</div></div>
      <div class="card ${statusTone(s.led_status)}"><h3>LED</h3><div class="kpi">${esc(s.led_status)}</div></div>
      <div class="card ${statusTone(s.oled_status)}"><h3>OLED</h3><div class="kpi">${esc(s.oled_status)}</div></div>
      <div class="card"><h3>Zustand</h3><div>last_event: ${esc(s.last_event || "-")}</div><div>last_run_id: ${esc(s.last_run_id || "-")}</div></div>
      <div class="card"><h3>Gerätedaten</h3><pre>${esc(JSON.stringify(s.last_device || {}, null, 2))}</pre></div>
    `;
  } catch (e) {
    flash(`Status konnte nicht geladen werden: ${e.message}`);
  }
}

async function loadRuns() {
  const table = byId("runs-body");
  if (!table) return;
  try {
    const q = new URLSearchParams({
      limit: byId("limit")?.value || "100",
      search: byId("search")?.value || "",
      status: byId("status-filter")?.value || "",
      sort: byId("sort")?.value || "id_desc",
      person_id: byId("person-filter")?.value || ""
    });
    const [runs, persons] = await Promise.all([api(`/api/v1/runs?${q.toString()}`), api("/api/v1/persons")]);
    const personOptions = persons.persons.map((p) => `<option value="${p.id}">${esc(p.name)}</option>`).join("");
    table.innerHTML = runs.runs.map((r) => `
      <tr>
        <td>${r.id}</td><td>${r.run_number}</td><td>${r.time_ms}</td><td>${r.start_weight_g}</td>
        <td><select id="run-person-${r.id}">${personOptions}</select></td>
        <td>${esc(r.received_at)}</td><td>${esc(r.status)}</td>
        <td><input id="run-note-${r.id}" value="${esc(r.note || "")}" maxlength="200"></td>
        <td>
          <button onclick="saveRun(${r.id})">Speichern</button>
          <button class="btn-danger" onclick="deleteRun(${r.id})">Löschen</button>
        </td>
      </tr>`).join("");
    runs.runs.forEach((r) => { const sel = byId(`run-person-${r.id}`); if (sel) sel.value = String(r.person_id || 1); });
    flash(`${runs.count} Läufe geladen.`, true);
  } catch (e) {
    flash(`Läufe konnten nicht geladen werden: ${e.message}`);
  }
}

async function saveRun(id) {
  try {
    await api(`/api/v1/runs/${id}`, {method: "PUT", headers: {"Content-Type": "application/json"}, body: JSON.stringify({person_id: Number(byId(`run-person-${id}`).value), note: byId(`run-note-${id}`).value})});
    flash(`Lauf ${id} aktualisiert.`, true);
  } catch (e) { flash(`Lauf ${id} konnte nicht gespeichert werden: ${e.message}`); }
}
async function deleteRun(id) {
  if (!confirm(`Lauf ${id} wirklich löschen?`)) return;
  try { await api(`/api/v1/runs/${id}`, {method: "DELETE"}); await loadRuns(); } catch (e) { flash(`Lauf ${id} konnte nicht gelöscht werden: ${e.message}`); }
}

async function loadPersons() {
  const root = byId("persons-list");
  const filter = byId("person-filter");
  if (!root && !filter) return;
  try {
    const d = await api("/api/v1/persons");
    if (filter) filter.innerHTML = `<option value="">alle Personen</option>` + d.persons.map((p) => `<option value="${p.id}">${esc(p.name)}</option>`).join("");
    if (!root) return;
    root.innerHTML = d.persons.map((p) => `<div class="card"><div class="kpi">${esc(p.name)}</div>
      <div class="toolbar">
        <input id="rename-${p.id}" value="${esc(p.name)}" ${p.id===1?"disabled":""}>
        <button onclick="activatePerson(${p.id})" ${d.active_person_id===p.id?"disabled":""}>${d.active_person_id===p.id?"Aktiv":"Aktiv setzen"}</button>
        <button onclick="renamePerson(${p.id})" ${p.id===1?"disabled":""}>Umbenennen</button>
        <button class="btn-danger" onclick="deletePerson(${p.id})" ${p.id===1?"disabled":""}>Löschen</button>
      </div></div>`).join("");
  } catch (e) { flash(`Personen konnten nicht geladen werden: ${e.message}`); }
}

async function addPerson() {
  try {
    await api("/api/v1/persons", {method: "POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify({name: byId("new-person").value, activate: byId("activate").checked})});
    byId("new-person").value = "";
    await loadPersons();
    flash("Person angelegt.", true);
  } catch (e) { flash(`Person konnte nicht angelegt werden: ${e.message}`); }
}
async function renamePerson(id) {
  try { await api(`/api/v1/persons/${id}`, {method:"PUT", headers:{"Content-Type":"application/json"}, body: JSON.stringify({name: byId(`rename-${id}`).value})}); await loadPersons(); flash("Person umbenannt.", true);} catch (e) { flash(`Umbenennen fehlgeschlagen: ${e.message}`); }
}
async function deletePerson(id) {
  if (!confirm("Person wirklich löschen?")) return;
  try { await api(`/api/v1/persons/${id}`, {method:"DELETE"}); await loadPersons(); flash("Person gelöscht.", true);} catch (e) { flash(`Löschen fehlgeschlagen: ${e.message}`); }
}
async function activatePerson(id) {
  try { await api(`/api/v1/persons/${id}/activate`, {method:"POST"}); await loadPersons(); await loadDashboard(); flash("Aktive Person geändert.", true);} catch (e) { flash(`Aktivieren fehlgeschlagen: ${e.message}`); }
}

loadDashboard();
loadStatus();
loadPersons();
loadRuns();


async function loadNetworkConfig() {
  if (!byId("ap-ssid")) return;
  try {
    const [cfg, status] = await Promise.all([api('/api/v1/config/network'), api('/api/v1/config/network/status')]);
    (document.querySelector(`input[name="network-mode"][value="${cfg.network_mode}"]`) || {}).checked = true;
    byId('ap-ssid').value = cfg.ap_ssid || '';
    byId('ap-ip').value = cfg.ap_ip || '';
    byId('ap-dhcp-start').value = cfg.ap_dhcp_start || '';
    byId('ap-dhcp-end').value = cfg.ap_dhcp_end || '';
    byId('client-ssid').value = cfg.client_ssid || '';
    byId('network-status-grid').innerHTML = `<div class="card"><h3>Aktuell</h3><div>Modus: ${esc(cfg.network_mode)}</div><div>Pi-IP: ${esc(cfg.current_pi_ip)}</div><div>API-Ziel: ${esc(cfg.api_target)}</div><div>Status: ${esc(status.status)}</div></div>`;
  } catch (e) { flash(`Konfiguration konnte nicht geladen werden: ${e.message}`); }
}

async function refreshConfigPage() {
  await loadNetworkConfig();
  await loadUpdateStatus();
  await loadStatus();
  await loadDashboard();
}

async function saveNetworkConfig() {
  const mode = document.querySelector('input[name="network-mode"]:checked')?.value || 'ap';
  const payload = {
    network_mode: mode, ap_ssid: byId('ap-ssid')?.value || '', ap_password: byId('ap-password')?.value || '', ap_ip: byId('ap-ip')?.value || '',
    ap_dhcp_start: byId('ap-dhcp-start')?.value || '', ap_dhcp_end: byId('ap-dhcp-end')?.value || '', client_ssid: byId('client-ssid')?.value || '',
    client_password: byId('client-password')?.value || '', client_dhcp_enabled: true
  };
  try {
    await api('/api/v1/config/network', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)});
    if (byId('ap-password')) byId('ap-password').value = '';
    if (byId('client-password')) byId('client-password').value = '';
    flash('Netzwerk-Konfiguration gespeichert.', true);
    await refreshConfigPage();
  } catch (e) {
    flash(`Speichern fehlgeschlagen: ${e.message}`);
  }
}

async function applyNetworkConfig() {
  try {
    const d = await api('/api/v1/config/network/apply', {method:'POST'});
    flash(d.status || 'Angewendet.', d.ok);
  } catch (e) {
    const msg = String(e.message || '');
    if (msg.toLowerCase().includes('failed to fetch') || msg.toLowerCase().includes('networkerror') || msg.toLowerCase().includes('network')) {
      flash('Netzwerk wird angewendet. Verbindung kann kurz abbrechen. Bitte Seite nach einigen Sekunden neu laden.');
    } else {
      flash(`Anwenden fehlgeschlagen: ${e.message}`);
    }
  } finally {
    await loadNetworkConfig();
    await loadStatus();
    setTimeout(() => { loadNetworkConfig(); loadStatus(); }, 2000);
    setTimeout(() => { loadNetworkConfig(); loadStatus(); }, 5000);
  }
}

loadNetworkConfig();

let updateBusy = false;
let updatePollTimer = null;

function setUpdateButtonsDisabled(disabled) {
  ['btn-update-check', 'btn-update-apply', 'btn-update-refresh'].forEach((id) => {
    const btn = byId(id);
    if (btn) btn.disabled = disabled;
  });
}

function setUpdateHint(message, tone = 'info') {
  const hint = byId('update-hint');
  if (!hint) return;
  hint.innerHTML = message || '';
  hint.className = `msg msg-${tone}`;
}

function spinnerLabel(text) { return `<span class="busy-dot" aria-hidden="true"></span>${esc(text)}`; }

function stateTone(uiState) {
  if (['no_update','success'].includes(uiState)) return 'ok';
  if (['checking','updating'].includes(uiState)) return 'info';
  if (uiState === 'update_available') return 'warn';
  return 'err';
}

function renderUpdateStatus(data) {
  const root = byId('update-status-grid');
  if (!root) return;
  const changed = data.changed_pi_files || [];
  const blocking = data.blocking_local_code_files || [];
  const canApply = !!data.can_apply || data.ui_state === 'update_available';
  const runtimeHint = (data.ignored_local_runtime_files || []).length > 0 || (data.ignored_remote_runtime_files || []).length > 0;
  const progress = Number(data.progress_percent || 0);
  const details = `
    <details class="update-details"><summary>Technische Details</summary>
      <div><strong>local_commit:</strong> ${esc(data.local_commit || '-')}</div>
      <div><strong>remote_commit:</strong> ${esc(data.remote_commit || '-')}</div>
      <div><strong>changed_pi_files:</strong><ul>${(data.changed_pi_files||[]).map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
      <div><strong>synced_with_remote_files:</strong><ul>${(data.synced_with_remote_files||[]).map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
      <div><strong>ignored_runtime_files:</strong><ul>${(data.ignored_local_runtime_files||[]).map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
      <div><strong>blocking_local_code_files:</strong><ul>${blocking.map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
    </details>`;

  root.innerHTML = `
    <div class="card status-${stateTone(data.ui_state)} update-main-card">
      <h3>Update-Status</h3>
      <div class="kpi">${esc(data.ui_message || '-')}</div>
      <div>Netzwerkmodus: <strong>${esc(data.network_mode || '-')}</strong></div>
      <div>Update erlaubt: <strong>${data.allowed ? 'ja' : 'nein'}</strong></div>
      <div>Letzte Prüfung: ${esc(data.last_update_at || '-')}</div>
      <div>Letztes Update: ${esc(data.last_update_status || '-')}</div>
      ${(data.ui_state === 'checking' || data.ui_state === 'updating') ? `<div class="update-progress-head">${spinnerLabel(esc(data.progress_step || 'Bitte warten...'))}</div>
      <div class="progress"><div class="progress-bar" style="width:${Math.max(0,Math.min(100,progress))}%"></div></div>` : ''}
      ${(changed.length > 0 && canApply) ? `<h4>Geänderte Pi-Dateien</h4><ul>${changed.map(f=>`<li>${esc(f)}</li>`).join('')}</ul>` : ''}
      ${(blocking.length > 0) ? `<h4>Blockierende Dateien</h4><ul>${blocking.map(f=>`<li>${esc(f)}</li>`).join('')}</ul>` : ''}
      ${runtimeHint ? '<small>Lokale Betriebsdaten bleiben geschützt.</small>' : ''}
      ${details}
    </div>`;

  const applyBtn = byId('btn-update-apply');
  if (applyBtn) applyBtn.style.display = canApply ? 'inline-flex' : 'none';
  if (applyBtn) applyBtn.disabled = updateBusy || !canApply;
  setUpdateHint(data.ui_message || '', stateTone(data.ui_state));
  setUpdateButtonsDisabled(updateBusy || !data.can_check);
}

function setUpdatePolling(active) {
  if (updatePollTimer) clearInterval(updatePollTimer);
  updatePollTimer = null;
  if (!active) return;
  updatePollTimer = setInterval(() => loadUpdateStatus(true).catch(() => {}), 2000);
}

async function loadUpdateStatus(silent = false) {
  if (!byId('update-status-grid')) return;
  try {
    const data = await api('/api/v1/system/update/status');
    renderUpdateStatus(data);
    if (data.ui_state === 'success' && !updateBusy) {
      await api('/api/v1/system/update/check', {method: 'POST'});
      return loadUpdateStatus(true);
    }
    setUpdatePolling(data.ui_state === 'checking' || data.ui_state === 'updating' || updateBusy);
  } catch (e) {
    const inProgress = updateBusy;
    if (inProgress) {
      setUpdatePolling(true);
      return;
    }
    if (!silent) flash(`Update-Status konnte nicht geladen werden: ${e.message}`);
  }
}


async function checkPiUpdates() {
  updateBusy = true;
  setUpdateButtonsDisabled(true);
  setUpdateHint(spinnerLabel('Suche nach Updates...'), 'info');
  setUpdatePolling(true);
  try {
    const data = await api('/api/v1/system/update/check', {method: 'POST'});
    renderUpdateStatus(data);
    setUpdatePolling(true);
  } catch (e) {
    flash(`Update-Prüfung fehlgeschlagen: ${e.message}`);
  } finally {
    updateBusy = false;
    await loadUpdateStatus(true);
  }
}

async function waitForBackendAfterUpdate() {
  setUpdateHint(spinnerLabel('Backend startet neu, bitte warten...'), 'info');
  for (let i = 0; i < 45; i += 1) {
    await new Promise((r) => setTimeout(r, 2000));
    try {
      const h = await api('/api/v1/health');
      if (h && h.ok) return true;
    } catch (_) {}
  }
  return false;
}

async function applyPiUpdate() {
  updateBusy = true;
  setUpdateButtonsDisabled(true);
  setUpdatePolling(true);
  setUpdateHint(spinnerLabel('Update wird vorbereitet...'), 'info');
  try {
    const data = await api('/api/v1/system/update/apply', {method: 'POST'});
    renderUpdateStatus(data);
    setUpdatePolling(true);
  } catch (e) {
    const msg = String(e.message || '').toLowerCase();
    if (msg.includes('failed to fetch') || msg.includes('networkerror') || msg.includes('network')) {
      const ok = await waitForBackendAfterUpdate();
      if (ok) await loadUpdateStatus();
      else flash('Backend war nicht rechtzeitig erreichbar.', false);
    } else {
      flash(`Pi-Update fehlgeschlagen: ${e.message}`);
    }
  } finally {
    updateBusy = false;
    await loadUpdateStatus(true);
  }
}

function startConfigAutoRefresh() {
  loadUpdateStatus(true).catch(() => {});
}
loadUpdateStatus();
startConfigAutoRefresh();


(function initTouchKeyboard() {
  const textRows = [
    ['1','2','3','4','5','6','7','8','9','0'],
    ['q','w','e','r','t','y','u','i','o','p'],
    ['a','s','d','f','g','h','j','k','l','-'],
    ['SHIFT','z','x','c','v','b','n','m','_','@'],
    ['.','SPACE','BACKSPACE','OK','CLOSE']
  ];
  const numericRows = [
    ['1','2','3'],
    ['4','5','6'],
    ['7','8','9'],
    ['.','0','BACKSPACE'],
    ['OK','CLOSE']
  ];

  let activeInput = null;
  let shiftEnabled = false;
  let hideTimer = null;
  const keyboard = document.createElement('div');
  keyboard.id = 'touch-keyboard';
  keyboard.className = 'touch-keyboard hidden';
  keyboard.setAttribute('aria-hidden', 'true');
  document.body.appendChild(keyboard);

  function triggerInputEvents(el) {
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  function isSupportedInput(el) {
    if (!el || el.disabled || el.readOnly) return false;
    if (el.tagName === 'TEXTAREA') return true;
    if (el.tagName !== 'INPUT') return false;
    const type = (el.getAttribute('type') || 'text').toLowerCase();
    return ['text', 'password', 'number', ''].includes(type);
  }

  function isNumericLayout(el) {
    if (!el) return false;
    const type = (el.getAttribute('type') || '').toLowerCase();
    if (type === 'number') return true;
    const probe = `${el.id || ''} ${el.name || ''}`.toLowerCase();
    return ['ip', 'dhcp', 'port', 'address'].some((k) => probe.includes(k));
  }

  function resolveKeyLabel(key) {
    if (key.length !== 1) return key;
    if (!/[a-z]/i.test(key)) return key;
    return shiftEnabled ? key.toUpperCase() : key.toLowerCase();
  }

  function insertAtCursor(el, text) {
    const start = el.selectionStart ?? el.value.length;
    const end = el.selectionEnd ?? el.value.length;
    const before = el.value.slice(0, start);
    const after = el.value.slice(end);
    el.value = `${before}${text}${after}`;
    const pos = start + text.length;
    el.setSelectionRange(pos, pos);
    triggerInputEvents(el);
  }

  function backspaceAtCursor(el) {
    const start = el.selectionStart ?? el.value.length;
    const end = el.selectionEnd ?? el.value.length;
    if (start === 0 && end === 0) return;
    if (start !== end) {
      el.value = `${el.value.slice(0, start)}${el.value.slice(end)}`;
      el.setSelectionRange(start, start);
    } else {
      el.value = `${el.value.slice(0, start - 1)}${el.value.slice(end)}`;
      el.setSelectionRange(start - 1, start - 1);
    }
    triggerInputEvents(el);
  }

  function keyboardHeight() {
    return Math.min(window.innerHeight * 0.35, 340);
  }

  function setBodyPadding(show) {
    document.body.classList.toggle('touch-keyboard-open', show);
    document.body.style.setProperty('--touch-keyboard-height', show ? `${keyboardHeight()}px` : '0px');
  }

  function hideKeyboard(blur = false) {
    keyboard.classList.add('hidden');
    keyboard.setAttribute('aria-hidden', 'true');
    setBodyPadding(false);
    const target = activeInput;
    activeInput = null;
    if (blur && target) target.blur();
  }

  function ensureVisible(el) {
    if (!el) return;
    window.setTimeout(() => {
      el.scrollIntoView({ behavior: 'smooth', block: 'center', inline: 'nearest' });
    }, 60);
  }

  function onKeyPress(key) {
    if (!activeInput) return;
    if (key === 'SHIFT') {
      shiftEnabled = !shiftEnabled;
      renderKeyboard();
      return;
    }
    if (key === 'SPACE') return insertAtCursor(activeInput, ' ');
    if (key === 'BACKSPACE') return backspaceAtCursor(activeInput);
    if (key === 'OK') return hideKeyboard(true);
    if (key === 'CLOSE') return hideKeyboard(false);
    insertAtCursor(activeInput, resolveKeyLabel(key));
    if (shiftEnabled) {
      shiftEnabled = false;
      renderKeyboard();
    }
  }

  function renderKeyboard() {
    if (!activeInput) return;
    const layout = isNumericLayout(activeInput) ? numericRows : textRows;
    keyboard.innerHTML = layout.map((row) => {
      const buttons = row.map((key) => {
        const label = resolveKeyLabel(key);
        const classes = ['touch-key'];
        if (['BACKSPACE', 'OK', 'CLOSE', 'SHIFT'].includes(key)) classes.push('touch-key-action');
        if (key === 'SPACE') classes.push('touch-key-space');
        if (key === 'SHIFT' && shiftEnabled) classes.push('is-active');
        return `<button type="button" class="${classes.join(' ')}" data-key="${key}">${esc(label)}</button>`;
      }).join('');
      return `<div class="touch-key-row">${buttons}</div>`;
    }).join('');
    keyboard.querySelectorAll('button[data-key]').forEach((btn) => {
      btn.addEventListener('click', () => onKeyPress(btn.dataset.key));
    });
  }

  function showKeyboard(el) {
    clearTimeout(hideTimer);
    activeInput = el;
    renderKeyboard();
    keyboard.classList.remove('hidden');
    keyboard.setAttribute('aria-hidden', 'false');
    setBodyPadding(true);
    ensureVisible(el);
  }

  document.addEventListener('focusin', (ev) => {
    const target = ev.target;
    if (!isSupportedInput(target)) return hideKeyboard(false);
    showKeyboard(target);
  });

  document.addEventListener('pointerdown', (ev) => {
    if (!activeInput) return;
    const target = ev.target;
    if (keyboard.contains(target) || target === activeInput) return;
    if (isSupportedInput(target)) return;
    hideKeyboard(false);
  });

  document.addEventListener('focusout', () => {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      const focused = document.activeElement;
      if (!keyboard.contains(focused) && focused !== activeInput) hideKeyboard(false);
    }, 100);
  });
})();
