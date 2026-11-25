'use strict';
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
let checks=0,requests=0;function check(v,m){assert.ok(v,m);++checks;}
const ids=new Map();
class Node{
 constructor(tag='div'){Object.assign(this,{tag,children:[],parentElement:null,dataset:{},style:{},attributes:{},value:'',type:'',checked:false,disabled:false,hidden:false,textContent:'',className:''});this.classList={toggle:(c,on)=>{const s=new Set(this.className.split(' ').filter(Boolean));if(on??!s.has(c))s.add(c);else s.delete(c);this.className=[...s].join(' ');},add:(...c)=>{for(const v of c)this.classList.toggle(v,true);},remove:(...c)=>{for(const v of c)this.classList.toggle(v,false);}};}
 set id(v){this._id=v;ids.set(v,this);}get id(){return this._id;}
 append(...nodes){for(const n of nodes){if(n.parentElement)n.parentElement.children=n.parentElement.children.filter(x=>x!==n);n.parentElement=this;this.children.push(n);}}
 replaceChildren(...nodes){const detach=n=>{n.parentElement=null;for(const c of n.children)detach(c);if(n.id&&ids.get(n.id)===n)ids.delete(n.id);};for(const n of this.children)detach(n);this.children=[];this.append(...nodes);}
 prepend(n){this.append(n);this.children=[n,...this.children.filter(x=>x!==n)];}
 setAttribute(k,v){this.attributes[k]=String(v);}getAttribute(k){return this.attributes[k];}removeAttribute(k){delete this.attributes[k];}
 querySelectorAll(selector){const match=n=>selector.split(',').some(s=>s.startsWith('.')?n.className.split(' ').includes(s.slice(1)):n.tag===s);return this.children.flatMap(n=>[...(match(n)?[n]:[]),...n.querySelectorAll(selector)]);}
 querySelector(s){return this.querySelectorAll(s)[0]||null;}addEventListener(){}scrollIntoView(){}focus(){}
}
const html=fs.readFileSync('src/apps/linux_web/index.html','utf8'),stack=[];
for(const m of html.matchAll(/<(\/?)([a-z][a-z0-9-]*)\b([^>]*)>/gi)){const tag=m[2].toLowerCase();if(m[1]){const at=stack.map(n=>n.tag).lastIndexOf(tag);if(at>=0)stack.splice(at);continue;}const n=new Node(tag);for(const a of m[3].matchAll(/([\w-]+)(?:="([^"]*)")?/g)){if(a[1]==='id')n.id=a[2];else if(a[1]==='class')n.className=a[2];else if(a[1]==='hidden')n.hidden=true;n.attributes[a[1]]=a[2]??'';}stack.at(-1)?.append(n);if(!['input','meta','link','hr','br','img','source'].includes(tag))stack.push(n);}
const $=id=>ids.get(id)||null;
const source=fs.readFileSync('src/apps/linux_web/app.js','utf8');
const c=vm.createContext({$,element:(tag,text,cls)=>Object.assign(new Node(tag),{textContent:text,className:cls||''}),page:'config',route:'general.board_identity',board:null,busy:false,saving:false,offlineBusy:false,offline:null,maintenanceActive:false,mainPending:null,
 doorSafeKeys:[],doorCatalogRoute:r=>r==='doors.manager',catalogRoute:r=>r==='messages.local_echoes'||r==='messages.clone_permissions',fileCatalogRoute:r=>r==='files.groups_areas',recordRoute:r=>['chat.rooms','users.accounts','users.security_levels','files.count_rules','files.caller_exemptions','tools.scheduled_jobs','general.door_commands'].includes(r),callerSearchRoute:r=>['users.accounts','files.caller_exemptions'].includes(r),throttleRoute:r=>r==='security.throttle_recovery',systemPasswordRoute:r=>r==='system.system_password',accountRoute:r=>r==='users.accounts',securityRoute:r=>r==='users.security_levels',catalogState:{entry:null,action:'group'},fileCatalogState:{entry:null},api(){++requests;throw new Error('Presentation must never call API');},notice(){},selectRecord:async id=>{c.selectedRecord=id;},ftnState:{section:'networks'},console});
vm.runInContext(source.slice(source.indexOf('const configHelp='),source.indexOf('const catalogRoute=value=>')),c);
vm.runInContext(source.slice(source.indexOf('const safeDraftFields='),source.indexOf('function transferBaseline(')),c);
const run=s=>vm.runInContext(s,c),help=run('configHelp'),safe=run('safeDraftFields');
const independent=['qwk-form','statistics-form','sound-editor','message-utilities','antivirus-section','offline-tools'];const parents=Object.fromEntries(independent.map(k=>[k,$(k).parentElement]));
function fields(keys){$('config-fields').replaceChildren();return keys.map((key,i)=>{const input=new Node('input');input.id='field-'+key;input.type=i%3===0?'checkbox':'text';input.value='9007199254740993';input.checked=true;const label=new Node('label');label.id=input.id+'-label';label.textContent=key;if(input.type==='checkbox')label.append(input);$('config-fields').append(label);if(input.type!=='checkbox')$('config-fields').append(input);const active=new Node('p');active.id=input.id+'-active';active.textContent='Currently active: preserved';$('config-fields').append(active);return {key,type:input.type==='checkbox'?'checkbox':'text',value:input.type==='checkbox'?true:input.value};});}
(async()=>{
 check(Object.keys(help).length===33,'exact33 primary help targets');
 const routesHeader=fs.readFileSync('src/apps/config_ui/routes.hpp','utf8').split('inline constexpr Route ftn_network_route')[0],shared=[...routesHeader.matchAll(/\{L"([a-z_]+\.[a-z_]+)", L"([^"]+)"/g)];
 check(shared.length===31&&new Set(shared.map(m=>m[2])).size===11,'source has31 shared pages in11 groups');
 assert.deepEqual(Object.keys(help).sort(),[...shared.map(m=>m[1]),'security.web_access','system.event_sounds'].sort());++checks;
 for(const route of Object.keys(help)){
  c.route=route;c.board={route,title:route,fields:fields(safe[route]||[]),activation:'Saved; applies at the next run',records:[],record:null};
  const original=c.board.fields.map(f=>({key:f.key,node:$('field-'+f.key),value:$('field-'+f.key).value,checked:$('field-'+f.key).checked}));
  run('configPageLayout();configGroupFields(board);configPresentationControls()');
  if(route==='doors.manager'){check($('config-page-guide').hidden&&$('config-editor-title').hidden,'qualified Doors keeps its own headings/layout');continue;}
  check($('config-page-help').href===help[route]&&!!$('config-page-purpose').textContent,'page has exact help and source-bound task explanation: '+route);
  for(const f of original){check($('field-'+f.key)===f.node&&f.node.value===f.value&&f.node.checked===f.checked,'grouping preserves control identity/value: '+route+'/'+f.key);check(f.node.parentElement!==null,'grouped control stays in DOM');}
  for(const k of independent)check($(k).parentElement===parents[k],'independent save scope never reparented: '+k);
 }
 c.route='users.accounts';c.board={title:'Accounts',fields:[],records:[{id:'9007199254740993',name:'First caller'},{id:'9007199254740995',name:'Second caller'}],record:'9007199254740993'};run('configRecordPresentation(board)');await $('config-record-list').children[1].onclick();check(c.selectedRecord==='9007199254740995','wide caller selection preserves64bit opaque string identity');check($('config-record-selected').textContent==='Selected: First caller','selected heading reflects saved record, not a queued click');
 for(const route of ['users.accounts','files.caller_exemptions'])for(const record of [null,0,'0','9007199254740993','9007199254740995']){c.route=route;c.board={route,title:'Caller settings',fields:fields(['name']),records:[{id:'9007199254740993',name:'First caller'}],record,can_save:false};run('configPresentationControls()');check($('board-form').hidden===(record===null),'unselected caller hides the common editor; new or unavailable exact selection remains visible: '+route+'/'+record);check($('config-editor-title').hidden===(record===null),'unselected caller has no misleading Editing heading');}
 c.route='messages.local_echoes';c.board={title:'Messages',fields:[],activation:'',records:[]};const list=$('catalog-list');list.replaceChildren();let oldAction=0;
 for(const [key,name] of [['group:9007199254740993','Group One'],['group:2','Group Two']]){const row=new Node('section');row.className='catalog-row';row.dataset.key=key;row.append(Object.assign(new Node('h4'),{textContent:name}));const b=new Node('button');b.textContent='Edit Group';b.onclick=()=>{++oldAction;};row.append(b);list.append(row);}
 run("configCatalogRows('messages',$('catalog-list'))");list.children[0].children[0].onclick();check($('config-messages-selected').textContent==='Selected: Group One','wide catalog publishes named selected context');$('config-messages-actions').children[0].onclick();check(oldAction===1,'selected action uses original feature-owned callback');c.mainPending={exact:true};run('configPresentationControls()');check($('config-messages-actions').children[0].disabled,'unknown existing operation locks selected actions');c.mainPending=null;
 c.route='ftn.networks';for(const section of ['networks','settings','links','echoes','files','routes','permissions','clone','import','setup','held']){c.ftnState={section,exactDraft:'unchanged'};const before=JSON.stringify(c.ftnState);run('configFtnPresentation()');check(JSON.stringify(c.ftnState)===before&&!!$('config-ftn-scope').textContent,'FTN presentation preserves '+section+' state and explicit save boundary');}
 c.ftnState={section:'links'};const slot=run("configFtnFieldSlot('address')");check(slot.parentElement.parentElement===$('ftn-fields'),'FTN field slot is inside the visible form');$('ftn-fields').replaceChildren();const next=run("configFtnFieldSlot('address')");check(next.parentElement.parentElement===$('ftn-fields')&&next.parentElement!==slot.parentElement,'a refreshed FTN form creates attached groups rather than appending into removed old groups');
 check(requests===0,'all33page/11FTN presentation operations issue zero API requests');
 console.log('PASS '+checks+' Linux SFXConfig33page/scoped-form/selection/FTN presentation checks');
})().catch(e=>{console.error(e);process.exitCode=1;});
