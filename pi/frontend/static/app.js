async function api(url, opts = {}) {
  const res = await fetch(url, opts);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) {
    let msg = `HTTP ${res.status}`;
    if (typeof data.detail === 'string') {
      msg = data.detail;
    } else if (Array.isArray(data.detail)) {
      msg = data.detail.map(d => d.msg || JSON.stringify(d)).join('; ');
    } else if (data.detail) {
      msg = JSON.stringify(data.detail);
    }
    throw new Error(msg);
  }
  return data;
}

const byId = (id) => document.getElementById(id);
// Aktuelle Seite in der Navigation markieren
(function markActiveNav() {
  const path = window.location.pathname;
  document.querySelectorAll('header nav a').forEach((a) => {
    const href = a.getAttribute('href');
    const hit = href === '/' ? path === '/' : (path === href || path.startsWith(href.endsWith('/') ? href : href + '/'));
    if (hit) a.classList.add('active');
  });
})();
// Zeitstempel kommen als UTC-ISO-Text; angezeigt wird die lokale Zeit des Geräts.
function fmtTs(value) {
  if (!value) return '—';
  const d = new Date(value);
  if (Number.isNaN(d.getTime())) return String(value);
  return d.toLocaleString('de-DE', { dateStyle: 'short', timeStyle: 'medium' });
}
const esc = (v) => String(v ?? "").replace(/[&<>'"]/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;","\"":"&quot;"}[c]));
function flash(msg, ok = false) {
  const el = byId("msg");
  if (!el) return;
  el.textContent = msg || "";
  el.className = ok ? "msg msg-ok" : "msg msg-err";
}


function networkActionMsg(msg, tone = "info") {
  const el = byId("network-action-msg");
  if (!el) return;
  el.textContent = msg || "";
  el.className = `msg msg-${tone}`;
}
function setNetworkButtonsDisabled(disabled) {
  ["btn-network-save", "btn-network-apply"].forEach((id) => { const btn = byId(id); if (btn) btn.disabled = disabled; });
}
function setApPasswordHint(visible, msg = "") {
  const hint = byId("ap-password-hint");
  if (!hint) return;
  hint.style.display = visible ? "block" : "none";
  hint.textContent = msg;
}
async function fetchSecretIntoField(inputId, endpoint, toggleBtnId, loadBtnId) {
  const input = byId(inputId);
  const toggleBtn = byId(toggleBtnId);
  const loadBtn = byId(loadBtnId);
  if (!input) return;
  try {
    const data = await api(endpoint);
    input.value = data.value || '';
    input.type = 'text';
    if (toggleBtn) toggleBtn.textContent = 'verbergen';
    if (loadBtn) loadBtn.textContent = 'neu laden';
  } catch (e) {
    flash(`Gespeichertes Passwort konnte nicht geladen werden: ${e.message}`);
  }
}

function setupPasswordToggle(inputId, btnId, loadBtnId, secretEndpoint) {
  const input = byId(inputId); const btn = byId(btnId); const loadBtn = byId(loadBtnId);
  if (!input || !btn) return;
  btn.addEventListener('click', async () => {
    if (!input.value && loadBtn && secretEndpoint) {
      await fetchSecretIntoField(inputId, secretEndpoint, btnId, loadBtnId);
      return;
    }
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    btn.textContent = show ? 'verbergen' : 'anzeigen';
  });
  if (loadBtn && secretEndpoint) {
    loadBtn.addEventListener('click', () => fetchSecretIntoField(inputId, secretEndpoint, btnId, loadBtnId));
  }
}

function statusTone(v) {
  const s = String(v ?? "").toLowerCase();
  if (s === "ok" || s.includes("green") || s === "true") return "status-ok";
  if (s.includes("error") || s.includes("red")) return "status-err";
  if (s.includes("degraded") || s.includes("yellow") || s === "false") return "status-warn";
  return "status-info";
}

async function loadDashboard() {
  // Status auch auf Nicht-Dashboard-Seiten für Nav-Links laden
  try {
    const s = await api("/api/v1/status");
    updateWaageNavLinks(s.scale_online);
    if (!byId("dash-main")) return;
    renderDashboardData(s);
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
      <div class="card ${statusTone(s.scale_online)}"><h3>Waagenstatus</h3><div class="kpi">${s.scale_online ? "Online" : "Offline"}</div><div>Kontakt: ${esc(fmtTs(s.last_contact_to_scale))}</div></div>
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
    const params = {
      limit: byId("limit")?.value || "100",
      search: byId("search")?.value || "",
      status: byId("status-filter")?.value || "",
      sort: byId("sort")?.value || "id_desc",
    };
    const personFilterVal = byId("person-filter")?.value || "";
    if (personFilterVal) params.person_id = personFilterVal;
    const q = new URLSearchParams(params);
    const [runs, persons] = await Promise.all([api(`/api/v1/runs?${q.toString()}`), api("/api/v1/persons")]);
    const personOptions = persons.persons.map((p) => `<option value="${p.id}">${esc(p.name)}</option>`).join("");
    table.innerHTML = runs.runs.map((r) => `
      <tr>
        <td>${r.id}</td><td>${r.run_number}</td><td>${r.time_ms}</td><td>${r.start_weight_g}</td>
        <td><select id="run-person-${r.id}"><option value="">— keine Person —</option>${personOptions}</select></td>
        <td>${esc(fmtTs(r.measured_at || r.received_at))}</td><td>${esc(r.status)}</td>
        <td><input id="run-note-${r.id}" value="${esc(r.note || "")}" maxlength="200"></td>
        <td class="run-actions">
          <button onclick="window.location.href='/runs/'+${r.id}" style="background:var(--bg-card2);border:1px solid var(--border);color:var(--text)">Details</button>
          <button onclick="saveRun(${r.id})">Speichern</button>
          <button class="btn-danger" onclick="deleteRun(${r.id})">Löschen</button>
        </td>
      </tr>`).join("");
    runs.runs.forEach((r) => { const sel = byId(`run-person-${r.id}`); if (sel) sel.value = r.person_id ? String(r.person_id) : ""; });
    flash(`${runs.count} Läufe geladen.`, true);
  } catch (e) { flash(`Läufe konnten nicht geladen werden: ${e.message || e}`); }
}

async function saveRun(id) {
  try {
    await api(`/api/v1/runs/${id}`, {method: "PUT", headers: {"Content-Type": "application/json"}, body: JSON.stringify({person_id: byId(`run-person-${id}`).value ? Number(byId(`run-person-${id}`).value) : null, note: byId(`run-note-${id}`).value})});
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
    root.innerHTML = d.persons.map((p) => {
      const isActive = d.active_person_id === p.id;
      return `<div class="card ${isActive ? 'status-ok' : ''}">
        <div style="display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:8px">
          <div class="kpi" style="font-size:1.3rem">${esc(p.name)}</div>
          ${isActive ? '<span style="background:var(--ok);color:#fff;font-size:.75rem;font-weight:700;padding:3px 8px;border-radius:999px">AKTIV</span>' : ''}
        </div>
        <div style="font-size:.8rem;color:var(--text-muted);margin-bottom:12px">${p.run_count || 0} Läufe</div>
        <div style="display:flex;flex-direction:column;gap:6px">
          <div style="display:flex;gap:6px">
            <input id="rename-${p.id}" value="${esc(p.name)}" style="flex:1" ${p.id===1?"disabled":""}  placeholder="Name">
            <button onclick="renamePerson(${p.id})" ${p.id===1?"disabled":""}>✓</button>
          </div>
          <input id="tags-${p.id}" value="${esc(p.tags||'')}" placeholder="Tags (kommagetrennt, z.B. Profi, Team A)" ${p.id===1?"disabled":""}>
          <textarea id="note-${p.id}" rows="2" style="background:var(--bg-card2);border:1px solid var(--border);border-radius:6px;padding:8px;color:var(--text);font-size:.9rem;resize:vertical" placeholder="Notiz" ${p.id===1?"disabled":""}>${esc(p.note||'')}</textarea>
          <div style="display:flex;gap:6px">
            <button onclick="activatePerson(${p.id})" ${isActive?"disabled":""} style="flex:1">${isActive ? "Aktiv" : "Aktiv setzen"}</button>
            <button class="btn-danger" onclick="deletePerson(${p.id})" ${p.id===1?"disabled":""}>✕</button>
          </div>
        </div>
      </div>`;
    }).join("");
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
  try {
    const payload = {
      name: byId(`rename-${id}`).value,
      tags: (byId(`tags-${id}`)?.value || ''),
      note: (byId(`note-${id}`)?.value || ''),
    };
    await api(`/api/v1/persons/${id}`, {method:'PUT', headers:{'Content-Type':'application/json'}, body: JSON.stringify(payload)});
    await loadPersons();
    flash('Person gespeichert.', true);
  } catch (e) { flash(`Speichern fehlgeschlagen: ${e.message}`); }
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

// SSE: Dashboard live aktualisieren wenn neuer Lauf eingeht
(function initDashboardSSE() {
  if (!byId("dash-main")) return;
  let es;
  function connect() {
    es = new EventSource("/api/v1/status/stream");
    es.onmessage = (ev) => {
      try {
        const data = JSON.parse(ev.data);
        renderDashboardData(data);
      } catch (_) {}
    };
    es.onerror = () => {
      es.close();
      setTimeout(connect, 5000);
    };
  }
  connect();
})();

function updateWaageNavLinks(online) {
  const ids = ['nav-waage-config', 'nav-ota'];
  ids.forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    if (online) {
      el.classList.remove('disabled');
      el.removeAttribute('title');
    } else {
      el.classList.add('disabled');
      el.setAttribute('title', 'Waage ist offline');
    }
  });
}

function renderDashboardData(s) {
  updateWaageNavLinks(s.scale_online);
  const root = byId("dash-main");
  if (!root) return;
  const last = s.last_run || {};
  const onlineCls = s.scale_online ? "status-ok" : "status-err";
  const apiCls = (s.api_status === "ok" && s.database_status === "ok") ? "status-ok" : "status-err";
  root.innerHTML = `
    <div class="card ${onlineCls}">
      <h3>Waage</h3>
      <div class="kpi">${s.scale_online ? "Online" : "Offline"}</div>
      <div style="font-size:.8rem;color:var(--text-muted);margin-top:4px">${esc(fmtTs(s.last_contact_to_scale))}</div>
    </div>
    <div class="card ${apiCls}">
      <h3>System</h3>
      <div class="kpi">${esc(s.api_status || "—")}</div>
      <div style="font-size:.8rem;color:var(--text-muted);margin-top:4px">${esc(s.pi_url || "—")}</div>
    </div>
    <div class="card status-info">
      <h3>Aktive Person</h3>
      <div class="kpi" style="font-size:1.3rem">${esc(s.active_person?.name || "—")}</div>
    </div>
    <div class="card status-info">
      <h3>Letzter Lauf</h3>
      <div class="kpi">${last.id ? "#" + esc(last.id) : "—"}</div>
      <div style="font-size:.8rem;color:var(--text-muted);margin-top:4px">${last.start_weight_g ? esc(last.start_weight_g) + " g · " + esc(last.time_ms) + " ms" : ""}</div>
    </div>
  `;
  byId("dash-runs").innerHTML = (s.recent_runs || []).map((r) =>
    `<div class="list-row" style="display:flex;justify-content:space-between;align-items:center">
      <span><strong>#${esc(r.id)}</strong> &nbsp; ${esc(r.person_name || "—")}</span>
      <span style="color:var(--text-muted);font-size:.85rem">${esc(r.start_weight_g)} g &nbsp;·&nbsp; ${esc(fmtTs(r.measured_at || r.received_at))}</span>
    </div>`
  ).join("") || '<div style="color:var(--text-muted);padding:12px 0">Noch keine Läufe vorhanden.</div>';
}


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
    if (byId('ap-password')) byId('ap-password').placeholder = cfg.ap_password_set ? 'AP Passwort gesetzt – leer lassen = behalten' : 'AP Passwort (mindestens 8 Zeichen)';
    setApPasswordHint(!cfg.ap_password_set, 'AP-Passwort erforderlich, mindestens 8 Zeichen');
    if (byId('client-password')) byId('client-password').placeholder = cfg.client_password_set ? 'WLAN Passwort gesetzt – leer lassen = behalten' : 'WLAN Passwort (optional)';
    const statusRoot = byId('network-status-grid');
    if (statusRoot) {
      const nmConn = String(status.current_nmcli_connection || '-');
      const shortNmConn = nmConn.length > 140 ? `${nmConn.slice(0, 140)}…` : nmConn;
      statusRoot.innerHTML = `<div class="card network-status-card">
        <h3>Netzwerk-Status</h3>
        <div><strong>Gespeicherter Modus:</strong> ${esc(cfg.network_mode)}</div>
        <div><strong>Aktiver Status:</strong> ${esc(status.status)}</div>
        <div><strong>AP-Sicherheit:</strong> ${esc(cfg.ap_security)}</div>
        <div><strong>Pi-IP:</strong> ${esc(cfg.current_pi_ip)}</div>
        <div><strong>API-Ziel:</strong> ${esc(cfg.api_target)}</div>
        <div><strong>Letzte Anwendung:</strong> ${esc(status.last_network_apply_at || '-')}</div>
        <div><strong>Letzter Apply-Status:</strong> <span class="long-text">${esc(status.last_network_apply_status || '-')}</span></div>
        <details class="nm-connection-details">
          <summary>Aktive NM-Connection: ${esc(shortNmConn)}</summary>
          <div class="long-text">${esc(nmConn)}</div>
        </details>
      </div>`;
    }
  } catch (e) { flash(`Konfiguration konnte nicht geladen werden: ${e.message}`); }
}

async function refreshConfigPage() {
  await loadNetworkConfig();
  await loadUpdateStatus();
  await loadStatus();
  await loadDashboard();
}

async function saveNetworkConfig() {
  setNetworkButtonsDisabled(true);
  networkActionMsg("Speichere...", "info");
  const mode = document.querySelector('input[name="network-mode"]:checked')?.value || 'ap';
  const payload = {
    network_mode: mode, ap_ssid: byId('ap-ssid')?.value || '', ap_password: byId('ap-password')?.value || '', ap_ip: byId('ap-ip')?.value || '',
    ap_dhcp_start: byId('ap-dhcp-start')?.value || '', ap_dhcp_end: byId('ap-dhcp-end')?.value || '', client_ssid: byId('client-ssid')?.value || '',
    client_password: byId('client-password')?.value || ''
  };
  try {
    await api('/api/v1/config/network', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)});
    if (byId('ap-password')) byId('ap-password').value = '';
    if (byId('client-password')) byId('client-password').value = '';
    flash('Netzwerk-Konfiguration gespeichert.', true);
    networkActionMsg('Gespeichert', 'ok');
    await refreshConfigPage();
  } catch (e) {
    flash(`Speichern fehlgeschlagen: ${e.message}`);
      networkActionMsg(`Speichern fehlgeschlagen: ${e.message}`, 'err');
  } finally {
    setNetworkButtonsDisabled(false);
  }
}

async function applyNetworkConfig() {
  setNetworkButtonsDisabled(true);
  networkActionMsg("Wende Netzwerkeinstellungen an...", "info");
  try {
    const d = await api('/api/v1/config/network/apply', {method:'POST'});
    flash(d.status || 'Angewendet.', d.ok);
    networkActionMsg(d.status || 'Netzwerkeinstellungen angewendet', d.ok ? 'ok' : 'err');
  } catch (e) {
    const msg = String(e.message || '');
    if (msg.toLowerCase().includes('failed to fetch') || msg.toLowerCase().includes('networkerror') || msg.toLowerCase().includes('network')) {
      flash('Netzwerk wird angewendet. Verbindung kann kurz abbrechen. Bitte Seite nach einigen Sekunden neu laden.');
      networkActionMsg('Netzwerk wird angewendet. Verbindung kann kurz abbrechen.', 'warn');
    } else {
      flash(`Anwenden fehlgeschlagen: ${e.message}`);
      networkActionMsg(`Anwenden fehlgeschlagen: ${msg}`, 'err');
    }
  } finally {
    setNetworkButtonsDisabled(false);
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
  const obsolete = data.obsolete_pi_files || [];
  const protectedRuntime = data.protected_runtime_files || [];
  const canApply = !!data.can_apply || data.ui_state === 'update_available';
  const progress = Number(data.progress_percent || 0);
  const details = `
    <details class="update-details"><summary>Technische Details</summary>
      <div><strong>local_commit:</strong> ${esc(data.local_commit || '-')}</div>
      <div><strong>remote_commit:</strong> ${esc(data.remote_commit || '-')}</div>
      <div><strong>changed_pi_files:</strong><ul>${(data.changed_pi_files||[]).map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
      <div><strong>obsolete_pi_files:</strong><ul>${obsolete.map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
      <div><strong>protected_runtime_files:</strong><ul>${protectedRuntime.map(f=>`<li>${esc(f)}</li>`).join('') || '<li>-</li>'}</ul></div>
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
      ${(changed.length > 0) ? `<h4>Neue/geänderte Dateien</h4><ul>${changed.map(f=>`<li>${esc(f)}</li>`).join('')}</ul>` : ''}
      ${(obsolete.length > 0) ? `<h4>Dateien, die entfernt werden</h4><ul>${obsolete.map(f=>`<li>${esc(f)}</li>`).join('')}</ul>` : ''}
      <small>Lokale Betriebsdaten bleiben geschützt.</small><br>
      <small>Lokale Pi-Code-Abweichungen werden durch den Repo-Stand ersetzt.</small>
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

// Wartet, bis update.sh fertig ist. update.sh schreibt seinen Zustand selbst in
// die DB (job_state: updating → success / error); das Backend startet es als
// letzten Schritt neu, kurze Verbindungsfehler sind daher normal.
// Früher wurde hier nur auf /health gewartet – das antwortete sofort (das alte
// Backend lief ja noch), und die Oberfläche meldete "abgeschlossen", bevor das
// Update überhaupt lief.
async function waitForUpdateJob(timeoutMs = 15 * 60 * 1000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    await new Promise((r) => setTimeout(r, 2000));
    try {
      const d = await api('/api/v1/system/update/status');
      if (d.job_state === 'success' || d.job_state === 'error') return d;
      showUpdateOverlay(d.ui_message || 'Pi-Update wird durchgeführt...', d.progress_step || '', d.progress_percent || 0);
    } catch (_) {
      showUpdateOverlay('Backend startet neu...', 'Bitte warten...', 95);
    }
  }
  return null;
}

function showUpdateOverlay(msg, step, percent) {
  const ov = document.getElementById('update-overlay');
  if (!ov) return;
  ov.classList.add('active');
  const m = document.getElementById('ov-msg');
  const s = document.getElementById('ov-step');
  const b = document.getElementById('ov-bar');
  const p = document.getElementById('ov-percent');
  if (m && msg !== undefined) m.textContent = msg;
  if (s && step !== undefined) s.textContent = step;
  if (b && percent !== undefined) b.style.width = Math.max(0, Math.min(100, percent)) + '%';
  if (p && percent !== undefined) p.textContent = Math.round(percent);
}

function hideUpdateOverlay() {
  const ov = document.getElementById('update-overlay');
  if (ov) ov.classList.remove('active');
}

async function applyPiUpdate() {
  updateBusy = true;
  setUpdateButtonsDisabled(true);
  setUpdatePolling(true);
  showUpdateOverlay('Pi-Update wird durchgeführt...', 'Vorbereitung...', 5);
  setUpdateHint(spinnerLabel('Update wird vorbereitet...'), 'info');

  const finishUpdate = async () => {
    const result = await waitForUpdateJob();
    if (result && result.job_state === 'success') {
      showUpdateOverlay('Update abgeschlossen!', 'Alle Dienste wurden neu gestartet', 100);
      setTimeout(hideUpdateOverlay, 2000);
    } else if (result) {
      showUpdateOverlay('Update fehlgeschlagen', result.ui_message || result.last_update_status || 'Details in pi/logs/update.log', 100);
      flash(`Pi-Update fehlgeschlagen: ${result.ui_message || result.last_update_status || 'siehe pi/logs/update.log'}`);
      setTimeout(hideUpdateOverlay, 5000);
    } else {
      showUpdateOverlay('Timeout — kein Ergebnis vom Update', 'Bitte Seite neu laden und pi/logs/update.log prüfen', 100);
      flash('Das Update hat sich nicht rechtzeitig zurückgemeldet.', false);
    }
    updateBusy = false;
    await loadUpdateStatus(true);
  };

  try {
    const data = await api('/api/v1/system/update/apply', {method: 'POST'});
    renderUpdateStatus(data);
    if (data.ui_state !== 'updating') {
      // Nichts zu tun (z.B. "Keine Pi-Updates verfügbar") – kein Update gestartet.
      hideUpdateOverlay();
      updateBusy = false;
      await loadUpdateStatus(true);
      return;
    }
    await finishUpdate();
  } catch (e) {
    const msg = String(e.message || '').toLowerCase();
    if (msg.includes('failed to fetch') || msg.includes('networkerror') || msg.includes('network')) {
      // Verbindung abgebrochen, das Update kann trotzdem laufen – Ergebnis abwarten.
      await finishUpdate();
    } else {
      hideUpdateOverlay();
      updateBusy = false;
      flash(`Pi-Update fehlgeschlagen: ${e.message}`);
      await loadUpdateStatus(true);
    }
  }
}

loadUpdateStatus();


// Bildschirmtastatur: static/keyboard.js

setupPasswordToggle('ap-password', 'toggle-ap-password', 'load-ap-password', '/api/v1/config/network/secret/ap-password');
setupPasswordToggle('client-password', 'toggle-client-password', 'load-client-password', '/api/v1/config/network/secret/client-password');





// === OTA Update (Waage-Config Seite) ===

async function otaSync() {
  const btn = document.getElementById('btn-ota-sync');
  if (btn) btn.disabled = true;
  setOtaStatus('Lade Firmware von GitHub...', null);
  try {
    const r = await fetch('/api/v1/esp-firmware/sync', { method: 'POST' });
    const d = await r.json();
    if (d.ok) {
      setOtaStatus('Firmware geladen: ' + (d.manifest?.version || '-'), null);
      document.getElementById('ota-available').textContent = d.manifest?.version || '-';
    } else {
      setOtaStatus('Fehler beim Laden', null);
    }
  } catch (e) {
    setOtaStatus('Fehler: ' + e.message, null);
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function otaCheck() {
  const btn = document.getElementById('btn-ota-check');
  const applyBtn = document.getElementById('btn-ota-apply');
  if (btn) btn.disabled = true;
  setOtaStatus('Prüfe...', 0);
  try {
    // Manifest vom Pi holen
    const mr = await fetch('/api/v1/esp-firmware/manifest?refresh=true');
    const manifest = await mr.json();
    const available = manifest.version || '-';
    document.getElementById('ota-available').textContent = available;

    // Aktuellen Status vom ESP holen
    const sr = await fetch('/esp-proxy/ota/status');
    if (!sr.ok) throw new Error('Waage nicht erreichbar (HTTP ' + sr.status + ')');
    const esp = await sr.json();
    const current = esp.current_version || '-';
    document.getElementById('ota-current').textContent = current;

    if (current !== available) {
      setOtaStatus('Update verfügbar: ' + current + ' → ' + available, null);
      if (applyBtn) applyBtn.disabled = false;
    } else {
      setOtaStatus('Firmware ist aktuell (' + current + ')', null);
      if (applyBtn) applyBtn.disabled = true;
    }
  } catch (e) {
    setOtaStatus('Fehler: ' + e.message, null);
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function otaApply() {
  if (!confirm('Firmware jetzt aktualisieren? Das Gerät startet danach automatisch neu.')) return;
  const applyBtn = document.getElementById('btn-ota-apply');
  if (applyBtn) applyBtn.disabled = true;
  setOtaStatus('Lade Firmware vom Pi auf ESP...', 10);

  // Erst sync – die Version aus dem Manifest ist die, die der ESP nach dem
  // Neustart melden muss, damit das Update als erfolgreich gilt.
  let expectedVersion = (document.getElementById('ota-available')?.textContent || '').trim();
  try {
    const syncResp = await fetch('/api/v1/esp-firmware/sync', { method: 'POST' });
    const syncData = await syncResp.json().catch(() => ({}));
    if (!syncResp.ok || !syncData.ok) {
      setOtaStatus('Fehler: Firmware konnte nicht von GitHub geladen werden' + (syncData.detail ? ' (' + syncData.detail + ')' : ''), null);
      if (applyBtn) applyBtn.disabled = false;
      return;
    }
    if (syncData.manifest?.version) expectedVersion = syncData.manifest.version;
  } catch (e) {
    setOtaStatus('Fehler: ' + e.message, null);
    if (applyBtn) applyBtn.disabled = false;
    return;
  }
  setOtaStatus('Firmware synchronisiert, prüfe Update...', 20);

  // Check auf dem ESP ausführen damit otaGetState() == UPDATE_AVAILABLE
  try {
    await fetch('/esp-proxy/ota/check', { method: 'POST' });
  } catch (_) {}
  setOtaStatus('Starte Update...', 30);

  try {
    const r = await fetch('/esp-proxy/ota/apply', { method: 'POST' });
    const text = await r.text();
    if (r.status === 409) {
      setOtaStatus('Fehler: ' + text, null);
      if (applyBtn) applyBtn.disabled = false;
      return;
    }
    setOtaStatus(text || 'Update läuft...', 50);

    // Fortschritt pollen.
    // OTA-Zustände des ESP (wage-esp32/include/ota_module.h, OtaState):
    //   0 IDLE, 1 CHECKING, 2 UP_TO_DATE, 3 UPDATE_AVAILABLE,
    //   4 DOWNLOADING, 5 FLASHING, 6 SUCCESS_PENDING_REBOOT, 7 ERROR
    // Während Download/Flash blockiert der ESP und antwortet nicht; der Proxy
    // liefert dann einen Fehlerstatus. Erfolg gilt erst, wenn der ESP wieder
    // antwortet UND die erwartete Version meldet.
    const OTA_SUCCESS_PENDING_REBOOT = 6;
    const OTA_ERROR = 7;
    const POLL_MS = 2000;
    let retries = 0;
    let maxPercent = 50;
    let espWasOffline = false;
    const poll = setInterval(async () => {
      try {
        const sr = await fetch('/esp-proxy/ota/status');
        if (!sr.ok) throw new Error('no response');
        const esp = await sr.json();
        const pct = Math.max(maxPercent, esp.progress || 50);
        maxPercent = pct;
        retries = 0;

        if (esp.state === OTA_ERROR) {
          clearInterval(poll);
          setOtaStatus('Fehler: ' + (esp.status || 'Update fehlgeschlagen'), null);
          if (applyBtn) applyBtn.disabled = false;
          return;
        }

        if (esp.state === OTA_SUCCESS_PENDING_REBOOT) {
          // Weiter pollen, bis der ESP nach dem Neustart wieder antwortet.
          setOtaStatus('Update geschrieben, ESP startet neu...', Math.max(pct, 95));
          return;
        }

        if (espWasOffline) {
          clearInterval(poll);
          const current = esp.current_version || '-';
          if (!expectedVersion || expectedVersion === '-' || current === expectedVersion) {
            setOtaStatus('Update abgeschlossen — ESP wieder online, Version ' + current + ' ✓', 100);
            const curEl = document.getElementById('ota-current');
            if (curEl) curEl.textContent = current;
          } else {
            setOtaStatus('ESP wieder erreichbar, meldet aber Version ' + current + ' statt ' + expectedVersion + ' — Update vermutlich fehlgeschlagen', null);
            if (applyBtn) applyBtn.disabled = false;
          }
          return;
        }

        setOtaStatus(esp.status || '-', pct);
      } catch (_) {
        retries++;
        espWasOffline = true;
        maxPercent = Math.max(maxPercent, 80);
        setOtaStatus('Update läuft, ESP antwortet gerade nicht... (' + Math.round(retries * POLL_MS / 1000) + 's)', maxPercent);
        if (retries > 60) {
          clearInterval(poll);
          setOtaStatus('Timeout — ESP nicht mehr erreichbar', null);
          if (applyBtn) applyBtn.disabled = false;
        }
      }
    }, POLL_MS);
  } catch (e) {
    setOtaStatus('Fehler: ' + e.message, null);
    if (applyBtn) applyBtn.disabled = false;
  }
}

function setOtaStatus(msg, percent) {
  const el = document.getElementById('ota-status');
  if (el) el.textContent = msg;
  const wrap = document.getElementById('ota-progress-wrap');
  if (percent !== null && percent !== undefined) {
    if (wrap) wrap.style.display = 'block';
    const bar = document.getElementById('ota-progress-bar');
    const pct = document.getElementById('ota-progress-pct');
    if (bar) bar.style.width = percent + '%';
    if (pct) pct.textContent = percent + '%';
  }
}


// === Pi Hardware Konfiguration ===

async function loadHardwareConfig() {
  // Nur auf der Konfig-Seite (Abschnitt "Pi Hardware"). Streifengrößen und LED-Helligkeit gehören zur
  // LED-Seite (/config/leds); hier stehen nur OLED-Drehung und Energiesparmodus.
  if (!document.getElementById('pi-oled-rotation')) return;
  try {
    const d = await api('/api/v1/config/hardware');
    document.getElementById('pi-oled-rotation').value = d.pi_oled_rotation ?? 0;
    const psEl = document.getElementById('power-save-after-minutes');
    if (psEl) psEl.value = d.power_save_after_minutes ?? 1;
    const drvEl = document.getElementById('pi-oled-driver');
    if (drvEl) drvEl.value = d.pi_oled_driver || 'ssd1306';
    const sdEl = document.getElementById('auto-shutdown-after-minutes');
    if (sdEl) sdEl.value = d.auto_shutdown_after_minutes ?? 10;
  } catch (e) {
    const el = document.getElementById('hardware-msg');
    if (el) { el.textContent = 'Laden fehlgeschlagen: ' + e.message; el.className = 'msg msg-err'; }
  }
}

async function saveHardwareConfig() {
  const el = document.getElementById('hardware-msg');
  try {
    const payload = {
      pi_oled_rotation: parseInt(document.getElementById('pi-oled-rotation').value),
      power_save_after_minutes: parseInt(document.getElementById('power-save-after-minutes')?.value ?? '1'),
      pi_oled_driver: document.getElementById('pi-oled-driver')?.value || 'ssd1306',
      auto_shutdown_after_minutes: parseInt(document.getElementById('auto-shutdown-after-minutes')?.value ?? '10'),
    };
    const d = await api('/api/v1/config/hardware', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload)
    });
    if (el) { el.textContent = d.message || 'Gespeichert.'; el.className = 'msg msg-ok'; }
    // OLED-Dienst neu starten (Drehung/Typ werden nur beim Start gelesen)
    await fetch('/api/v1/system/restart-services', { method: 'POST' }).catch(() => {});
  } catch (e) {
    if (el) { el.textContent = 'Fehler: ' + e.message; el.className = 'msg msg-err'; }
  }
}

loadHardwareConfig();


// === Manueller Lauf ===

async function initRunNew() {
  if (!byId('manual-weight')) return;

  // Datum/Zeit auf jetzt vorausfüllen
  const now = new Date();
  const local = new Date(now.getTime() - now.getTimezoneOffset() * 60000)
    .toISOString().slice(0, 16);
  byId('manual-datetime').value = local;

  // Personen laden
  try {
    const data = await api('/api/v1/persons');
    const sel = byId('manual-person');
    sel.innerHTML = '<option value="">— keine Person —</option>';
    (data.persons || []).forEach(p => {
      const o = document.createElement('option');
      o.value = p.id;
      o.textContent = p.name;
      sel.appendChild(o);
    });
  } catch (_) {}
}

async function submitManualRun() {
  const weight = parseFloat(byId('manual-weight').value);
  if (isNaN(weight) || weight <= 0) {
    flash('Bitte ein gültiges Gewicht eingeben.');
    return;
  }

  const datetimeVal = byId('manual-datetime').value;
  let receivedAt = null;
  if (datetimeVal) {
    receivedAt = new Date(datetimeVal).toISOString();
  }

  const payload = {
    start_weight_g: weight,
    time_ms: parseInt(byId('manual-time-ms').value) || 0,
    person_id: byId('manual-person').value || null,
    received_at: receivedAt,
    note: byId('manual-note').value || '',
    status: byId('manual-status').value || 'manual',
  };

  try {
    const result = await api('/api/v1/runs/manual', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload),
    });
    if (result.ok) {
      window.location.href = '/runs';
    }
  } catch (e) {
    flash('Fehler beim Speichern: ' + e.message);
  }
}

initRunNew();

// OTA-Seite: "Prüfen"/"Installieren" brauchen die Waage, "Firmware laden" nur den Pi.
// Der Zustand wird alle 10 s neu geprüft (früher nur beim Laden: kam die Waage danach online,
// blieben die Buttons bis zum Neuladen gesperrt).
let espOfflineWarned = false;
async function guardEspPage() {
  const espButtons = ['btn-ota-check', 'btn-ota-apply'];
  if (!espButtons.some(id => document.getElementById(id))) return;
  try {
    const s = await api('/api/v1/status');
    const msg = document.getElementById('msg');
    if (!s.scale_online) {
      if (msg) {
        msg.textContent = '⚠️ Waage ist offline — Prüfen/Installieren gesperrt. Bitte Waage einschalten und mit wage-net verbinden.';
        msg.className = 'msg msg-err';
      }
      espButtons.forEach(id => { const btn = document.getElementById(id); if (btn) { btn.disabled = true; btn.title = 'Waage offline'; } });
      espOfflineWarned = true;
    } else if (espOfflineWarned) {
      espOfflineWarned = false;
      if (msg) { msg.textContent = ''; msg.className = 'msg'; }
      const check = document.getElementById('btn-ota-check');
      if (check) { check.disabled = false; check.title = ''; }  // "Installieren" erst nach dem Prüfen
    }
  } catch (_) {}
  setTimeout(guardEspPage, 10000);
}

guardEspPage();


// === Statistiken ===

async function loadStats(period = 'all') {
  if (!document.getElementById('stats-kpis')) return;

  // Aktiven Button markieren
  ['today','week','month','all'].forEach(p => {
    const btn = document.getElementById('btn-' + p);
    if (btn) btn.style.background = p === period ? 'var(--ok)' : 'var(--accent)';
  });

  try {
    const s = await api(`/api/v1/stats?period=${period}`);

    // KPI Cards
    document.getElementById('stats-kpis').innerHTML = `
      <div class="card status-info">
        <h3>Läufe gesamt</h3>
        <div class="kpi">${s.total_runs}</div>
      </div>
      <div class="card status-info">
        <h3>Ø Gewicht</h3>
        <div class="kpi">${s.avg_weight_g !== null ? s.avg_weight_g + ' g' : '—'}</div>
      </div>
      <div class="card status-info">
        <h3>Ø Zeit</h3>
        <div class="kpi">${s.avg_time_ms !== null ? (s.avg_time_ms / 1000).toFixed(2) + ' s' : '—'}</div>
      </div>
      <div class="card status-ok">
        <h3>Häufigste Person</h3>
        <div class="kpi" style="font-size:1.2rem">${s.top_person ? esc(s.top_person.name) : '—'}</div>
        ${s.top_person ? `<div style="font-size:.8rem;color:var(--text-muted);margin-top:4px">${s.top_person.count} Läufe</div>` : ''}
      </div>
    `;

    // Personen-Balken
    const maxCnt = Math.max(...(s.per_person.map(p => p.count)), 1);
    document.getElementById('stats-persons').innerHTML = s.per_person.length
      ? s.per_person.map(p => `
          <div style="margin-bottom:10px">
            <div style="display:flex;justify-content:space-between;font-size:.85rem;margin-bottom:3px">
              <span>${esc(p.name)}</span>
              <span style="color:var(--text-muted)">${p.count}</span>
            </div>
            <div class="progress">
              <div class="progress-bar" style="width:${Math.round(p.count/maxCnt*100)}%;background:var(--ok)"></div>
            </div>
          </div>`).join('')
      : '<div style="color:var(--text-muted)">Keine Daten</div>';

    // Tagesverlauf
    const maxDay = Math.max(...(s.daily.map(d => d.count)), 1);
    document.getElementById('stats-daily').innerHTML = s.daily.length
      ? s.daily.map(d => `
          <div style="margin-bottom:10px">
            <div style="display:flex;justify-content:space-between;font-size:.85rem;margin-bottom:3px">
              <span>${esc(d.day)}</span>
              <span style="color:var(--text-muted)">${d.count}</span>
            </div>
            <div class="progress">
              <div class="progress-bar" style="width:${Math.round(d.count/maxDay*100)}%"></div>
            </div>
          </div>`).join('')
      : '<div style="color:var(--text-muted)">Keine Daten der letzten 7 Tage</div>';

  } catch (e) {
    flash('Statistiken konnten nicht geladen werden: ' + e.message);
  }
}

loadStats('all');


// === Lauf-Detail Seite ===

async function loadRunDetail() {
  const idEl = document.getElementById('run-id');
  if (!idEl) return;
  const runId = idEl.textContent.trim();

  try {
    const d = await api(`/api/v1/runs/${runId}`);
    const r = d.run;

    // Messung
    byId('d-weight').textContent = r.start_weight_g != null ? r.start_weight_g + ' g' : '—';
    byId('d-min-weight').textContent = r.min_weight_g != null ? r.min_weight_g + ' g' : '—';
    byId('d-time').textContent = r.time_ms != null ? (r.time_ms / 1000).toFixed(3) + ' s (' + r.time_ms + ' ms)' : '—';
    byId('d-start-thr').textContent = r.start_drop_threshold_g != null ? r.start_drop_threshold_g + ' g' : '—';
    byId('d-stop-thr').textContent = r.stop_rise_threshold_g != null ? r.stop_rise_threshold_g + ' g' : '—';
    byId('d-status').textContent = r.status || '—';

    // Gerät
    byId('d-device').textContent = r.device_id || '—';
    byId('d-firmware').textContent = r.firmware_version || '—';
    byId('d-boot').textContent = r.boot_id || '—';
    byId('d-run-nr').textContent = r.run_number || '—';
    byId('d-event').textContent = r.event_id || '—';
    byId('d-received').textContent = fmtTs(r.received_at);
    byId('d-measured').textContent = fmtTs(r.measured_at || r.received_at);

    // Status Badge
    const badge = byId('run-status-badge');
    if (badge) {
      const ok = r.status === 'ok' || r.status === 'manual';
      badge.innerHTML = `<span style="background:${ok ? 'var(--ok)' : 'var(--err)'};color:#fff;font-size:.75rem;font-weight:700;padding:3px 10px;border-radius:999px">${esc(r.status || '?')}</span>`;
    }

    // Bearbeiten
    const sel = byId('edit-person');
    sel.innerHTML = '<option value="">— keine Person —</option>' +
      d.persons.map(p => `<option value="${p.id}">${esc(p.name)}</option>`).join('');
    if (r.person_id) sel.value = String(r.person_id);
    byId('edit-note').value = r.note || '';

  } catch (e) {
    flash('Lauf konnte nicht geladen werden: ' + e.message);
  }
}

async function saveRunDetail() {
  const runId = document.getElementById('run-id')?.textContent.trim();
  if (!runId) return;
  try {
    await api(`/api/v1/runs/${runId}`, {
      method: 'PUT',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        person_id: parseInt(byId('edit-person').value) || null,
        note: byId('edit-note').value,
      }),
    });
    flash('Gespeichert.', true);
  } catch (e) {
    flash('Fehler: ' + e.message);
  }
}

loadRunDetail();


// === LED Konfiguration ===

async function loadLedConfig() {
  if (!document.getElementById('led-rainbow')) return;
  try {
    const d = await api('/api/v1/config/leds');
    const set = (id, valId, val) => {
      const el = document.getElementById(id);
      const valEl = document.getElementById(valId);
      if (el) { el.value = val; }
      if (valEl) { valEl.textContent = val; }
    };
    const sizesEl = document.getElementById('pi-led-strip-sizes');
    if (sizesEl) sizesEl.value = d.pi_led_strip_sizes ?? '80,80,82,82';
    set('led-global', 'led-global-val', d.pi_led_brightness ?? 32);
    set('led-rainbow', 'led-rainbow-val', d.led_brightness_rainbow ?? 80);
    set('led-pulse-offline', 'led-pulse-offline-val', d.led_brightness_pulse_offline ?? 60);
    set('led-wave', 'led-wave-val', d.led_brightness_wave ?? 120);
    set('led-power-save', 'led-power-save-val', d.led_brightness_power_save ?? 12);
    set('led-boot', 'led-boot-val', d.led_brightness_boot ?? 40);
    set('led-error', 'led-error-val', d.led_brightness_error ?? 100);
  } catch (e) {
    flash('LED-Config konnte nicht geladen werden: ' + e.message);
  }
}

async function saveLedConfig() {
  const el = document.getElementById('msg');
  try {
    const payload = {
      pi_led_strip_sizes: document.getElementById('pi-led-strip-sizes')?.value ?? '80,80,82,82',
      pi_led_brightness: parseInt(document.getElementById('led-global').value),
      led_brightness_rainbow: parseInt(document.getElementById('led-rainbow').value),
      led_brightness_pulse_offline: parseInt(document.getElementById('led-pulse-offline').value),
      led_brightness_wave: parseInt(document.getElementById('led-wave').value),
      led_brightness_power_save: parseInt(document.getElementById('led-power-save').value),
      led_brightness_boot: parseInt(document.getElementById('led-boot').value),
      led_brightness_error: parseInt(document.getElementById('led-error').value),
    };
    const d = await api('/api/v1/config/leds', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload),
    });
    // Kein Dienst-Neustart noetig: der LED-Dienst liest die Werte selbst (~1 s)
    if (el) { el.textContent = d.message || 'Gespeichert.'; el.className = 'msg msg-ok'; }
  } catch (e) {
    if (el) { el.textContent = 'Fehler: ' + e.message; el.className = 'msg msg-err'; }
  }
}

loadLedConfig();


// === WLAN Scan ===

async function scanWifi() {
  const btn = document.getElementById('btn-scan');
  const list = document.getElementById('wifi-scan-list');
  const msg = document.getElementById('wifi-scan-msg');
  if (btn) { btn.disabled = true; btn.textContent = '🔍 Scanne...'; }
  if (msg) { msg.textContent = 'Suche nach Netzwerken...'; msg.className = 'msg msg-info'; }
  try {
    const d = await api('/api/v1/config/network/scan');
    if (d.error && !d.networks?.length) {
      if (msg) { msg.textContent = 'Fehler: ' + d.error; msg.className = 'msg msg-err'; }
      return;
    }
    if (list) {
      list.innerHTML = '<option value="">— Netzwerk auswählen —</option>' +
        (d.networks || []).map(n =>
          `<option value="${esc(n.ssid)}">${esc(n.ssid)} (${n.signal}%) ${n.security ? '🔒' : ''}</option>`
        ).join('');
      list.style.display = 'block';
    }
    if (msg) { msg.textContent = `${d.networks?.length || 0} Netzwerke gefunden.`; msg.className = 'msg msg-ok'; }
  } catch (e) {
    if (msg) { msg.textContent = 'Scan fehlgeschlagen: ' + e.message; msg.className = 'msg msg-err'; }
  } finally {
    if (btn) { btn.disabled = false; btn.textContent = '🔍 Scan'; }
  }
}


// === Debug-Modus ===

async function loadDebugMode() {
  try {
    const d = await api('/api/v1/system/debug-mode');
    _applyDebugMode(d.debug_mode === 1);
  } catch (e) {}
}

function _applyDebugMode(active) {
  const label = document.getElementById('debug-mode-label');
  const btn = document.getElementById('btn-debug-toggle');
  if (label) label.textContent = active ? '🟢 An' : '⚫ Aus';
  if (btn) {
    btn.textContent = active ? 'Deaktivieren' : 'Aktivieren';
    btn.style.background = active ? 'var(--err)' : 'var(--ok)';
  }
}

async function toggleDebugMode() {
  const btn = document.getElementById('btn-debug-toggle');
  if (btn) btn.disabled = true;
  try {
    const current = await api('/api/v1/system/debug-mode');
    const newState = current.debug_mode !== 1;
    await api('/api/v1/system/debug-mode', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ enabled: newState })
    });
    _applyDebugMode(newState);
    flash(newState ? 'Debug-Modus aktiviert — kein Auto-Shutdown.' : 'Debug-Modus deaktiviert.', true);
  } catch (e) {
    flash('Fehler: ' + e.message);
  } finally {
    if (btn) btn.disabled = false;
  }
}

loadDebugMode();


// === Pi Herunterfahren ===

async function shutdownPi() {
  if (!confirm('Pi wirklich herunterfahren?')) return;
  try {
    const d = await api('/api/v1/system/shutdown', { method: 'POST' });
    flash(d.message || 'Pi wird heruntergefahren...', d.ok);
  } catch (e) {
    flash('Pi wird heruntergefahren...');
  }
}

async function rebootPi() {
  if (!confirm('Pi wirklich neu starten?')) return;
  try {
    const d = await api('/api/v1/system/reboot', { method: 'POST' });
    flash(d.message || 'Pi wird neu gestartet...', d.ok);
  } catch (e) {
    flash('Pi wird neu gestartet...');
  }
}
