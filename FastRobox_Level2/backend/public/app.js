const $ = id => document.getElementById(id);
const fmt = n => n == null ? '—' : String(n);
const date = n => n ? new Date(n).toLocaleString('en-GB',{timeZone:'Asia/Dhaka',hour12:false}) : '—';
async function json(url){const res=await fetch(url,{cache:'no-store'});if(!res.ok)throw Error(`HTTP ${res.status}`);return res.json()}
async function refresh(){
 try{
  const state=await json('/api/latest');const p=state.latest||{};
  $('live').textContent=state.online?'LIVE':'OFFLINE';$('live').className='badge '+(state.online?'on':'off');
  $('temp').textContent=fmt(p.tempC);$('rh').textContent=fmt(p.rh);
  $('flame').textContent=p.flame==null?'—':p.flame?'DETECTED':'Clear';
  $('status').textContent=fmt(p.status);$('device').textContent=state.deviceId;
  $('room').textContent=fmt(p.roomId);$('door').textContent=fmt(p.door);
  $('rssi').textContent=p.rssi==null?'—':`${p.rssi} dBm`;
  $('chip').textContent=p.chipTempC==null?'—':`${p.chipTempC} °C`;
  $('last').textContent=date(state.lastSeen);$('error').textContent='';
 }catch(e){$('live').textContent='SERVER ERROR';$('live').className='badge off';$('error').textContent=String(e.message)}
}
async function slower(){
 try{
  const [history,events]=await Promise.all([json('/api/history'),json('/api/events')]);
  $('rows').replaceChildren(...history.map(({payload:p,received_at})=>{
    const tr=document.createElement('tr');
    for(const val of [date(received_at),fmt(p.tempC),fmt(p.rh),p.flame?'YES':'NO',fmt(p.status)]){
      const td=document.createElement('td');td.textContent=val;tr.append(td);
    }return tr;
  }));
  $('events').replaceChildren(...events.map(({payload:p,received_at})=>{
    const li=document.createElement('li');li.textContent=`${date(received_at)} — ${p.type}: ${p.detail}`;return li;
  }));
 }catch(e){$('error').textContent=String(e.message)}
}
refresh();slower();setInterval(refresh,2000);setInterval(slower,10000);
