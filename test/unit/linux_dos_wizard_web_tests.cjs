const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
let checks=0;const check=(value,message)=>{assert.ok(value,message);++checks;};
const elements=new Map(),storage=new Map(),calls=[];
class Element{
  constructor(tag='div'){this.tag=tag;this.type='';this.value='';this.checked=false;this.open=false;this.disabled=false;this.hidden=false;this.textContent='';this.children=[];this.events={};}
  append(...items){this.children.push(...items);if(this.tag==='select'&&this.value===''&&items[0])this.value=items[0].value;}
  replaceChildren(...items){this.children=[...items];this.value='';}
  addEventListener(name,action){(this.events[name]??=[]).push(action);}
  fire(name){for(const action of this.events[name]||[])action({});}
  querySelectorAll(){return [...elements.values()].filter(e=>['input','select','button'].includes(e.tag));}
  get selectedOptions(){return this.children.filter(e=>e.value===this.value);}
  showModal(){this.open=true;}close(){this.open=false;}
}
const $=id=>{if(!elements.has(id)){const select=['candidate','program','io','drop','drop-location','menu'].some(s=>id==='dw-'+s);const e=new Element(select?'select':/dw-(name|directory|command|input-interval|security|sysop|multiple|number|menu-title|description|binary|root)$/.test(id)?'input':'button');if(id==='dw-sysop'||id==='dw-multiple')e.type='checkbox';if(id==='dw-input-interval'){e.type='number';e.value='0';}elements.set(id,e);}return elements.get(id);};
let revision=1,doors=[],entries=[],runtimeRevision=0,phaseFault='',nextId=1;
const gameDrivers=new Map();
function gameDriver(directory){if(!gameDrivers.has(directory))gameDrivers.set(directory,{ready:false,interval:0,revision:0});return gameDrivers.get(directory);}
const driverIdentity=driver=>driver.ready?driver.revision.toString(16).padStart(64,'f'):'absent';
const runtime={binary:'/fixture/runtime/dosemu2.bin',root:'/fixture/runtime'};
let checked=false,savedRuntime={binary:'',root:''};
const status=()=>({active:false,job:'c'.repeat(32),checked,ok:checked,detail:'Fixture DOS status',revision:String(runtimeRevision),check_revision:String(runtimeRevision),saved:{...savedRuntime},saved_identity:runtimeRevision?'f'.repeat(64):'',checked_identity:checked?'f'.repeat(64):'',runtime:{...runtime},version:'DOSEMU2 fixture',can_prepare:false,candidates:[runtime]});
const catalog=menu=>({revision:String(revision),operation:revision.toString(16).padStart(32,'a'),menu,doors:doors.map(d=>({id:d.id,name:d.name,code:d.code,enabled:d.enabled})),menus:[{id:0,name:'Door Menu'},{id:1,name:'Games'}],entries:entries.filter(e=>e.menu===menu).map(e=>({...e}))});
const error=(message,unresolved,status=409)=>Object.assign(new Error(message),{status,response:{detail:message,unresolved}});
async function api(path,body){
  calls.push({path,body:body===undefined?undefined:structuredClone(body)});
  if(path.endsWith('/dos/status'))return status();
  if(path.endsWith('/dos/start')){checked=true;return status();}
  if(path.endsWith('/dos/save')){check(body.revision===String(runtimeRevision),'runtime save uses observed revision');++runtimeRevision;savedRuntime={...runtime};if(phaseFault==='runtime-lost'){phaseFault='';throw error('Lost runtime response',true,503);}return status();}
  if(path.endsWith('/dos/game')){const driver=gameDriver(body.directory);return {directory:body.directory,programs:['GAME.EXE','START.BAT'],driver_identity:driverIdentity(driver),driver_change:!driver.ready,input_interval_ms:driver.interval,driver_preview:driver.ready?'Already prepared':'Set CanDisconnect=false'};}
  if(path.endsWith('/dos/prepare-game')){
    check(body.runtime_revision===String(runtimeRevision),'game preparation binds saved runtime revision');
    const driver=gameDriver(body.directory);check(body.driver_identity===driverIdentity(driver),'game preparation binds observed driver identity');
    check(Number.isInteger(body.input_interval_ms)&&body.input_interval_ms>=0&&body.input_interval_ms<=100,'game preparation sends bounded numeric pacing');
    driver.ready=true;driver.interval=body.input_interval_ms;++driver.revision;
    if(phaseFault==='driver-uncertain'){phaseFault='';throw error('Lost committed driver response',true,503);}
    if(phaseFault==='driver-response-mismatch'){phaseFault='';return {ok:true,input_interval_ms:0,driver_identity:driverIdentity(driver)};}
    return {ok:true,input_interval_ms:driver.interval,driver_identity:driverIdentity(driver)};
  }
  if(path.includes('/catalog?menu='))return catalog(Number(path.split('=').at(-1)));
  if(path.includes('/door/')){const id=Number(path.split('/').at(-1));return {revision:String(revision),operation:revision.toString(16).padStart(32,'a'),entry:id?structuredClone(doors.find(d=>d.id===id)):{id:0,name:'',code:'',enabled:true,password:'',pepper:'',authentication:0,caller_password:false,salt:'',banner:false,clear_password:false,clear_pepper:false,doordrop_utf8:false}};}
  if(path.endsWith('/change')){
    check(body.revision===String(revision),'Door/menu write binds current catalog revision');
    if(body.action==='door'){
      const d={...structuredClone(body.entry),id:nextId++};doors.push(d);++revision;
      if(phaseFault==='door-uncertain'){phaseFault='';throw error('Lost Door response',true,503);}
      return {ok:true,door_id:d.id,revision:String(revision)};
    }
    if(phaseFault==='entry-known'){phaseFault='';throw error('Menu number changed',false);}
    entries.push(structuredClone(body.entry.placement));++revision;
    if(phaseFault==='entry-uncertain'){phaseFault='';throw error('Lost menu response',true,503);}
    return {ok:true,revision:String(revision)};
  }
  throw new Error('Unexpected route '+path);
}
const context=vm.createContext({doorLeaveAsk(request){context.leaveRequest=request;},$,api,element:(tag,text)=>Object.assign(new Element(tag),{textContent:text}),draftKey:'fixture',authenticated:true,doorCatalogLocked:()=>false,sessionStorage:{setItem:(k,v)=>storage.set(k,v),getItem:k=>storage.get(k)||null,removeItem:k=>storage.delete(k)},structuredClone,window:{addEventListener(){}},setTimeout:()=>0,clearTimeout(){},console});
const source=fs.readFileSync('src/apps/linux_web/app.js','utf8');const marker='// DOS Door Setup owns its nonsecret';check(source.includes(marker),'test runs integrated shipping wizard');vm.runInContext(source.slice(source.indexOf(marker)),context);
const run=code=>vm.runInContext(code,context);
function fillGame(name){$('dw-name').value=name;$('dw-directory').value='/fixture/games/'+name;$('dw-command').value='START.BAT /NODE:%#';$('dw-security').value='10';$('dw-sysop').checked=true;$('dw-multiple').checked=false;$('dw-drop-location').value='0';$('dw-io').value='';$('dw-drop').value='';$('dw-menu').value='0';$('dw-menu-title').value='';$('dw-number').value='';}
function pacing(value){$('dw-input-interval').value=String(value);$('dw-input-interval').fire('input');}
async function ensureRuntime(){if(run('dw.step')===0){await run("dwStart('check')");await run('dwNext()');}}
async function finishReview(){await run('dwNext()');await run('dwNext()');await run('dwNext()');check(run('dw.step')===4,'explicitly reviewed driver resumes through caller/menu/review');}
async function toReview(name,interval){
  fillGame(name);await run('dwScan()');await run('dwNext()');check(run('dw.step')===2,'game folder and command advance to caller step');
  await run('dwNext()');check(run('dw.step')===2,'unknown I/O/drop requirements are never guessed');
  $('dw-io').value='3';$('dw-drop').value='3';if(interval!==undefined)pacing(interval);await run('dwNext()');await run('dwNext()');check(run('dw.step')===4,'explicit caller and menu choices reach review');check($('dw-result').textContent.includes('Save and add to '),'review has current save instructions rather than stale check/folder text');
}
(async()=>{
  const html=fs.readFileSync('src/apps/linux_web/index.html','utf8');for(const id of ['dos-wizard-dialog','dw-page-0','dw-page-5','dw-working','dw-result','dw-input-interval','dw-input-help'])check(html.includes('id="'+id+'"'),'shipped dialog has '+id);
  check(/id="dw-input-interval"[^>]*type="number"[^>]*min="0"[^>]*max="100"[^>]*step="1"[^>]*value="0"/.test(html),'input pacing HTML has integer0..100 and fresh default0');
  await run('dwOpen()');check($('dos-wizard-dialog').open,'wizard opens from authenticated Doors');check(calls.every(c=>!c.path.endsWith('/dos/start')),'opening only reads; no emulator command or download starts automatically');check($('dw-prepare').disabled,'unqualified automatic profile remains disabled');
  await run("dwStart('check')");await run('dwNext()');check(run('dw.runtimeSaved')&&run('dw.step')===1,'checked runtime choice is saved before game setup');
  await toReview('<img onerror=bad>');check($('dw-input-interval').value==='0'&&$('dw-summary').textContent.includes('Input pacing: 0 ms (no added delay)'),'fresh game preserves no pacing default and explains it');check($('dw-summary').textContent.includes('<img onerror=bad>'),'review treats user text as inert text');await run('dwNext()');check(doors.length===1&&entries.length===1&&run('dw.step')===5,'normal flow creates Door and menu entry');check(doors[0].command==='CALL START.BAT /NODE:%#','DOS batch command returns through generated launcher');check(doors[0].program_type===0&&doors[0].io===3&&doors[0].drop_file===3&&doors[0].access.sysop_only,'DOS type, chosen I/O/drop and initial Sysop-only testing are saved');check(entries[0].target===doors[0].id,'menu uses confirmed full Door identity');
  $('dw-another').onclick();check($('dw-input-interval').value==='0'&&!run('dw.inputDraft'),'Add Another resets pacing without inheriting another game choice');await toReview('Second',75);
  for(const bad of ['',' ','-1','101','75.5','NaN','Infinity']){pacing(bad);assert.throws(()=>run('dwCallerValid()'),/whole-number input pacing/);++checks;}
  for(const good of ['0','75','100']){pacing(good);check(run('dwCallerValid()')["input-interval"]===good,'bounded whole-number pacing accepted');}
  pacing(75);run('dwSummary()');check($('dw-summary').textContent.includes('Input pacing: 75 ms')&&$('dw-summary').textContent.includes('Save this game’s input pacing at 75 ms'),'review shows chosen75 and per-game save');
  phaseFault='entry-known';await run('dwNext()');check(gameDriver('/fixture/games/Second').interval===75&&calls.findLast(c=>c.path.endsWith('/dos/prepare-game')).body.input_interval_ms===75,'chosen75 is sent numerically and saved for this game');check(gameDriver('/fixture/games/<img onerror=bad>').interval===0,'saving75 does not change earlier game0');check(doors.length===2&&entries.length===1&&run('dw.savedDoor')===2&&run('dw.step')===3,'known placement refusal retains saved Door for explicit correction');await run('dwNext()');await run('dwNext()');check(doors.length===2&&entries.length===2&&run('dw.step')===5,'retry places existing Door without duplicate creation');
  $('dw-another').onclick();await toReview('Third');phaseFault='door-uncertain';await run('dwNext()');check(run('dw.uncertain')&&$('dw-next').disabled,'uncertain Door response locks further writes');const writes=calls.filter(c=>c.path.endsWith('/change')).length;run('dwClose(true)');await run('dwOpen()');check(run('dw.uncertain')&&$('dw-name').value==='Third','unconfirmed draft survives close and reopen');check(calls.filter(c=>c.path.endsWith('/change')).length===writes,'reopening never repeats the request');doors[2].access.sysop_only=false;await run('dwReview()');check(run('dw.uncertain'),'readback refuses access settings changed after uncertain save');doors[2].access.sysop_only=true;await run('dwReview()');check(!run('dw.uncertain')&&run('dw.savedDoor')===3&&run('dw.step')===3,'explicit readback reconciles exact saved Door and access');await run('dwNext()');await run('dwNext()');check(doors.length===3&&entries.length===3,'reconciled Door receives one menu placement');
  // Restore the runtime check, then exercise a committed menu response lost in transit.
  $('dw-another').onclick();check(run('dw.step')===0,'restored tab rechecks runtime before adding another game');await run("dwStart('check')");await run('dwNext()');await toReview('Fourth');phaseFault='entry-uncertain';await run('dwNext()');check(run('dw.uncertain')&&doors.length===4&&entries.length===4,'uncertain menu outcome preserves saved Door and request');const before=calls.filter(c=>c.path.endsWith('/change')).length;await run('dwReview()');check(!run('dw.uncertain')&&run('dw.step')===5,'exact saved menu entry reconciles by explicit readback');check(calls.filter(c=>c.path.endsWith('/change')).length===before,'readback performs no writes');
  run('dwClose(true)');await run('dwOpen()');check(run('dw.step')===5&&run('dw.savedDoor')===4,'completed wizard reopens at caller test rather than offering duplicate placement');
  $('dw-another').onclick();await ensureRuntime();await toReview('DriverUncertain',75);run('dwClose(true)');await run('dwOpen()');check($('dw-input-interval').value==='75'&&run('dw.inputDraft'),'nonzero pacing draft survives close/reopen');await ensureRuntime();await run('dwScan()');check($('dw-input-interval').value==='75','rechecking an unprepared folder keeps explicit pacing draft');await finishReview();
  phaseFault='driver-uncertain';const priorDoors=doors.length,priorEntries=entries.length;await run('dwNext()');check(run('dw.uncertain')&&run('dw.pending.kind')==='driver'&&run('dw.pending.input_interval_ms')===75&&doors.length===priorDoors&&entries.length===priorEntries,'lost committed driver response locks further writes before Door creation');
  const beforeDriverReview=calls.filter(c=>c.path.endsWith('/dos/prepare-game')||c.path.endsWith('/change')).length;
  gameDriver('/fixture/games/DriverUncertain').interval=0;await run('dwReview()');check(run('dw.uncertain')&&$('dw-next').disabled&&$('dw-result').textContent.includes('saved input pacing differs'),'readback mismatch refuses to treat0 as requested75');
  gameDriver('/fixture/games/DriverUncertain').interval=75;await run('dwReview()');check(!run('dw.uncertain')&&run('dw.step')===1&&$('dw-input-interval').value==='75','exact saved75 reconciles without repeating driver write');check(calls.filter(c=>c.path.endsWith('/dos/prepare-game')||c.path.endsWith('/change')).length===beforeDriverReview,'both driver readbacks are read-only');await finishReview();await run('dwNext()');check(doors.length===priorDoors+1&&entries.length===priorEntries+1,'explicitly resumed driver flow creates one Door/menu');
  $('dw-another').onclick();await ensureRuntime();await toReview('DriverResponseMismatch',75);phaseFault='driver-response-mismatch';const beforeMalformed=doors.length;await run('dwNext()');check(run('dw.uncertain')&&run('dw.pending.kind')==='driver'&&doors.length===beforeMalformed,'different successful response pacing is unconfirmed and cannot create a Door');await run('dwReview()');check(!run('dw.uncertain')&&$('dw-input-interval').value==='75','readback safely reconciles the committed request after a malformed response');await finishReview();await run('dwNext()');
  $('dw-another').onclick();check($('dw-input-interval').value==='0','Add Another resets previous75');fillGame('Saved75');gameDrivers.set('/fixture/games/Saved75',{ready:true,interval:75,revision:1});await run('dwScan()');check($('dw-input-interval').value==='75','folder scan loads saved game pacing');pacing(0);await run('dwScan()');check($('dw-input-interval').value==='0'&&run('dw.inputDraft'),'rescan preserves explicit0 rather than restoring saved75');run('dwClose(true)');await run('dwOpen()');check($('dw-input-interval').value==='0','explicit zero draft survives reopen');
  $('dw-another').onclick();$('dw-binary').value='/custom/pending/dosemu2.bin';$('dw-root').value='/custom/pending';run('dwClose(true)');await run('dwOpen()');check($('dw-binary').value==='/custom/pending/dosemu2.bin'&&$('dw-root').value==='/custom/pending','unsaved runtime paths survive close and reopen');check(!run('dw.status.checked'),'restored different paths cannot borrow earlier runtime check');
  run('dwLock()');check(!$('dos-wizard-dialog').open&&!run('dw.runtimeSaved'),'authentication lock closes setup and clears runtime approval');check(!source.slice(source.indexOf(marker)).includes('innerHTML'),'wizard never injects user markup');
  const retainedGame=storage.get('fixture-dos-door'),runtimeStart=calls.length,oldDoors=doors.length;
  await run('dwOpen({runtimeOnly:true})');check(run('dw.runtimeOnly')&&run('dw.step')===0,'separate runtime utility opens only runtime stage');check(!calls.slice(runtimeStart).some(c=>c.path.includes('/catalog')),'runtime-only open never fetches menu categories');
  await run("dwStart('check')");await run('dwNext()');check(run('dw.step')===0&&run('dw.runtimeSaved')&&$('dw-next').disabled,'runtime save stays in runtime tool without advancing to game');check(doors.length===oldDoors&&storage.get('fixture-dos-door')===retainedGame,'runtime-only save does not create Door or overwrite game draft');
  run('dwClose(true)');await run('dwOpen({runtimeOnly:true})');await run("dwStart('check')");phaseFault='runtime-lost';await run('dwNext()');check(run('dw.uncertain')&&run('dw.pending.kind')==='runtime','runtime-only lost response retains exact request');
  run('dwClose()');check($('dos-wizard-dialog').open&&!context.leaveRequest.save&&!context.leaveRequest.discard,'unconfirmed runtime close blocks save/discard and offers explicit keep');context.leaveRequest.keep();await run('dwOpen({runtimeOnly:true})');check(run('dw.uncertain')&&run('dw.pending.kind')==='runtime','runtime-only pending survives close/reopen in separate key');const beforeRuntimeReview=calls.filter(c=>c.body).length;await run('dwReview()');check(!run('dw.uncertain')&&run('dw.step')===0&&run('dw.runtimeSaved'),'runtime readback confirms save without game transition');check(calls.filter(c=>c.body).length===beforeRuntimeReview,'runtime readback performs no writes');
  run('dwClose(true)');await run('dwOpen({runtimeOnly:true})');const gameBeforeDiscard=storage.get('fixture-dos-door');$('dw-binary').value='/another/unqualified/dosemu2.bin';$('dw-binary').fire('input');run('dwClose()');check($('dos-wizard-dialog').open&&!context.leaveRequest.save&&context.leaveRequest.discard,'unchecked runtime path edits offer Discard Stay and cannot save on close');context.leaveRequest.discard();check(!$('dos-wizard-dialog').open&&storage.get('fixture-dos-door')===gameBeforeDiscard,'discarding runtime-only paths preserves complete Add Door draft');
  await run('dwOpen({menu:1})');check($('dw-menu').value==='1','Add DOS Door preselects supplied category');$('dw-name').value='CloseDraft';run('dwClose()');check(!context.leaveRequest.save&&context.leaveRequest.discard,'incomplete game close offers discard/stay without skipping review');check(!calls.slice(runtimeStart).some(c=>c.path.endsWith('/dos/prepare-game')||c.path.endsWith('/change')),'runtime utility creates no game driver, Door or menu entry');
  console.log('PASS '+checks+' current Linux DOS wizard flow/draft/partial-save/readback checks');
})().catch(e=>{console.error(e);process.exitCode=1;});
