#!/usr/bin/env node
// Execute the embedded production scripts with controlled DOM/HTTP ports.
// This is an offline browser-logic regression test, not captive-portal HIL.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assets = path.join(__dirname, '../components/wqn_wifi_provision/assets');
const source = name => fs.readFileSync(path.join(assets, name), 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const settle = () => new Promise(setImmediate);
let checks = 0;
function check(value) { assert.ok(value); ++checks; }

async function page(initial = {}, options = {}) {
  const elements = new Map(), requests = [], timers = [], store = {primary_ssid:'',backup_ssid:'',...initial};
  let resolveSubmit;
  const element = id => {
    if (!elements.has(id)) elements.set(id, {
      value:'',textContent:'',className:'',checked:false,disabled:false,hidden:false,handlers:{},
      classList:{toggle(){}},addEventListener(name, fn){this.handlers[name]=fn},appendChild(){},
    });
    return elements.get(id);
  };
  const context = vm.createContext({
    document:{getElementById:element,createElement:()=>element(Symbol())},TextEncoder,
    location:{href:'/'},setInterval(){},setTimeout(fn, delay){timers.push({fn,delay})},
    fetch:async (url, request) => {
      requests.push({url,request});
      if (url === '/networks') {
        if (options.readFailure) throw new Error('offline');
        return {ok:true,json:async()=>({...store})};
      }
      if (url === '/scan') return {ok:true,json:async()=>({aps:[]})};
      if (url === '/submit') {
        const body = JSON.parse(request.body);
        if (options.delayed) await new Promise(resolve=>resolveSubmit=resolve);
        if (options.saveFailure) return {ok:true,json:async()=>({success:false,error:'Save failed'})};
        store[body.role+'_ssid'] = body.ssid;
        return {ok:true,json:async()=>({success:true,connect:body.role==='primary'})};
      }
      if (options.exitFailure) throw new Error('exit not acknowledged');
      return {ok:true,json:async()=>({success:!options.exitRejected})};
    },
  });
  vm.runInContext(source(options.done?'wifi_configuration_done.html':'wifi_configuration.html'),context);
  await settle();
  return {
    element,context,requests,timers,store,options,
    role:role=>vm.runInContext(`chooseRole('${role}')`,context),
    input:(id,value)=>{element(id).value=value;element(id).handlers.input?.()},
    submit:()=>element('form').handlers.submit({preventDefault(){}}),
    finish:()=>element('finish').handlers.click(),resolve:()=>resolveSubmit(),
  };
}

(async()=>{
  const first=await page();
  check(first.element('backup-role').disabled);
  first.input('ssid','Primary');first.input('password','synthetic-primary');await first.submit();
  check(first.store.primary_ssid==='Primary'&&!first.element('backup-role').disabled);
  check(first.context.location.href==='/'&&first.timers.length===0);
  first.role('backup');first.input('ssid','Backup');await first.submit();
  check(first.store.backup_ssid==='Backup'&&first.context.location.href==='/');
  check(first.requests.filter(r=>r.url==='/submit').map(r=>JSON.parse(r.request.body).role).join() === 'primary,backup');
  first.finish();check(first.context.location.href==='/done.html');
  check(!first.requests.some(r=>r.url==='/exit'));

  const draft=await page({primary_ssid:'Primary',backup_ssid:'Backup'});
  draft.input('ssid','Draft');draft.input('password','synthetic-draft');draft.role('backup');draft.role('primary');
  check(draft.element('ssid').value==='Draft'&&draft.element('password').value==='synthetic-draft');
  draft.role('backup');draft.finish();check(draft.context.location.href==='/'&&draft.element('msg').className.includes('error'));
  draft.role('primary');await draft.submit();draft.finish();check(draft.context.location.href==='/done.html');

  const pending=await page({primary_ssid:'Primary',backup_ssid:'Backup'},{delayed:true});
  pending.role('backup');pending.input('ssid','Backup-new');const saving=pending.submit();
  check(pending.element('primary-role').disabled&&pending.element('finish').disabled);
  pending.role('primary');pending.finish();check(pending.context.location.href==='/');
  pending.resolve();await saving;
  check(pending.element('primary-label').textContent==='Primary'&&pending.element('backup-label').textContent==='Backup-new');
  check(!pending.element('primary-role').disabled&&!pending.element('submit').disabled);

  const keep=await page({primary_ssid:'Primary'});await keep.submit();
  check(JSON.parse(keep.requests.find(r=>r.url==='/submit').request.body).keep_password);
  keep.input('ssid','汉'.repeat(11));await keep.submit();
  check(keep.requests.filter(r=>r.url==='/submit').length===1);
  const failed=await page({primary_ssid:'Primary'},{saveFailure:true});
  failed.input('ssid','Changed');failed.input('password','synthetic-retained');await failed.submit();
  check(failed.element('ssid').value==='Changed'&&failed.element('password').value==='synthetic-retained');
  check(failed.element('primary-label').textContent==='Primary'&&!failed.element('submit').disabled);
  const unreadable=await page({}, {readFailure:true});check(unreadable.element('submit').disabled&&unreadable.element('ssid').disabled);

  const done=await page({}, {done:true});
  check(done.element('title').textContent==='配置已保存'&&done.requests.length===1);
  for(const option of ['exitFailure','exitRejected']) {
    const retry=await page({}, {done:true,[option]:true});
    check(!retry.element('retry').hidden&&retry.element('title').textContent==='未收到结束确认');
    retry.options[option]=false;await retry.element('retry').handlers.click();
    check(retry.element('retry').hidden&&retry.element('title').textContent==='配置已保存');
  }
  console.log(`WiFi provisioning: ${checks} checks PASS (production JavaScript; offline ports only)`);
})().catch(error=>{console.error(error);process.exitCode=1});
