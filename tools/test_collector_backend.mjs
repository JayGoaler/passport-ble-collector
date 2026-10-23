// 使用实际后端源码和 C 固件报文；所有写入限定到新建临时数据库。
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createServer, request } from 'node:https';
import { once } from 'node:events';
import { execFileSync } from 'node:child_process';
import { DatabaseSync } from 'node:sqlite';

const [backendArg, fixturesArg, openssl = 'openssl'] = process.argv.slice(2);
if (!backendArg || !fixturesArg) throw new Error('需要后端源码目录和 C fixture 目录');
const backend = resolve(backendArg), fixtures = resolve(fixturesArg);
const temp = mkdtempSync(join(tmpdir(), 'passport-backend-'));
process.env.LEDGER_DB_PATH = join(temp, 'isolated.sqlite');
process.env.TOOLBOX_USERNAME = 'firmware-owner';
process.env.TOOLBOX_PASSWORD = 'isolated-fixture-only';
process.env.SESSION_SECRET = 'fixture-only-'.repeat(4);
process.env.LEDGER_TIME_ZONE = 'Asia/Shanghai';
process.env.LEDGER_MAX_RANGE_YEARS = '10';
delete process.env.LEDGER_OWNER_ID;
delete process.env.APP_ORIGIN;
const load = p => import(pathToFileURL(join(backend, p)));
let server, db;
let passed = 0;
async function test(name, fn) { await fn(); passed++; console.log(`PASS ${name}`); }
try {
    const repo = await load('src/features/bluetooth-explorer/data/repository.ts');
    const validation = await load('src/features/bluetooth-explorer/domain/validation.ts');
    const { handle } = await load('src/features/bluetooth-explorer/server/handler.ts');
    const batch = JSON.parse(readFileSync(join(fixtures, 'batch.json'), 'utf8'));
    const heartbeat = JSON.parse(readFileSync(join(fixtures, 'heartbeat.json'), 'utf8'));
    const pass = repo.registerNode('firmware-owner', {id: batch.node_id,type:'ai_passport',name:'Host C fixture',capabilities:[]});
    const mac = repo.registerNode('firmware-owner', {id:'mac-guard',type:'mac_native',name:'Mac guard',capabilities:[]});
    db = new DatabaseSync(process.env.LEDGER_DB_PATH);
    db.exec("CREATE TABLE ledger_guard(value TEXT); INSERT INTO ledger_guard VALUES('unchanged')");
    const baselineMac = repo.listNodes('firmware-owner').find(n => n.id === mac.node.id);
    function resetRate() { db.prepare('UPDATE ble_nodes SET rate_count=0,rate_second_count=0').run(); }
    execFileSync(openssl,['req','-x509','-newkey','rsa:2048','-nodes','-days','1',
        '-keyout',join(temp,'server.key'),'-out',join(temp,'server.crt'),'-subj','/CN=localhost',
        '-addext','subjectAltName=DNS:localhost,IP:127.0.0.1'],{stdio:'ignore'});
    const cert = readFileSync(join(temp,'server.crt'));
    server = createServer({key:readFileSync(join(temp,'server.key')),cert},async (req,res) => {
        try {
            const chunks=[]; for await (const chunk of req) chunks.push(chunk);
            const body=Buffer.concat(chunks).toString('utf8');
            const url=new URL(req.url,'https://localhost');
            const reply=await handle(new Request(url,{method:req.method,headers:req.headers,
                ...(body?{body}: {})}),url.pathname.replace('/api/tools/bluetooth-explorer/','').split('/'));
            res.writeHead(reply.status,Object.fromEntries(reply.headers)); res.end(await reply.text());
        } catch { res.writeHead(500); res.end('{}'); }
    });
    server.listen(0,'127.0.0.1'); await once(server,'listening');
    const port=server.address().port;
    function send(endpoint, body, token=pass.token, options={}) {
        return new Promise((resolveResponse,reject)=>{
            const raw=body===undefined?undefined:typeof body==='string'?body:JSON.stringify(body);
            const req=request({host:'127.0.0.1',port,path:`/api/tools/bluetooth-explorer/ingest/v1/${endpoint}`,
                method:raw===undefined?'GET':'POST',ca:cert,servername:'localhost',
                headers:{authorization:`Bearer ${token}`,'content-type':'application/json'},...options},res=>{
                let text=''; res.on('data',c=>text+=c); res.on('end',()=>{
                    try { resolveResponse({status:res.statusCode,body:JSON.parse(text)}); } catch { reject(new Error('non JSON')); }
                });
            });
            req.on('error',reject); req.setTimeout(5000,()=>req.destroy(new Error('timeout'))); req.end(raw);
        });
    }
    await test('C 序列化批次及心跳通过当前 validation.ts',()=>{
        validation.batch(batch); validation.heartbeat(heartbeat);
        assert.equal(batch.observations[0].rssi_dbm,-58.5);
    });
    await test('受信 HTTPS 成功，错误主机名与不受信证书失败',async()=>{
        assert.equal((await send('config')).status,200);
        await assert.rejects(send('config',undefined,pass.token,{servername:'wrong.invalid'}));
        await assert.rejects(send('config',undefined,pass.token,{ca:undefined}));
    });
    await test('真实接口接收 C 观测与重试幂等',async()=>{
        let r=await send('observations',batch); assert.equal(r.status,200); assert.equal(r.body.accepted,batch.observations.length);
        r=await send('observations',batch); assert.equal(r.body.duplicates,batch.observations.length);
        assert.equal(Number(db.prepare('SELECT count(*) n FROM ble_observations WHERE node_id=?').get(pass.node.id).n),batch.observations.length);
    });
    resetRate();
    await test('C 心跳、GET 配置、暂停后的版本确认',async()=>{
        let r=await send('heartbeat',heartbeat); assert.equal(r.status,200);
        repo.updateSettings('firmware-owner',pass.node.id,{scan_enabled:false,aggregation_window_ms:3000});
        r=await send('config'); assert.equal(r.body.scan_enabled,false); assert.equal(r.body.config_revision,2);
        r=await send('heartbeat',{...heartbeat,scan_state:'paused',applied_config_revision:2}); assert.equal(r.status,200);
        assert.equal(repo.listNodes('firmware-owner').find(n=>n.id===pass.node.id).scan_state,'paused');
        assert.equal((await send('heartbeat',{...heartbeat,applied_config_revision:3})).status,400);
    });
    resetRate();
    await test('坏条目整批拒绝、不发生部分写入',async()=>{
        const before=Number(db.prepare('SELECT count(*) n FROM ble_observations').get().n);
        const bad=structuredClone(batch); bad.observations[0].event_id='new-valid';
        bad.observations.push({...bad.observations[0],event_id:'invalid-item',rssi_dbm:0});
        assert.equal((await send('observations',bad)).status,400);
        assert.equal(Number(db.prepare('SELECT count(*) n FROM ble_observations').get().n),before);
    });
    await test('未知字段和 nullable 缺失不被静默接受',async()=>{
        assert.equal((await send('observations',{...batch,target_id:'forbidden'})).status,400);
        const missing=structuredClone(batch); delete missing.observations[0].local_name;
        assert.equal((await send('observations',missing)).status,400);
    });
    resetRate();
    await test('Token 绑定身份、HTTP类型及容量限制',async()=>{
        assert.equal((await send('observations',batch,mac.token)).status,403);
        assert.equal((await send('observations',batch,pass.token,{headers:{authorization:`Bearer ${pass.token}`,'content-type':'text/plain'}})).status,415);
        assert.equal((await send('observations','x'.repeat(262145))).status,413);
        const old=repo.LIMITS.observationsPerNode;
        try {
            repo.LIMITS.observationsPerNode=0;
            const fresh=structuredClone(batch); fresh.observations[0].event_id='capacity';
            const r=await send('observations',fresh); assert.equal(r.status,409); assert.equal(r.body.error.code,'OBSERVATION_LIMIT');
        } finally { repo.LIMITS.observationsPerNode=old; }
    });
    resetRate();
    await test('设备容量故障与历史容量故障区分',async()=>{
        const old=repo.LIMITS.devicesPerNode;
        try {
            repo.LIMITS.devicesPerNode=0;
            const fresh=structuredClone(batch); fresh.observations[0].event_id='new-device'; fresh.observations[0].local_device_id='C0:00:00:00:00:99';
            const r=await send('observations',fresh); assert.equal(r.status,409); assert.equal(r.body.error.code,'DEVICE_LIMIT');
        } finally { repo.LIMITS.devicesPerNode=old; }
    });
    await test('观测/心跳共享写限流，GET仍可用',async()=>{
        db.prepare('UPDATE ble_nodes SET rate_minute=?,rate_count=120 WHERE id=?').run(Math.floor(Date.now()/60000),pass.node.id);
        assert.equal((await send('observations',batch)).status,429);
        assert.equal((await send('heartbeat',heartbeat)).status,429);
        assert.equal((await send('config')).status,200);
    });
    await test('凭据轮换后旧Token停止认证',async()=>{
        const token=repo.rotateCredential('firmware-owner',pass.node.id).token;
        assert.equal((await send('config')).status,401); assert.equal((await send('config',undefined,token)).status,200);
    });
    await test('隔离账本与原Mac节点业务数据未变',()=>{
        assert.equal(db.prepare('SELECT value FROM ledger_guard').get().value,'unchanged');
        const after=repo.listNodes('firmware-owner').find(n=>n.id===mac.node.id);
        assert.deepEqual(after,baselineMac);
    });
    console.log(`Backend integration: PASS (${passed} scenarios, temporary SQLite, host HTTPS; device TLS NOT RUN)`);
} finally {
    if (server) { server.closeAllConnections(); await new Promise(r=>server.close(r)); }
    if (db) db.close();
    rmSync(temp,{recursive:true,force:true});
}
