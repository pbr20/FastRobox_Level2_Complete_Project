import express from 'express';
import pg from 'pg';
import crypto from 'crypto';
import path from 'path';
import { fileURLToPath } from 'url';
import helmet from 'helmet';

const { Pool } = pg;
for (const name of ['DATABASE_URL','DEVICE_TOKEN','DEVICE_ID','DASH_USER','DASH_PASSWORD']) {
  if (!process.env[name] || process.env[name].startsWith('GENERATE_')) {
    throw new Error(`Missing or placeholder environment variable: ${name}`);
  }
}
const pool = new Pool({ connectionString: process.env.DATABASE_URL, max: 5 });
const app = express();
app.disable('x-powered-by');
app.use(helmet({contentSecurityPolicy: {
  directives: {
    defaultSrc: ["'self'"], scriptSrc: ["'self'"], styleSrc: ["'self'"],
    imgSrc: ["'self'", 'data:'], connectSrc: ["'self'"],
    objectSrc: ["'none'"], upgradeInsecureRequests: []
  }
}}));
app.use(express.json({ limit: '12kb', strict: true }));
const __dirname = path.dirname(fileURLToPath(import.meta.url));

const equal = (a,b) => {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const x=Buffer.from(a), y=Buffer.from(b);
  return x.length === y.length && crypto.timingSafeEqual(x,y);
};
const deviceAuth = (req,res,next) => {
  if (!equal(req.get('X-Device-Token') || '', process.env.DEVICE_TOKEN)) {
    return res.status(401).json({error:'Device authentication failed'});
  }
  next();
};
const basicAuth = (req,res,next) => {
  const h = req.get('Authorization') || '';
  if (!h.startsWith('Basic ')) {res.set('WWW-Authenticate','Basic realm="Smart Classroom"');return res.status(401).send('Login required');}
  let user='', password='';
  try {
    const raw = Buffer.from(h.slice(6), 'base64').toString('utf8');
    const split = raw.indexOf(':'); user=raw.slice(0,split); password=raw.slice(split+1);
  } catch {}
  if (!equal(user, process.env.DASH_USER) || !equal(password, process.env.DASH_PASSWORD)) {
    res.set('WWW-Authenticate','Basic realm="Smart Classroom"');return res.status(401).send('Wrong login');
  }
  next();
};

app.get('/healthz',(req,res)=>res.json({ok:true}));
app.post('/api/ingest',deviceAuth,async (req,res,next)=>{
  const p=req.body;
  if (!p || typeof p !== 'object' || Array.isArray(p)) return res.status(400).json({error:'JSON object required'});
  const {deviceId,roomId,seq,ts,kind} = p;
  if (deviceId !== process.env.DEVICE_ID || !/^[A-Za-z0-9_-]{1,32}$/.test(roomId||'') ||
      !Number.isSafeInteger(seq) || seq < 1 || !Number.isSafeInteger(ts) || ts < 1700000000 ||
      !['TELEMETRY','EVENT'].includes(kind)) return res.status(400).json({error:'Invalid metadata'});
  if (kind==='TELEMETRY' && (
    (p.tempC !== null && (typeof p.tempC !== 'number' || !Number.isFinite(p.tempC) || p.tempC < -50 || p.tempC > 100)) ||
    (p.rh !== null && (typeof p.rh !== 'number' || !Number.isFinite(p.rh) || p.rh < 0 || p.rh > 100)) ||
    typeof p.flame !== 'boolean' || typeof p.status !== 'string')) return res.status(400).json({error:'Invalid sensor data'});
  if (kind==='EVENT' && (!/^[A-Z_]{2,32}$/.test(p.type||'') || typeof p.detail!=='string' || p.detail.length>128)) return res.status(400).json({error:'Invalid event'});
  try {
    const result=await pool.query(`INSERT INTO messages(device_id, seq, room_id, kind, occurred_at, payload)
      VALUES($1,$2,$3,$4,to_timestamp($5),$6::jsonb)
      ON CONFLICT (device_id,seq) DO NOTHING RETURNING seq`,
      [deviceId,seq,roomId,kind,ts,JSON.stringify(p)]);
    if (result.rowCount===0) return res.status(409).json({error:'Replay rejected'});
    res.status(201).json({saved:true,seq});
  } catch (e) {next(e);}
});
app.use('/', basicAuth);
app.get('/api/latest', async (req,res,next)=>{
  try {
    const {rows}=await pool.query(`SELECT payload,received_at FROM messages
       WHERE device_id=$1 AND kind='TELEMETRY' ORDER BY received_at DESC LIMIT 1`,[process.env.DEVICE_ID]);
    const latest=rows[0]||null;
    res.set('Cache-Control','no-store').json({deviceId:process.env.DEVICE_ID, online: !!latest && Date.now()-new Date(latest.received_at).getTime()<30000,
       lastSeen:latest?.received_at||null,latest:latest?.payload||null});
  } catch(e){next(e);}
});
app.get('/api/history',async(req,res,next)=>{
  try {
    const {rows}=await pool.query(`SELECT payload,received_at FROM messages
      WHERE device_id=$1 AND kind='TELEMETRY' ORDER BY received_at DESC LIMIT 60`,[process.env.DEVICE_ID]);
    res.set('Cache-Control','no-store').json(rows);
  } catch(e){next(e);}
});
app.get('/api/events',async(req,res,next)=>{
  try {
    const {rows}=await pool.query(`SELECT payload,received_at FROM messages
       WHERE device_id=$1 AND kind='EVENT' ORDER BY received_at DESC LIMIT 30`,[process.env.DEVICE_ID]);
    res.set('Cache-Control','no-store').json(rows);
  } catch(e){next(e);}
});
app.use(express.static(path.join(__dirname,'public'),{index:'index.html',extensions:['html']}));
app.use((err,req,res,next)=>{console.error(err);res.status(500).json({error:'Server error'});});

async function main(){
  await pool.query(`CREATE TABLE IF NOT EXISTS messages (
    device_id varchar(32) NOT NULL,
    seq bigint NOT NULL,
    room_id varchar(32) NOT NULL,
    kind varchar(16) NOT NULL CHECK (kind IN ('TELEMETRY','EVENT')),
    occurred_at timestamptz NOT NULL,
    received_at timestamptz NOT NULL DEFAULT now(),
    payload jsonb NOT NULL,
    PRIMARY KEY (device_id, seq))`);
  await pool.query('CREATE INDEX IF NOT EXISTS messages_device_latest ON messages(device_id,kind,received_at DESC)');
  const port=Number(process.env.PORT)||3000;
  app.listen(port,'0.0.0.0',()=>console.log(`Smart Classroom listening :${port}`));
}
main().catch(e=>{console.error('Failed to start:',e);process.exit(1)});
