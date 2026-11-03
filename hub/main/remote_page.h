/*
 * AIO Feeder — web dashboard page (PLAN.md §6.7, M6).
 *
 * One static page, no build step, no CDN: plain HTML + vanilla JS that
 * polls /api/status (5 s) and /api/events (15 s), feeds nodes, downloads
 * CSV/backup and edits the remote settings. The PIN (when web_auth is on)
 * is asked once with prompt() and sent as X-AIO-Pin on every /api call —
 * the page shell itself is static and carries no data.
 */
#ifndef AIO_REMOTE_PAGE_H
#define AIO_REMOTE_PAGE_H

static const char RMT_PAGE[] = R"page(<!doctype html>
<html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>AIO Feeder</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}
header{background:#1c1c1c;padding:10px 14px;display:flex;justify-content:space-between;align-items:center}
header h1{font-size:1.1rem;margin:0}
main{padding:10px;max-width:760px;margin:0 auto}
.grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fill,minmax(230px,1fr))}
.card{background:#1c1c1c;border-radius:10px;padding:12px}
.card h3{margin:0 0 6px;font-size:1rem}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;background:#555}
.on{background:#4c4}.off{background:#c44}.st{background:#cc4}
.small{color:#999;font-size:.82rem;line-height:1.5;margin:2px 0}
.row{display:flex;gap:6px;margin-top:8px}
input[type=number]{width:70px;background:#333;color:#eee;border:1px solid #444;border-radius:6px;padding:6px}
button{background:#3a7d3a;border:0;color:#fff;border-radius:6px;padding:6px 12px;cursor:pointer}
button:active{opacity:.7}
#alert{display:none;background:#7a2020;border-radius:8px;padding:8px 12px;margin-bottom:10px}
table{width:100%;border-collapse:collapse;font-size:.82rem;margin-top:8px}
td,th{padding:3px 6px;text-align:left;border-bottom:1px solid #262626;color:#ccc}
th{color:#888;font-weight:600}
details{background:#1c1c1c;border-radius:10px;padding:10px 12px;margin-top:12px}
summary{cursor:pointer;color:#aaa}
label{display:block;margin:8px 0 2px;font-size:.85rem;color:#aaa}
input[type=text],input[type=password]{width:100%;box-sizing:border-box;background:#333;color:#eee;border:1px solid #444;border-radius:6px;padding:7px}
a{color:#8c8}
#msg{margin-top:8px;color:#8c8;min-height:1.2em}
.err{color:#e88!important}
</style></head><body>
<header><h1>AIO Feeder</h1><span id=clock class=small></span></header>
<main>
<div id=alert></div>
<div id=hubinfo class=small></div>
<div class=grid id=nodes></div>
<details><summary>History</summary><table id=ev><tr><th>time</th><th>node</th><th>amount</th><th>result</th><th>src</th></tr></table>
<div class=row><a href=/api/events.csv>CSV export</a>&nbsp;<a href=/api/backup>backup.json</a></div></details>
<details><summary>Remote settings</summary>
<label>Web dashboard</label><input type=text id=web_on placeholder="1 = on, 0 = off">
<label>Web port</label><input type=text id=web_port>
<label>Require PIN on the API</label><input type=text id=web_auth placeholder="1 = on, 0 = off">
<label>MQTT on</label><input type=text id=mqtt_on placeholder="1 = on, 0 = off">
<label>MQTT broker URL</label><input type=text id=mqtt_url placeholder="mqtt://broker.local:1883">
<label>MQTT user</label><input type=text id=mqtt_user>
<label>MQTT password</label><input type=password id=mqtt_pass placeholder="(unchanged if empty)">
<label>HA discovery</label><input type=text id=mqtt_disc placeholder="1 = on, 0 = off">
<div class=row><button onclick=save()>Save</button></div><div id=msg></div>
</details>
<p class=small>Topics: aio/hub/status &middot; aio/hub/notify &middot; aio/node/&lt;id&gt;/{state,cmd/feed,event/feed_result,config}</p>
</main>
<script>
let pin=sessionStorage.aioPin||'';
function api(path,opt){
  opt=opt||{};opt.headers=Object.assign({'X-AIO-Pin':pin},opt.headers||{});
  return fetch(path,opt).then(r=>{
    if(r.status==401){pin=prompt('PIN required')||'';sessionStorage.aioPin=pin;throw new Error('pin')}
    if(!r.ok)throw new Error('http '+r.status);return r});
}
function esc(s){return String(s).replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]))}
function age(s){if(s<60)return s+'s';if(s<3600)return Math.round(s/60)+' min';if(s<86400)return (s/3600).toFixed(1)+' h';return Math.round(s/86400)+' d'}
function hm(min){return String(Math.floor(min/60)).padStart(2,'0')+':'+String(min%60).padStart(2,'0')}
let st=null;
function draw(){
  if(!st)return;
  document.getElementById('clock').textContent=st.time_ok?new Date(st.unix*1000).toLocaleString():'--:--';
  document.getElementById('hubinfo').textContent=st.online+'/'+st.nodes+' online - web :'+st.port+' (auth '+(st.web_auth?'on':'off')+') - mqtt '+st.mqtt;
  const al=document.getElementById('alert');
  al.style.display=st.alert?'block':'none';
  if(st.alert)al.textContent='Alert: node '+st.alert.node+' - '+st.alert.kind;
  const g=document.getElementById('nodes');g.innerHTML='';
  st.n.forEach(n=>{
    const c=document.createElement('div');c.className='card';
    let h='<h3><span class="dot '+n.state+'"></span>'+esc(n.name)+' <span class=small>#'+n.id+'</span></h3>';
    h+='<div class=small>'+n.mech+' &middot; '+n.unit+' &middot; '+n.state;
    if(n.vin)h+=' &middot; '+n.vin.toFixed(2)+' V';
    if(n.hb_age!=null&&n.state=='online')h+=' &middot; hb '+age(n.hb_age);
    h+='</div>';
    if(n.left!=null)h+='<div class=small>stock est: '+n.left+' '+n.unit+(n.days?' (~'+n.days+' d)':'')+'</div>';
    if(n.last)h+='<div class=small>last: '+n.last.amount+' '+n.unit+' '+n.last.result+' '+age(n.last.age)+' ago</div>';
    if(n.next)h+='<div class=small>next: '+n.next.min+' ('+n.next.in+' min) '+n.next.amount+' '+n.unit+(n.en?'':' [sched off]')+'</div>';
    h+='<div class=row><input type=number id=a'+n.id+' value='+(n.next?n.next.amount:10)+'><button onclick=feed('+n.id+')>Feed</button></div>';
    c.innerHTML=h;g.appendChild(c);
  });
}
function feed(id){
  const amt=parseInt(document.getElementById('a'+id).value,10);
  api('/api/feed',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({node:id,amount:amt})})
    .then(r=>r.json()).then(j=>note(j.ok?'feed sent':'refused: '+(j.error||'?'),!j.ok))
    .catch(e=>{if(e.message!='pin')note('feed failed: '+e.message,true)});
}
function note(t,err){const m=document.getElementById('msg');m.textContent=t;m.className=err?'err':''}
function save(){
  const b={};
  const g=id=>document.getElementById(id).value.trim();
  if(g('web_on')!='')b.web_on=+g('web_on');
  if(g('web_port')!='')b.web_port=+g('web_port');
  if(g('web_auth')!='')b.web_auth=+g('web_auth');
  if(g('mqtt_on')!='')b.mqtt_on=+g('mqtt_on');
  if(g('mqtt_url'))b.mqtt_url=g('mqtt_url');
  if(g('mqtt_user')!=''||st&&st.mqtt!='off')b.mqtt_user=g('mqtt_user');
  if(g('mqtt_pass'))b.mqtt_pass=g('mqtt_pass');
  if(g('mqtt_disc')!='')b.mqtt_disc=+g('mqtt_disc');
  api('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)})
    .then(r=>r.json()).then(j=>{if(j.ok){note('saved');setTimeout(()=>location.reload(),800)}else note('rejected',true)})
    .catch(e=>{if(e.message!='pin')note('save failed: '+e.message,true)});
}
function load_events(){
  api('/api/events?limit=20').then(r=>r.json()).then(j=>{
    const t=document.getElementById('ev');
    t.innerHTML='<tr><th>time</th><th>node</th><th>amount</th><th>result</th><th>src</th></tr>';
    j.e.forEach(e=>{
      const r=t.insertRow();
      const tm=st&&st.time_ok?new Date(e.unix*1000).toLocaleTimeString():e.unix;
      r.insertCell().textContent=tm;r.insertCell().textContent=esc(e.name);
      r.insertCell().textContent=e.amount;r.insertCell().textContent=e.kind;
      r.insertCell().textContent=e.src;});
  }).catch(()=>{});
}
function tick(){api('/api/status').then(r=>r.json()).then(j=>{st=j;draw()}).catch(()=>{if(st){st=null;document.getElementById('hubinfo').textContent='hub unreachable'}})}
tick();setInterval(tick,5000);load_events();setInterval(load_events,15000);
</script></body></html>)page";

#endif /* AIO_REMOTE_PAGE_H */
