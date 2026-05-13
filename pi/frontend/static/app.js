async function api(url, opts){const r=await fetch(url,opts);const d=await r.json();if(!r.ok) throw new Error(d.detail||'API Fehler');return d;}
const byId=(id)=>document.getElementById(id);
function flash(msg){const e=byId('msg'); if(e) e.textContent=msg;}

async function loadDashboard(){const e=byId('dash-main');if(!e)return;const s=await api('/api/v1/status');
e.innerHTML=`<h2>System</h2><p>Aktive Person: <b>${s.active_person?.name||'-'}</b></p><p>Waage: <span class="badge ${s.scale_online?'ok':'off'}">${s.scale_online?'online':'offline'}</span></p><p>Letzter Kontakt: ${s.last_contact_to_scale||'-'}</p><p>API: ${s.api_status} | DB: ${s.database_status}</p><p>URL: <a href="${s.pi_url}">${s.pi_url}</a></p>`;
const runs=byId('dash-runs'); runs.innerHTML=(s.recent_runs||[]).map(r=>`<div>#${r.id} Lauf ${r.run_number} · ${r.start_weight_g}g · ${r.person_name||'-'} · ${r.received_at}</div>`).join('')||'Keine Läufe';}

async function loadStatus(){const e=byId('status');if(!e)return;const s=await api('/api/v1/status');
e.innerHTML=`<p>Pi-IP: ${s.pi_ip}</p><p>Pi-URL: ${s.pi_url}</p><p>Waage: ${s.scale_online?'online':'offline'}</p><p>Letzter Kontakt: ${s.last_contact_to_scale||'-'}</p><p>LED: ${s.led_status}</p><p>OLED: ${s.oled_status}</p><p>QR-Ziel: ${s.pi_url}</p><p>Systemstatus: ${s.system_status}</p>`;}

async function loadRuns(){const t=byId('runs-table');if(!t)return;try{const q=byId('search')?.value||'';const st=byId('status-filter')?.value||'';const sort=byId('sort')?.value||'id_desc';
const d=await api(`/api/v1/runs?search=${encodeURIComponent(q)}&status=${encodeURIComponent(st)}&sort=${encodeURIComponent(sort)}&limit=100`);
const persons=await api('/api/v1/persons');
const pOpts=(pid)=>persons.persons.map(p=>`<option value="${p.id}" ${Number(pid)===p.id?'selected':''}>${p.name}</option>`).join('');
t.innerHTML='<tr><th>ID</th><th>Laufnr</th><th>Zeit(ms)</th><th>Startgewicht(g)</th><th>Person</th><th>Status</th><th>Notiz</th><th>Empfangen</th><th>Aktion</th></tr>'+
d.runs.map(r=>`<tr><td>${r.id}</td><td>${r.run_number}</td><td>${r.time_ms}</td><td>${r.start_weight_g}</td><td><select id="p-${r.id}">${pOpts(r.person_id)}</select></td><td>${r.status}</td><td><input id="n-${r.id}" value="${r.note||''}"></td><td>${r.received_at}</td><td><button onclick="saveRun(${r.id})">Speichern</button><button onclick="deleteRun(${r.id})">Löschen</button></td></tr>`).join('');
}catch(err){flash(err.message)}}

async function saveRun(id){try{await api(`/api/v1/runs/${id}`,{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({person_id:Number(byId('p-'+id).value),note:byId('n-'+id).value})});loadRuns();}catch(err){flash(err.message)}}
async function deleteRun(id){if(!confirm('Lauf wirklich löschen?')) return;try{await api('/api/v1/runs/'+id,{method:'DELETE'});loadRuns();}catch(err){flash(err.message)}}

async function loadPersons(){const el=byId('persons-list');if(!el)return;try{const d=await api('/api/v1/persons');el.innerHTML=d.persons.map(p=>`<div class="card"><b>${p.id===d.active_person_id?'🟢':''} ${p.name}</b><div><input id="pn-${p.id}" value="${p.name}"><button onclick="renamePerson(${p.id})">Umbenennen</button><button onclick="activatePerson(${p.id})">Aktiv</button>${p.id!==1?`<button onclick="deletePerson(${p.id})">Löschen</button>`:''}</div></div>`).join('');}catch(err){flash(err.message)}}
async function addPerson(){try{await api('/api/v1/persons',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:byId('new-person').value,activate:byId('activate').checked})});byId('new-person').value='';loadPersons();}catch(err){flash(err.message)}}
async function renamePerson(id){try{await api('/api/v1/persons/'+id,{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:byId('pn-'+id).value})});loadPersons();}catch(err){flash(err.message)}}
async function deletePerson(id){if(!confirm('Person wirklich löschen?')) return;try{await api('/api/v1/persons/'+id,{method:'DELETE'});loadPersons();}catch(err){flash(err.message)}}
async function activatePerson(id){try{await api('/api/v1/persons/'+id+'/activate',{method:'POST'});loadPersons();}catch(err){flash(err.message)}}

loadDashboard();loadStatus();loadRuns();loadPersons();
