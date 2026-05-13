async function api(url, opts){const r=await fetch(url,opts);const d=await r.json();if(!r.ok) throw new Error(d.detail||'API Fehler');return d;}
const byId=(id)=>document.getElementById(id);
const tone=(v)=>v==='ok'||v==='running'||v===true?'status-ok':(String(v).includes('degraded')||v===false?'status-warn':String(v).includes('error')?'status-err':'status-info');
function flash(msg){const e=byId('msg'); if(e) e.textContent=msg;}

async function loadDashboard(){const root=byId('dash-main');if(!root)return;const s=await api('/api/v1/status');
const last=s.last_run||{};
root.innerHTML=`<div class='card ${tone(s.scale_online)}'><div>Waage</div><div class='kpi'>${s.scale_online?'Online':'Offline'}</div><div>${s.last_contact_to_scale||'-'}</div></div>
<div class='card ${tone(s.api_status)}'><div>API / DB</div><div class='kpi'>${s.api_status} / ${s.database_status}</div><div>${s.pi_url}</div></div>
<div class='card ${tone(s.led_status)}'><div>LED</div><div class='kpi'>${s.led_status||'-'}</div><div>Last Event: ${s.last_event||'-'}</div></div>
<div class='card ${tone(s.oled_status)}'><div>OLED</div><div class='kpi'>${s.oled_status||'-'}</div><div>Aktive Person: ${s.active_person?.name||'-'}</div></div>
<div class='card status-info'><div>Letzter Lauf</div><div class='kpi'>#${last.id||'-'}</div><div>Zeit: ${last.time_ms||'-'} ms / Start: ${last.start_weight_g||'-'} g</div></div>`;
byId('dash-runs').innerHTML=(s.recent_runs||[]).map(r=>`<div class='card'>#${r.id} · ${r.person_name||'-'} · ${r.start_weight_g}g · ${r.received_at}</div>`).join('')||'Keine Läufe';}

async function loadStatus(){const e=byId('status');if(!e)return;const s=await api('/api/v1/status');
e.innerHTML=`<p><b>Pi-IP:</b> ${s.pi_ip}</p><p><b>Pi-URL:</b> ${s.pi_url}</p><p><b>Aktive Person:</b> ${s.active_person?.name||'-'}</p><p><b>Letzter Kontakt:</b> ${s.last_contact_to_scale||'-'}</p><p><b>Scale online:</b> ${s.scale_online}</p><p><b>LED:</b> ${s.led_status}</p><p><b>OLED:</b> ${s.oled_status}</p><p><b>Last run id:</b> ${s.last_run_id||'-'}</p><p><b>Last run received:</b> ${s.last_run_received_at||'-'}</p>`;}
async function loadRuns(){const t=byId('runs-table');if(!t)return;const q=byId('search')?.value||'';const st=byId('status-filter')?.value||'';const d=await api(`/api/v1/runs?search=${encodeURIComponent(q)}&status=${encodeURIComponent(st)}&limit=100`);t.innerHTML='<tr><th>ID</th><th>Lauf</th><th>Zeit</th><th>Gewicht</th><th>Person</th><th>Status</th><th>Empfangen</th></tr>'+d.runs.map(r=>`<tr><td>${r.id}</td><td>${r.run_number}</td><td>${r.time_ms}</td><td>${r.start_weight_g}</td><td>${r.person_name||'-'}</td><td>${r.status}</td><td>${r.received_at}</td></tr>`).join('');}
async function loadPersons(){const el=byId('persons-list');if(!el)return;const d=await api('/api/v1/persons');el.innerHTML=d.persons.map(p=>`<div class='card'><div class='kpi'>${p.name}</div><button onclick='activatePerson(${p.id})'>Aktiv setzen</button></div>`).join('');}
async function addPerson(){await api('/api/v1/persons',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:byId('new-person').value,activate:byId('activate').checked})});byId('new-person').value='';loadPersons();}
async function activatePerson(id){await api('/api/v1/persons/'+id+'/activate',{method:'POST'});loadPersons();loadDashboard();}
loadDashboard();loadStatus();loadRuns();loadPersons();
