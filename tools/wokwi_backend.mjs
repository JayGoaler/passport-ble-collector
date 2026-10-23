// Wokwi 专用本地夹具：真实业务路由，独立数据库，日志只含固定分类与计数。
import { chmodSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer } from 'node:https';
import { once } from 'node:events';
import { execFileSync } from 'node:child_process';
import { DatabaseSync } from 'node:sqlite';

const [backendArg, fixtureArg, portArg, openssl] = process.argv.slice(2);
const backend = resolve(backendArg), fixture = resolve(fixtureArg);
const owner = 'wokwi-fixture-owner', nodeId = 'Wokwi';
const counters = {config_ok:0, heartbeat_ok:0, observation_ack_ok:0, accepted_events:0,
  duplicate_events:0, stored_events:0, rejected_requests:0, internal_errors:0};
const report = {schema:'wokwi-backend-report-v1', backend:'personal-tools-real-handler',
  started:false, state:'starting', host:'127.0.0.1', port:0, counters,
  simulation:'NOT_RUN_BY_BACKEND', device_tests:'NOT_RUN'};
let server, stopping = false, phase='fixture';
process.umask(0o077);
function publish() {
  writeFileSync(join(fixture,'status.json.tmp'),JSON.stringify(report,null,2)+'\n',{mode:0o600});
  renameSync(join(fixture,'status.json.tmp'),join(fixture,'status.json'));
}
function summary(route,status) {
  console.log(JSON.stringify({route,status,config_ok:counters.config_ok,heartbeat_ok:counters.heartbeat_ok,
    accepted_events:counters.accepted_events,stored_events:counters.stored_events}));
}
function countStored() {
  const db = new DatabaseSync(process.env.LEDGER_DB_PATH,{readOnly:true});
  try { return Number(db.prepare('SELECT count(*) n FROM ble_observations WHERE node_id=?').get(nodeId).n); }
  finally { db.close(); }
}
async function stop() {
  if(stopping) return;
  stopping=true;
  if(server) { server.closeAllConnections(); await new Promise(done=>server.close(done)); }
  report.state='stopped'; publish(); process.exit(0);
}
try {
  // mkdir 不允许复用已有目录，避免把生产数据库或旧凭据带入运行。
  mkdirSync(fixture,{mode:0o700});
  publish();
  process.env.LEDGER_DB_PATH=join(fixture,'isolated.sqlite');
  process.env.TOOLBOX_USERNAME=owner;
  process.env.TOOLBOX_PASSWORD='wokwi-fixture-only';
  process.env.SESSION_SECRET='wokwi-fixture-only-'.repeat(4);
  process.env.LEDGER_TIME_ZONE='Asia/Shanghai';
  process.env.LEDGER_MAX_RANGE_YEARS='10';
  delete process.env.LEDGER_OWNER_ID;
  delete process.env.APP_ORIGIN;
  phase='load-backend';
  const load = path => import(pathToFileURL(join(backend,path)));
  const repo = await load('src/features/bluetooth-explorer/data/repository.ts');
  const {handle} = await load('src/features/bluetooth-explorer/server/handler.ts');
  phase='register-node';
  const registered = repo.registerNode(owner,{id:nodeId,type:'ai_passport',name:'Wokwi synthetic BLE',capabilities:[]});
  if(!/^[A-Za-z0-9_-]{43}$/.test(registered.token)) throw new Error('token shape');
  phase='certificate';
  execFileSync(openssl,['req','-x509','-newkey','rsa:2048','-nodes','-days','7',
    '-keyout',join(fixture,'server.key'),'-out',join(fixture,'server.crt'),
    '-subj','/CN=host.wokwi.internal',
    '-addext','subjectAltName=DNS:host.wokwi.internal,DNS:localhost,IP:127.0.0.1'],{stdio:'ignore'});
  chmodSync(join(fixture,'server.key'),0o600);
  const cert=readFileSync(join(fixture,'server.crt'));
  server=createServer({key:readFileSync(join(fixture,'server.key')),cert,minVersion:'TLSv1.2'},async(req,res)=>{
    let route='unknown';
    try {
      const url=new URL(req.url,'https://host.wokwi.internal');
      const prefix='/api/tools/bluetooth-explorer/ingest/v1/';
      if(url.pathname.startsWith(prefix) && ['config','heartbeat','observations'].includes(url.pathname.slice(prefix.length)))
        route=url.pathname.slice(prefix.length);
      if(route==='unknown' || url.search) {
        counters.rejected_requests++; res.writeHead(404); res.end('{}'); publish(); summary(route,404); return;
      }
      const chunks=[]; let size=0;
      for await(const chunk of req) {
        size+=chunk.length;
        if(size>8192) {
          counters.rejected_requests++; res.writeHead(413,{'Connection':'close'}); res.end('{}');
          publish(); summary(route,413); return;
        }
        chunks.push(chunk);
      }
      const body=Buffer.concat(chunks).toString('utf8');
      const reply=await handle(new Request(url,{method:req.method,headers:req.headers,
        ...(body?{body}:{})}),['ingest','v1',route]);
      const responseText=await reply.text();
      if(reply.status===200) {
        const ack=JSON.parse(responseText);
        if(route==='config') counters.config_ok++;
        if(route==='heartbeat') counters.heartbeat_ok++;
        if(route==='observations') {
          const request=JSON.parse(body);
          if(ack.schema_version!=='1.0' || ack.batch_id!==request.batch_id || ack.rejected!==0 ||
            !Number.isSafeInteger(ack.accepted) || !Number.isSafeInteger(ack.duplicates) ||
            ack.accepted+ack.duplicates!==request.observations.length) throw new Error('invalid ack');
          counters.observation_ack_ok++; counters.accepted_events+=ack.accepted;
          counters.duplicate_events+=ack.duplicates; counters.stored_events=countStored();
        }
      } else counters.rejected_requests++;
      res.writeHead(reply.status,Object.fromEntries(reply.headers)); res.end(responseText);
      publish(); summary(route,reply.status);
    } catch {
      counters.internal_errors++;
      if(!res.headersSent) res.writeHead(500);
      res.end('{}'); publish(); summary(route,500);
    }
  });
  server.requestTimeout=10000; server.headersTimeout=10000; server.timeout=10000;
  server.maxHeadersCount=32;
  server.on('clientError',(_error,socket)=>socket.destroy());
  phase='listen';
  server.listen(Number(portArg),'127.0.0.1'); await once(server,'listening');
  report.port=server.address().port;
  writeFileSync(join(fixture,'credentials.private.json'),JSON.stringify({ssid:'Wokwi-GUEST',password:'',
    base_url:`https://host.wokwi.internal:${report.port}`,node_id:nodeId,token:registered.token,
    ca:cert.toString('utf8')})+'\n',{mode:0o600});
  report.started=true; report.state='ready'; publish();
  console.log(JSON.stringify({event:'WOKWI_BACKEND_READY',host:report.host,port:report.port}));
  process.on('SIGINT',stop); process.on('SIGTERM',stop);
} catch (error) {
  // 不打印异常对象、请求、URL 或环境，防止上游错误意外包含秘密。
  if(server) server.closeAllConnections();
  if(server) server.close();
  report.state='failed'; counters.internal_errors++;
  try { publish(); } catch { /* 目录创建失败时没有摘要。 */ }
  const code=['EACCES','EPERM','EADDRINUSE','ENOENT','ERR_MODULE_NOT_FOUND'].includes(error?.code)?error.code:'OTHER';
  console.error(JSON.stringify({event:'WOKWI_BACKEND_START_FAILED',phase,code})); process.exitCode=1;
}
