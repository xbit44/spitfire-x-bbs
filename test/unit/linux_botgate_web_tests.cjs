'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict'),crypto=require('node:crypto');
let checks=0;const check=(value,message)=>{assert.ok(value,message);++checks;};
const ids=new Map();
class Node{
 constructor(tag='div'){Object.assign(this,{tag,children:[],value:'',textContent:'',disabled:false,hidden:false,open:false,attributes:{},className:''});}
 set id(value){this._id=value;ids.set(value,this);}get id(){return this._id;}
 append(...values){this.children.push(...values);}replaceChildren(...values){this.children=[...values];}
 setAttribute(k,v){this.attributes[k]=v;}addEventListener(name,action){(this.events??={})[name]=action;}
 showModal(){this.open=true;}close(){this.open=false;}
}
const html=fs.readFileSync('src/apps/linux_web/index.html','utf8');
for(const match of html.matchAll(/<([a-z][a-z0-9-]*)\b([^>]*)>/gi)){const id=/\bid="([^"]+)"/.exec(match[2]);if(id){const n=new Node(match[1]);n.id=id[1];}}
const $=id=>ids.get(id),source=fs.readFileSync('src/apps/linux_web/app.js','utf8');
const sha=text=>crypto.createHash('sha256').update(text).digest('hex');
const names=['ipfilter_exempt.cfg','ip.can','ip-silent.can','host.can','temp_ip.can','feed-exempt.can'];
const files=names.map((name,file)=>({file,name,label:['Exempt addresses','Blocked IP addresses','Silent IP blocks','Verified hostnames','Temporary bans','Feed exceptions'][file],readonly:file===4}));
const disk=Array.from({length:6},()=>''),calls=[],storage=new Map();let handler=null,notice='';
const feed=()=>({running:true,available:false,updating:false,entries:0,bytes_received:0,last_attempt:0,last_success:0,result:'feed_not_available',progress:'',rule_status:'rules_loaded',rules_ready:true});
let status=feed();
function read(file){return {file,files,text:Buffer.byteLength(disk[file])<=65535?disk[file]:'',revision:sha(disk[file]),available:true,editable:file!==4&&Buffer.byteLength(disk[file])<=65535,detail:file===4?'Temporary bans are read-only.':'Rule files save separately.'};}
function failure(status,diagnostic){const error=new Error(diagnostic);error.status=status;error.response={diagnostic};return error;}
async function api(path,body){calls.push({path,body:body===undefined?undefined:structuredClone(body)});if(handler){const value=await handler(path,body);if(value!==undefined)return value;}
 if(path.startsWith('/api/config/botgate/rule?file='))return read(Number(path.split('=').at(-1)));
 if(path==='/api/config/botgate/rule/save'){if(body.file===4||Buffer.byteLength(body.text)>65535)throw failure(400,'botgate_rule_invalid');if(sha(disk[body.file])!==body.revision)throw failure(409,'botgate_rule_changed');disk[body.file]=body.text;return {ok:true,file:body.file,revision:sha(body.text),detail:'Rule file saved. Valid rules reload within about two seconds.'};}
 if(path==='/api/config/botgate/status')return structuredClone(status);
 if(path==='/api/config/botgate/update')return {ok:true,diagnostic:'feed_update_queued',detail:'Update requested; no success claimed.'};
 throw Error('Unexpected API '+path);
}
const c=vm.createContext({$,element:(tag,text,cls)=>Object.assign(new Node(tag),{textContent:text??'',className:cls??''}),page:'config',route:'security.botgate',busy:false,saving:false,offlineBusy:false,offline:null,maintenanceActive:false,mainPending:null,qwkPending:null,dirty:false,qwkDirty:false,statisticsDirty:false,messageUtilities:{},draftKey:'isolated-draft',restoringDraft:false,recordId:null,board:{revision:'1',fields:[]},qwk:null,statistics:null,sessionStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)},safeDraftFields:{'security.botgate':['enabled','telnet_challenge','new_ssh_challenge','challenge_seconds']},messageDrafts:()=>null,messageUnresolved:()=>false,accountSecrets:()=>false,callerSearchRoute:()=>false,ftnRoute:()=>false,notice:v=>notice=v,TextEncoder,api,console,structuredClone,Date});
vm.runInContext(source.slice(source.indexOf('// BotGate rule files have'),source.length),c);
vm.runInContext(source.slice(source.indexOf('function hasDraft(){'),source.indexOf('function notice(')),c);
vm.runInContext(source.slice(source.indexOf('function keepDraft(){'),source.indexOf('function retainedDraft(')),c);
vm.runInContext('function setDirty(value){dirty=value;botgateControls();keepDraft();}',c);
const run=text=>vm.runInContext(text,c),state=()=>run('botgateRule');
function edit(text){$('botgate-rule-text').value=text;$('botgate-rule-text').oninput();}
function writeCalls(){return calls.filter(row=>row.body!==undefined);}
(async()=>{
 await run('botgateLoad()');
 check(!$('botgate-section').hidden&&$('botgate-file').children.length===6,'BBS Security displays the six fixed rule files');
 check($('botgate-rule-text').value===''&&!$('botgate-rule-text').disabled,'editable empty file loads');
 check(!$('botgate-feed-update').disabled,'no valid cached feed does not disable an initial manual update');
 check(writeCalls().length===0,'initial loads and status reads never send an update or rule save');
 edit('192.0.2.10\n');check(state().dirty&&run('hasDraft()')&&!run('dirty'),'rule draft joins navigation without marking protection settings dirty');
 const retained=JSON.parse(storage.get('isolated-draft'));check(retained.botgate.text==='192.0.2.10\n'&&retained.main===null,'only rule draft persists when protection settings are clean');
 await run('botgateLoad()');check(state().text==='192.0.2.10\n','settings readback does not overwrite a separate rule draft');
 await run('botgateTransition(()=>botgateReadRule(1))');check($('botgate-leave-dialog').open&&state().data.file===0,'selecting another file reviews unsaved rule changes');
 await run("botgateLeaveAnswer('stay')");check(!$('botgate-leave-dialog').open&&state().dirty&&state().data.file===0,'Stay preserves draft and selection');
 await run('botgateTransition(()=>botgateReadRule(1))');await run("botgateLeaveAnswer('save')");
 check(disk[0]==='192.0.2.10\n'&&state().data.file===1&&!state().dirty,'Save then select persists exact rule file and changes selection');
 check(writeCalls().filter(r=>r.path.endsWith('/rule/save')).length===1,'known save has one POST followed by readback');
 edit('198.51.100.3\n');await run('botgateTransition(()=>botgateReadRule(2))');await run("botgateLeaveAnswer('discard')");
 check(disk[1]===''&&state().data.file===2&&!state().dirty,'Discard then select leaves old file unchanged');
 await run('botgateReadRule(4)');check($('botgate-rule-text').disabled&&$('botgate-rule-save').disabled,'BBS-managed temporary bans remain read-only');
 edit('203.0.113.10');check(!state().dirty,'read-only input cannot form a save draft');await run('botgateSave()');check(disk[4]==='','temporary bans cannot be submitted');
 disk[3]='x'.repeat(65536);await run('botgateReadRule(3)');check(!state().data.editable&&$('botgate-rule-text').disabled,'oversized rule file is read-only');
 await run('botgateReadRule(1)');edit('203.0.113.9\n');disk[1]='192.0.2.9\n';
 await run('botgateSave()');check(state().dirty&&!state().pending&&disk[1]==='192.0.2.9\n','explicit stale refusal preserves draft and current disk file');
 await run('botgateTransition(()=>botgateReadRule(1))');await run("botgateLeaveAnswer('discard')");
 edit('203.0.113.11\n');let lost=true;handler=async(path,body)=>{if(path.endsWith('/rule/save')&&lost){lost=false;disk[body.file]=body.text;throw Error('reply lost');}};
 await run('botgateSave()');check(!!state().pending&&state().dirty&&$('botgate-rule-text').disabled,'lost reply keeps the candidate and locks editing');
 const writes=writeCalls().length;await run('botgateSave()');
 check(!state().pending&&!state().dirty&&writeCalls().length===writes,'Check Save Result confirms exact candidate without another POST');handler=null;
 edit('203.0.113.12\n');handler=async path=>{if(path.endsWith('/rule/save'))throw Error('transport failed');};
 await run('botgateSave()');check(state().pending&&!run('botgateCanDiscard()'),'unknown save initially blocks discard and selection');
 await run('botgateTransition(()=>botgateReadRule(0))');check(state().data.file===1&&!!state().pending,'unknown save cannot navigate directly to another rule');
 await run('botgateCheckSave()');check(state().pending&&run('botgateCanDiscard()'),'different canonical readback keeps draft and allows an explicit review/discard');
 const uncertainDraft=run('botgateDraft()');check(uncertainDraft.pending&&uncertainDraft.pending.observed===undefined,'retained unknown request has no untrusted canonical readback');
 await run('botgateTransition(()=>botgateReadRule(1))');check($('botgate-leave-dialog').open&&$('botgate-leave-save').disabled,'unknown readback review never offers a resend');
 await run("botgateLeaveAnswer('discard')");check(!state().pending&&!state().dirty&&disk[1]==='203.0.113.11\n','explicit discard reads current file without repeating failed save');handler=null;
 c.saved=uncertainDraft;run('botgateRestoreDraft(saved)');check(state().pending&&state().dirty&&!run('botgateCanDiscard()'),'restored unconfirmed save requires a fresh canonical check');
 const beforeWrites=writeCalls().length;await run('botgateCheckSave()');check(writeCalls().length===beforeWrites,'restored request uses readback only');
 run('botgateDiscard()');await run('botgateReadRule(0)');
 edit('198.51.100.80\n');const draftBefore=state().text;status={...feed(),available:true,entries:123,last_attempt:1700000000,last_success:1700000000,result:'feed_update_success'};
 await run('botgateRefreshFeed()');check(state().dirty&&state().text===draftBefore,'feed polling preserves rule draft');
 check($('botgate-feed-status').children[0].children.some(n=>String(n.textContent).includes('123 addresses')),'feed status shows active address count');
 check(run('botgateTime(1700000000)').endsWith(' UTC'),'feed timestamps explicitly identify UTC');
 for(const [result,expected] of [['feed_update_success','Update completed and verified'],['feed_cache_loaded','Saved threat feed loaded'],['feed_candidate_invalid','Downloaded feed was rejected'],['feed_worker_unavailable','Updater unavailable'],['feed_tls_certificate_date_invalid_check_clock','Certificate date check failed; check the server clock'],['feed_http_status_429','Download server returned an error; see update output'],['feed_https_curl_error_28','Secure download failed; see update output'],['future_feed_success','Outcome unavailable; see update output']]){
  c.labelCase={...feed(),available:true,result};check(run('botgateFeedLabel(labelCase)')===expected,'plain feed label for '+result);
 }
 c.labelCase={...feed(),updating:true,result:'feed_downloading'};check(run('botgateFeedLabel(labelCase)')==='Updating threat feed','working feed uses plain status');
 c.labelCase.result='feed_validating';check(run('botgateFeedLabel(labelCase)')==='Checking downloaded feed','validation feed uses plain status');
 c.labelCase={...feed(),available:false,result:'feed_update_success'};check(run('botgateFeedLabel(labelCase)').startsWith('Feed unavailable'),'success code without an available cache does not assert verified readiness');
 c.labelCase={...feed(),rules_ready:true,rule_status:'rules_loaded'};check(run('botgateRuleLabel(labelCase)')==='Rules loaded and active','loaded rule state uses plain words');
 c.labelCase.rule_status='botgate_rule_parse_failed';check(run('botgateRuleLabel(labelCase)')==='Last good rules active; latest reload failed','latest reload failure preserves truthful last-good activity');
 c.labelCase.rule_status='future_rules_success';check(run('botgateRuleLabel(labelCase)').includes('outcome unavailable'),'unknown rule code does not invent reload success or failure');
 c.labelCase.rules_ready=false;c.labelCase.rule_status='rules_loaded';check(!run('botgateRuleLabel(labelCase)').includes('loaded and active'),'a loaded code without a ready snapshot does not assert active rules');
 status={...feed(),available:true,result:'feed_update_success',rule_status:'rules_loaded'};await run('botgateRefreshFeed()');
 const primary=$('botgate-feed-status').children[0].children.map(n=>n.textContent).join('\n');
 check(!primary.includes('feed_update_success')&&!primary.includes('rules_loaded')&&primary.includes('Update completed and verified'),'primary status values contain human labels, not success implementation identifiers');
 status={...feed(),available:true,result:'feed_candidate_invalid',rule_status:'botgate_rule_parse_failed',progress:'[ERROR] feed_candidate_invalid; last good feed retained.'};await run('botgateRefreshFeed()');
 check($('botgate-feed-status').children.some(n=>n.tag==='pre'&&n.textContent.includes('feed_candidate_invalid')),'actual update error output retains the native diagnostic');
 const diagnostic=$('botgate-feed-status').children.find(n=>n.tag==='details');check(!!diagnostic&&diagnostic.children[1].textContent.includes('botgate_rule_parse_failed'),'rule reload failures retain raw details separately from the human primary label');
 status=feed();await run('botgateRefreshFeed()');await run('botgateUpdateFeed()');
 check(writeCalls().at(-1).path==='/api/config/botgate/update'&&Object.keys(writeCalls().at(-1).body).length===0,'manual update sends only the bounded empty update request');
 check(!!run('botgateFeed.requested')&&$('botgate-feed-update').disabled,'queued update stays distinct from verified success and blocks a duplicate');
 const updateWrites=writeCalls().length;await run('botgateUpdateFeed()');check(writeCalls().length===updateWrites,'repeated clicks cannot queue another update');
 status={...feed(),updating:true,last_attempt:1,bytes_received:321,progress:'Downloading 321 bytes'};await run('botgateRefreshFeed()');check(!run('botgateFeed.requested')&&$('botgate-feed-update').disabled,'observed updater activity clears local queue marker but stays busy');
 status={...feed(),available:true,last_attempt:1,last_success:1,entries:10,result:'feed_update_success'};await run('botgateRefreshFeed()');check(!$('botgate-feed-update').disabled,'completed status releases manual update control');
 handler=async path=>{if(path.endsWith('/update'))throw Error('reply lost');};await run('botgateUpdateFeed()');
 check(run('botgateFeed.uncertain')&&$('botgate-feed-update').disabled,'unknown update result blocks a blind second request');
 const unknownWrites=writeCalls().length;await run('botgateRefreshFeed(true)');check(!run('botgateFeed.uncertain')&&writeCalls().length===unknownWrites,'explicit status review clears uncertain marker without sending update');handler=null;
 const beforeLarge=writeCalls().length;edit('\t'.repeat(65535));await run('botgateSave()');
 check(state().dirty&&!state().pending&&writeCalls().length===beforeLarge,'escaped raw-maximum file is refused before its encoded JSON exceeds the request bound');
 check($('botgate-rule-result').textContent.includes('encoding'),'encoded-size refusal explains how to keep or shorten the draft');
 run('botgateDiscard()');disk[0]='x'.repeat(65536);await run('botgateReadRule(0)');
 c.oversizedDraft={file:0,revision:sha(''),base_text:'',text:'192.0.2.1\n',pending:null};run('botgateRestoreDraft(oversizedDraft)');
 check(!state().data.editable&&state().dirty&&$('botgate-rule-text').disabled,'restoring a draft never makes an oversized current file editable');
 disk[0]='';run('botgateDiscard()');await run('botgateReadRule(0)');
 c.otherFileDraft={file:2,revision:sha(''),base_text:'',text:'192.0.2.2\n',pending:null};const restoring=run('botgateRestoreDraft(otherFileDraft)');
 check(state().reading&&$('botgate-rule-text').disabled,'restored selection stays locked until that exact current file is inspected');
 await restoring;check(state().data.file===2&&state().text==='192.0.2.2\n'&&state().data.editable,'exact-file restore readback preserves the old draft baseline and current editing restrictions');
 run('botgateDiscard()');handler=async path=>{if(path.includes('/rule?file='))throw failure(409,'botgate_rule_unavailable');};await run('botgateReadRule(2)');
 check(state().data.available===false&&!state().data.editable&&$('botgate-rule-text').disabled,'failed current-file inspection locks the stale editor until successful readback');handler=null;await run('botgateReadRule(2)');
 check(state().data.editable&&!$('botgate-rule-text').disabled,'successful rule readback restores editing after a read failure');
 status={...feed(),running:false,entries:999,last_success:0};await run('botgateRefreshFeed()');
 check($('botgate-feed-update').disabled&&$('botgate-feed-status').children[0].children.some(n=>n.textContent==='Not running'),'stopped runtime offers no manual fetch and reports Not running instead of a false zero');
 c.page='panel';run('botgatePageChanged()');check($('botgate-section').hidden&&!state().data&&!run('hasDraft()'),'accepted page change clears only the finished BotGate view');
 check(source.includes('botgateRestoreDraft(value.botgate)')&&source.includes('botgate:botgateDraft()')&&source.includes('if(botgateHasDraft()&&!await botgateSave())return;'),'main session and navigation Save use the separate rule draft');
 check(source.includes('botgateDiscard();setDirty(false)')&&source.includes('if(requested===\'security.botgate\')await botgateLoad()'),'navigation Discard and settings reload integrate the rule scope');
 console.log('PASS '+checks+' Linux BotGate rule/editor/draft/feed checks');
})().catch(error=>{console.error(error);process.exitCode=1;});
