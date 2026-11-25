'use strict';
let cpState=null,cpSelection=null,cpViewing=null,cpRestartPending=null,cpPendingKey='',cpViewBusy=false;
function cpRememberRestart(){if(!draftKey)return;try{if(cpRestartPending)sessionStorage.setItem(draftKey+'-restart',JSON.stringify(cpRestartPending));else sessionStorage.removeItem(draftKey+'-restart');}catch{}}
function cpRefresh(state){
  cpState=state;
  if(cpPendingKey!==draftKey){cpPendingKey=draftKey;cpRestartPending=null;try{const value=JSON.parse(sessionStorage.getItem(draftKey+'-restart')||'null');if(value&&/^[0-9]+$/.test(value.run)&&/^[0-9]*$/.test(value.confirmation)&&/^[a-f0-9]{32}$/.test(value.epoch))cpRestartPending=value;}catch{}}
  const blocked=busy||maintenanceActive||!!cpRestartPending;
  $('cp-restart').disabled=blocked||!state.running||state.draining||state.recovery_required;
  $('cp-drain').disabled=blocked||!state.running||state.draining||state.recovery_required;
  $('cp-drain-time').disabled=$('cp-drain').disabled;
  $('cp-cancel-drain').disabled=blocked||!state.draining;
  $('cp-restart-pending').hidden=!cpRestartPending;
  $('cp-check-restart').disabled=$('cp-retry-restart').disabled=busy;
  if(cpRestartPending)$('start').disabled=$('stop').disabled=true;
  $('cp-lifecycle-detail').textContent=state.draining?'Graceful shutdown: refusing new callers. Stops when callers and scheduled work finish, or in '+Math.ceil(state.drain_remaining_ms/1000)+' seconds. Closing this browser does not cancel shutdown.':'';
  if(state.draining)$('bbs-state').textContent='Shutting down';
  cpNodeControls();
}
function cpSelect(node,state){if(!node||busy)return;cpSelection={...node,epoch:state.epoch};$('cp-node-result').textContent='';cpNodeControls();}
function cpCurrentNode(){return cpSelection&&cpState?.epoch===cpSelection.epoch?cpState.nodes.find(n=>n.number===cpSelection.number&&n.generation===cpSelection.generation):null;}
function cpNodeControls(){
  const current=cpCurrentNode(),blocked=busy||maintenanceActive||!cpState?.running||cpState?.recovery_required||!current;
  $('cp-selected').textContent=!cpSelection?'Select a node above.':!current?'Node '+cpSelection.number+' changed. Select it again to review its current caller.':'Node '+current.number+' — '+(current.connected?current.caller||'Logging in':current.enabled?'Waiting for caller':'Disabled');
  $('cp-view').disabled=blocked||!current?.connected;
  $('cp-enable').disabled=blocked||cpState?.draining||current?.enabled;
  $('cp-disable').disabled=blocked||cpState?.draining||!current?.enabled;
  $('cp-disconnect').disabled=blocked||!current?.connected;
  $('cp-terminate').disabled=blocked||!current?.connected||!current?.door_launch||current.door_launch==='0';
  cpCallerButtons();
}
function cpIdentity(node){return {node:node.number,epoch:node.epoch,generation:node.generation};}
async function cpNodeAction(action){
  const current=cpCurrentNode();if(!current||busy||maintenanceActive)return;
  const chosen={...current,epoch:cpSelection.epoch},name='Node '+chosen.number+(chosen.caller?' — '+chosen.caller:'');
  const titles={enable:'Enable this node?',disable:'Disable this node?',disconnect:'Disconnect this caller?',terminate:'Terminate this Door?'};
  const warnings={enable:'This saved setting allows new callers on this node.',disable:(chosen.connected?'The current caller will disconnect. ':'No caller is currently connected. ')+'The node stays disabled after the next BBS start.',disconnect:'Only this exact caller will disconnect.',terminate:'Only the selected Door launch will be asked to stop. The caller can return to the BBS.'};
  busy=true;cpNodeControls();
  try{if(!await offlineAsk(titles[action],name,warnings[action],action==='terminate'?'Terminate Door':action==='disconnect'?'Disconnect':action==='disable'?'Disable':'Enable'))return;
    const result=await api('/api/control/node',{...cpIdentity(chosen),action,door_launch:action==='terminate'?chosen.door_launch:'0'});$('cp-node-result').textContent=result.detail;
  }catch(error){$('cp-node-result').textContent=error.status?error.message:'The action result was not received. Refresh and review this node before choosing another action. No action was repeated.';}
  finally{busy=false;await refresh();}
}
function cpCloseView(){cpViewing=null;$('cp-view-screen').replaceChildren();$('cp-view-panel').hidden=true;}
async function cpRefreshView(){
  if(!cpViewing||cpViewBusy||!authenticated)return;cpViewBusy=true;const selected=cpViewing;
  try{const result=await api('/api/control/view',cpIdentity(selected));if(cpViewing!==selected)return;
    if(!result.active){cpViewing=null;$('cp-view-screen').replaceChildren();$('cp-view-detail').textContent=result.detail;return;}
    $('cp-view-title').textContent='Node '+selected.number+' — '+(result.caller||'Logging in');$('cp-view-screen').replaceChildren(...previewCells(result.cells));$('cp-view-detail').textContent=result.activity+' · '+result.seconds+' seconds connected';
  }catch(error){if(cpViewing===selected){$('cp-view-screen').replaceChildren();$('cp-view-detail').textContent='View unavailable. '+error.message;}}
  finally{cpViewBusy=false;}
}
async function cpView(){const node=cpCurrentNode();if(!node?.connected||busy)return;cpViewing={...node,epoch:cpSelection.epoch};$('cp-view-panel').hidden=false;await cpRefreshView();}
async function cpDrain(cancel=false){
  if(busy||!cpState?.running)return;const run=cpState.run,seconds=cancel?0:Number($('cp-drain-time').value);busy=true;cpRefresh(cpState);
  try{if(!cancel&&!await offlineAsk('Begin graceful shutdown?','Allow current callers up to '+seconds/60+' minute'+(seconds===60?'':'s')+' to finish.','New callers are refused. The BBS stops early when callers and scheduled work finish. At the deadline, remaining callers disconnect. Web controls stay open.','Begin Shutdown'))return;
    await api('/api/control/drain',{run,seconds});notice(cancel?'Shutdown cancelled. New callers can connect.':'Graceful shutdown started.');
  }catch(error){notice('Check the current BBS state before choosing another action. '+error.message);}
  finally{busy=false;await refresh();}
}
async function cpRestart(){
  if(busy||cpRestartPending||!cpState?.running)return;const request={run:cpState.run,epoch:cpState.epoch,confirmation:''};busy=true;cpRefresh(cpState);
  try{if(!await offlineAsk('Restart the BBS now?','Stop the BBS and start it with saved settings.','Any current callers require a separate confirmation. Web controls stay open.','Review Restart'))return;await cpSubmitRestart(request);}
  finally{busy=false;await refresh();}
}
async function cpSubmitRestart(request){
  cpRestartPending={...request};cpRememberRestart();
  let result;try{result=await api('/api/control/restart',{run:request.run,confirmation:request.confirmation});}
  catch(error){notice('Restart result needs checking. Use Check Restart Result; no new Restart was issued. '+error.message);return;}
  if(result.confirmation){
    const reviewed={...request,confirmation:result.confirmation};cpRestartPending=reviewed;cpRememberRestart();
    if(await offlineAsk('Disconnect callers and restart?',result.callers+' caller'+(result.callers===1?' is':'s are')+' currently online.','The current callers will disconnect. If callers change before you confirm, SFX asks you to review them again.','Restart BBS')){await cpSubmitRestart(reviewed);return;}
    cpRestartPending=null;cpRememberRestart();await api('/api/stop/cancel',{});return;
  }
  if(result.ok){cpRestartPending=null;cpRememberRestart();notice('BBS restarted.');}
}
async function cpCheckRestart(){
  if(!cpRestartPending||busy)return;busy=true;
  try{const state=await api('/api/status');if(state.epoch!==cpRestartPending.epoch){cpRestartPending=null;cpRememberRestart();notice('Management restarted. The earlier Restart result cannot be confirmed; review the current board.');}
    else if(BigInt(state.run)>BigInt(cpRestartPending.run)){cpRestartPending=null;cpRememberRestart();notice('The BBS has advanced to a new run. No further Restart was sent.');}
    else if(!state.running){cpRestartPending=null;cpRememberRestart();notice('The BBS is stopped. Restart did not reach a new running state. Review any recovery message before starting it.');}
    else notice('No completed Restart is visible. Review the current state. Retry Same Restart keeps the earlier run and confirmation; it cannot restart a later run.');cpRefresh(state);
  }catch(error){notice('Restart result is still unavailable. '+error.message);}finally{busy=false;await refresh();}
}
async function cpRetryRestart(){
  if(!cpRestartPending||busy)return;const retained={...cpRestartPending};busy=true;
  try{if(await offlineAsk('Retry this exact Restart?','Keep the original BBS run and caller confirmation.','A later BBS run cannot be restarted by this request. A changed caller set needs another review.','Retry Same Restart'))await cpSubmitRestart(retained);}
  finally{busy=false;await refresh();}
}
function doorCatalogRoute(value){return value==='doors.manager';}
let doorCatalogState={data:null,menu:0,action:null,entry:null,unconfirmed:null},doorCatalogRead=0;
const doorSafeKeys=['name','code','enabled','access_mode','access_level','access_sysop_only','execution_mode','execution_level','execution_sysop_only','command','startup_directory','cleanup_command','program_type','io','command_shell','drop_file','drop_location','doordrop_utf8','allow_multiple','automatic_event','event_only','authentication','caller_password','salt','banner','clear_password','clear_pepper','placement_menu','placement_number','placement_title','placement_description','number','title','description','destination','target','kind'];
function doorCatalogLocked(){return busy||saving||offlineBusy||offline?.active||maintenanceActive||mainPending||board?.readback_failed;}
function doorCatalogControls(){dbulControls();if(!doorCatalogRoute(route))return;for(const b of $('door-catalog-section').querySelectorAll('button'))b.disabled=!!doorCatalogLocked();doorPilotControls();}
function doorFieldVisibility(){
  if(!doorCatalogRoute(route)||doorCatalogState.action!=='door'||!board||doorCatalogState.create?.savedDoor)return;
  const command=$('field-command').value.trimStart(),rlogin=/^rlogin(?:\s|$)/.test(command),telnet=/^telnet(?:\s|$)/.test(command),local=!rlogin&&!telnet;
  const localKeys=['startup_directory','cleanup_command','program_type','io','command_shell','drop_file','drop_location','doordrop_utf8'];
  const remoteKeys=['authentication','caller_password','salt','banner','password','clear_password','pepper','clear_pepper'];
  for(const field of board.fields){const id='field-'+field.key,control=$(id);let visible=true;
    if(localKeys.includes(field.key))visible=local&&(field.key!=='doordrop_utf8'||$('field-drop_file').value==='14');
    if(remoteKeys.includes(field.key))visible=rlogin;
    for(const suffix of ['','-label','-help']){const part=$(id+suffix);if(part)part.hidden=!visible;}
    if(!visible)control.disabled=true;
  }
  if(rlogin){const usesCaller=$('field-caller_password').checked||/(?:^|\s)-(?:p|h)(?:\s|$)/.test(command),hash=$('field-authentication').value==='1'||/(?:^|\s)-h(?:\s|$)/.test(command);if(usesCaller)$('field-password').disabled=$('field-clear_password').disabled=true;if(!hash)$('field-pepper').disabled=$('field-clear_pepper').disabled=true;}
  const help=$('field-command-help');help.textContent=telnet?'Telnet: host[:port], optional -t terminal and -T seconds. No password options.':rlogin?'RLogin: host[:port], -q quiet, -P no pause, -p caller password, -h caller hash, -c client name, -s server name, -t terminal and -T seconds. Use the masked fields for saved credentials.':'Program command. Use %D for the drop-file folder. Program files are on the Linux computer; saving does not run the Door.';
}
function doorAdjustProgramFields(){
  if(!doorCatalogRoute(route)||doorCatalogState.action!=='door'||doorCatalogState.create?.savedDoor)return;
  const kind=Number($('field-program_type').value),io=Number($('field-io').value);
  if(kind!==doorCatalogState.programType){if(kind===1&&io!==0&&io!==4)$('field-io').value='0';else if(kind===0&&io===4)$('field-io').value='1';doorCatalogState.programType=kind;const selected=$('field-io').value,choices=kind===1?[[0,'Standard input/output'],[4,'Socket']]:[[0,'Standard'],[1,'FOSSIL or UART'],[2,'UART Only'],[3,'FOSSIL Only']];$('field-io').replaceChildren();for(const [value,label] of choices){const option=element('option',label);option.value=value;$('field-io').append(option);}$('field-io').value=selected;}
  const command=$('field-command').value.trimStart();if(!/^rlogin(?:\s|$)/.test(command)){doorClearSecrets();$('field-clear_password').checked=false;$('field-clear_pepper').checked=false;}
}
function doorClearSecrets(){if(!doorCatalogRoute(route))return;for(const key of ['password','pepper']){const control=$('field-'+key);if(control)control.value='';}}
function doorCatalogFields(entry,action){
  if(!entry)return [];const fields=[];
  const add=(key,label,type='text',maximum=64,help='',choices=null)=>{const field={key,label,type,value:entry[key]??'',maximum,minimum:0,help};if(choices)field.choices=choices.map((v,i)=>typeof v==='string'?{value:i,label:v}:v);fields.push(field);return field;};
  const access=(prefix,label)=>{add(prefix+'_mode',label+' mode','select',2,'',['Minimum','Maximum','Exact']).value=entry[prefix].mode;fields.at(-1).disabled_by=prefix+'_sysop_only';const level=add(prefix+'_level',label+' level','number',999);level.minimum=1;level.value=entry[prefix].level;level.disabled_by=prefix+'_sysop_only';add(prefix+'_sysop_only',label+': Sysops only','checkbox').value=entry[prefix].sysop_only;};
  if(action==='door'){
    add('name','Door name');add('code','Door code','text',16,'Blank chooses an unused code.');add('enabled','Enabled','checkbox');
    add('command','Command Line','text',4096,'Program command, rlogin host[:port] or telnet host[:port]. Use %D for the drop-file folder.');
    add('startup_directory','Startup Directory','text',1024);add('cleanup_command','Cleanup Command','text',4096);
    add('program_type','Program Type','select',1,'',['DOS','Native']);add('io','I/O Method','select',4,'',entry.program_type===1?[{value:0,label:'Standard input/output'},{value:4,label:'Socket'}]:[{value:0,label:'Standard'},{value:1,label:'FOSSIL or UART'},{value:2,label:'UART Only'},{value:3,label:'FOSSIL Only'}]);
    add('command_shell','Use command shell','checkbox');add('drop_file','Drop File Type','select',14,'',[
      {value:0,label:'None'},{value:1,label:'XTRN.DAT'},{value:2,label:'CHAIN.TXT'},{value:3,label:'DOOR.SYS (52 lines)'},{value:4,label:'DORINFO#.DEF'},{value:7,label:'SFDOORS.DAT'},{value:8,label:'DOOR.SYS (31 lines)'},{value:9,label:'DOORFILE.SR'},{value:10,label:'DORINFO1.DEF'},{value:12,label:'DOOR32.SYS'},{value:13,label:'BBSDEV.DRP'},{value:14,label:'DOORDROP.INI'},
      ...([5,6,11].includes(entry.drop_file)?[{value:entry.drop_file,label:'Saved legacy format ('+entry.drop_file+')'}]:[])]);
    add('drop_location','Drop File Location','select',2,'',['Node Directory','Startup Directory','Temporary Directory']);add('doordrop_utf8','DOORDROP.INI uses UTF-8','checkbox');
    add('allow_multiple','Allow multiple callers','checkbox');add('automatic_event','Automatic Event','select',2,'',['None','Logon','Logoff']);add('event_only','Automatic event only','checkbox');access('access','Access Requirements');access('execution','Execution Requirements');
    add('authentication','RLogin Authentication','select',2,'',['Traditional','Synchronet caller hash','Synchronet predefined hash']);add('caller_password','Use the authenticated caller password','checkbox');add('salt','Hash salt (blank uses QWK ID)','text',128);add('banner','Show RLogin banner','checkbox');
    add('password','New remote password','password',127,'Blank keeps the saved password. Never stored in a browser draft.');add('clear_password','Remove saved remote password','checkbox');add('pepper','New hash pepper','password',127,'Blank keeps the saved pepper. Never stored in a browser draft.');add('clear_pepper','Remove saved hash pepper','checkbox');
  }else if(action==='menu'){add('name','Category name');access('access','Access Requirements');access('execution','Execution Requirements');}
  else if(action==='entry'){
    const p=entry.placement;add('number','Menu number','number',99).value=p.number;fields.at(-1).minimum=1;add('title','Name on this menu').value=p.title;add('description','Description','text',256).value=p.description;
    add('destination','Destination category','select',999,'',(doorCatalogState.data?.menus||[]).map(m=>({value:m.id,label:m.name}))).value=p.menu;
    if(entry.create&&!entry.move){add('kind','Show a','select',1,'',['Door','Category']).value=p.kind;add('target','Shared item','select',999,'',(p.kind===0?doorCatalogState.data?.doors:doorCatalogState.data?.menus?.filter(m=>m.id!==0)||[]).map(d=>({value:d.id,label:d.name}))).value=p.target;}
  }else if(action==='duplicate_door'||action==='duplicate_menu')add('name','Copy name');
  return action==='door'?doorCreationFields(fields):fields;
}
function renderDoorCatalogEditor(entry,action,revision=doorCatalogState.data?.revision||'1',operation=doorCatalogState.data?.operation||''){
  $('door-saved-review').hidden=true;$('door-saved-review').textContent='';
  doorClearSecrets();doorCatalogState.entry=entry?structuredClone(entry):null;doorCatalogState.action=action;doorCatalogState.restoredDraft=false;doorCatalogState.programType=entry?.program_type;const fields=doorCatalogFields(entry,action);
  board={route,title:'Doors',category:'Doors',revision,operation,fields,active_fields:fields,can_save:!!entry,activation:action==='door'?(entry.id?'Shared settings apply everywhere this Door appears.':'New Doors are saved without a menu entry. Use Add Existing Door to place the saved Door.'):action==='menu'?'Category settings are shared. Use Show Category to place a saved category.':'Choose a category, menu entry or configured Door.'};renderForm(board);doorGroupFields();setDirty(false);doorCatalogControls();doorPilotControls();$('config-title').scrollIntoView?.({block:'start'});
}
function doorCandidate(){
  if(!doorCatalogState.entry||!board)return null;const entry=structuredClone(doorCatalogState.entry),values=formValues(),action=doorCatalogState.action;
  if(action==='door'||action==='menu'){
    for(const [key,value] of Object.entries(values))if(!key.startsWith('access_')&&!key.startsWith('execution_')&&!key.startsWith('placement_'))entry[key]=value;
    for(const prefix of ['access','execution'])if(Object.hasOwn(values,prefix+'_mode'))entry[prefix]={mode:values[prefix+'_mode'],level:values[prefix+'_level'],sysop_only:values[prefix+'_sysop_only']};
  }else if(action==='entry'){
    entry.placement={...entry.placement,menu:values.destination,number:values.number,title:values.title,description:values.description,...(Object.hasOwn(values,'target')?{target:values.target,kind:values.kind}:{})};
    if(!entry.create&&entry.placement.menu!==doorCatalogState.entry.placement.menu){entry.move=true;entry.create=true;entry.source_menu=doorCatalogState.entry.placement.menu;entry.previous_number=doorCatalogState.entry.placement.number;}
  }else entry.name=values.name;
  return {revision:board.revision,operation:board.operation,action,entry};
}
function doorCatalogDraft(){
  const candidate=doorCatalogState.unconfirmed||doorCandidate();if(!candidate)return null;const safe=structuredClone(candidate);delete safe.entry.password;delete safe.entry.pepper;return {menu:doorCatalogState.menu,action:safe.action,entry:safe.entry,operation:safe.operation,unconfirmed:!!board.readback_failed,creation:doorCatalogState.create?structuredClone(doorCatalogState.create):null};

}
function restoreDoorCatalogDraft(value){
  if(!value||typeof value!=='object'||!Number.isInteger(value.menu)||value.menu<0||value.menu>999||!['door','menu','entry','duplicate_door','duplicate_menu','remove_door','remove_menu','remove_entry'].includes(value.action)||!value.entry||typeof value.entry!=='object'||JSON.stringify(value.entry).length>24000||!/^[a-f0-9]{32}$/.test(value.operation))return false;
  const entry=structuredClone(value.entry);delete entry.password;delete entry.pepper;
  if(value.action==='door'){entry.password='';entry.pepper='';}
  doorCatalogState.create=value.creation&&typeof value.creation==='object'&&JSON.stringify(value.creation).length<=32000?structuredClone(value.creation):null;
  if(doorCatalogState.create?.pending?.request?.entry){delete doorCatalogState.create.pending.request.entry.password;delete doorCatalogState.create.pending.request.entry.pepper;}
  try{renderDoorCatalogEditor(entry,value.action,board.revision,value.operation);}catch{return false;}
  doorCatalogState.menu=value.menu;doorCatalogState.restoredDraft=true;if(value.unconfirmed){board.readback_failed=true;board.can_save=false;doorCatalogState.unconfirmed={revision:board.revision,operation:value.operation,action:value.action,entry};}$('config-result').textContent=value.unconfirmed?'A previous save is unconfirmed. Discard Changes reloads the saved catalog without repeating it.':'Nonsecret draft restored. Enter any new password or pepper again.';return true;
}
async function doorCatalogTransition(action){if(doorCatalogLocked())return;if(hasDraft()){pendingNavigation={catalogAction:action};$('dirty-page').textContent='Doors';$('dirty-discard').disabled=!!doorCatalogState.create?.pending;$('dirty-dialog').showModal();return;}await action();}
async function doorCatalogBrowse(menu=0,allowReadback=false){
  if(board?.readback_failed&&!allowReadback)return;const generation=++doorCatalogRead,requested=route,selection=doorTree.selection,current=()=>generation===doorCatalogRead&&requested===route&&doorCatalogRoute(route),fresh=new Map();
  const data=await api('/api/config/doors/catalog?menu='+menu);if(!current())return;fresh.set(menu,data);
  const read=async id=>{if(fresh.has(id))return fresh.get(id);if(!current())return null;const value=await api('/api/config/doors/catalog?menu='+id);if(!current())return null;if(value.revision!==data.revision)throw new Error('The Door catalog changed while reading. Refresh to review it.');fresh.set(id,value);return value;};
  const root=await read(0);if(!current())return;
  let unavailable=false;
  // Read only the selected appearance's captured ancestor/source-parent chain.
  for(const link of selection?.trail||[]){if(!data.menus.some(m=>m.id===link.menu)){unavailable=true;break;}const parent=await read(link.menu);if(!current())return;if(!parent?.entries.some(e=>e.number===link.number&&e.kind===1&&e.target===link.target)){unavailable=true;break;}}
  if(['entry','category'].includes(selection?.kind)&&!data.menus.some(m=>m.id===selection.menu))unavailable=true;
  if(selection?.kind==='entry'&&!unavailable){const source=await read(selection.menu);if(!current())return;unavailable=!source?.entries.some(e=>e.number===selection.number&&e.kind===0&&e.target===selection.target);}
  if(selection?.kind==='category'&&!unavailable){const source=await read(selection.menu);if(!current())return;unavailable=!source?.entries.some(e=>e.number===selection.number&&e.kind===1&&e.target===selection.id);}
  if(['menu','category'].includes(selection?.kind)&&!data.menus.some(m=>m.id===selection.id))unavailable=true;
  if(selection?.kind==='door'&&!data.doors.some(d=>d.id===selection.id))unavailable=true;
  if(!current())return;doorCatalogState.data=data;doorCatalogState.menu=menu;doorTree.unavailable=unavailable;
  for(const [id,cached] of doorTree.cache)if(cached.revision!==data.revision)doorTree.cache.delete(id);
  for(const [id,value] of fresh)doorTree.cache.set(id,value);if(!doorTree.selection)doorTree.selection={kind:'menu',id:menu};
  if(allowReadback&&board)board.readback_failed=false;renderDoorCatalogEditor(null,null,data.revision,data.operation);$('config-result').textContent='';doorRenderTree();
}
async function loadDoorCatalog(){doorCatalogState.unconfirmed=null;doorCatalogState.create=null;try{await doorCatalogBrowse(doorCatalogState.menu,true);}catch(error){if(doorCatalogState.menu&&error.status===400){await doorCatalogBrowse(0,true);return;}throw error;}}
async function doorCatalogEdit(id){doorCatalogState.create=null;const generation=++doorCatalogRead;const data=await api('/api/config/doors/door/'+id);if(generation!==doorCatalogRead||!doorCatalogRoute(route))return;renderDoorCatalogEditor(data.entry,'door',data.revision,data.operation);$('config-result').textContent='Saved remote password: '+(data.saved_password?'protected':'none')+'. Saved hash pepper: '+(data.saved_pepper?'protected':'none')+'.';if(!id)setDirty(true);}
function doorCatalogPlace(kind){const data=doorCatalogState.data,targets=kind===0?data.doors:data.menus.filter(m=>m.id!==0);if(!targets.length){notice('Create a '+(kind===0?'Door':'category')+' first.');return;}const used=new Set(data.entries.map(e=>e.number));let n=1;while(used.has(n))++n;if(n>99){notice('This menu already has 99 entries.');return;}renderDoorCatalogEditor({placement:{menu:data.menu,number:n,kind,target:targets[0].id,title:targets[0].name,description:''},previous_number:0,source_menu:0,create:true,move:false,order:[]},'entry');setDirty(true);}
async function saveDoorCandidate(candidate){
  doorCatalogState.unconfirmed=structuredClone(candidate);delete doorCatalogState.unconfirmed.entry.password;delete doorCatalogState.unconfirmed.entry.pepper;board.readback_failed=true;board.can_save=false;setDirty(true);fieldControls();
  try{const result=await api('/api/config/doors/change',candidate);doorClearSecrets();if(candidate.action==='remove_menu')doorCatalogState.menu=0;await doorCatalogBrowse(doorCatalogState.menu,true);doorCatalogState.unconfirmed=null;setDirty(false);$('config-result').textContent=result.detail;return true;}
  catch(error){if(error.response?.unresolved===false){doorCatalogState.unconfirmed=null;board.can_save=!!doorCatalogState.entry;board.readback_failed=false;}else{board.can_save=false;board.readback_failed=true;setDirty(true);}throw error;}
  finally{if(candidate?.entry){candidate.entry.password='';candidate.entry.pepper='';delete candidate.entry.password;delete candidate.entry.pepper;}doorClearSecrets();keepDraft();}
}
async function saveDoorCatalog(){if(doorCatalogState.create)return doorCreateSave();const candidate=doorCandidate();if(!candidate)throw new Error('Choose a Door, category or menu entry.');
  if(!await offlineAsk('Save Door Settings?',candidate.entry.name||candidate.entry.placement?.title||'Review the selected item.','Shared Door and category settings apply on every menu. Active Doors keep their captured settings. Program files are not changed.','Save'))return false;
  return saveDoorCandidate(candidate);
}
async function doorCatalogRemove(action,id,number){
  busy=true;lockBoard(true);doorCatalogControls();try{
    const preview=await api('/api/config/doors/removal-preview',{action,id,number});
    const data=doorCatalogState.data,target=action==='remove_menu'?data.menus.find(m=>m.id===id)?.name:action==='remove_door'?data.doors.find(d=>d.id===id)?.name:data.entries.find(e=>e.menu===id&&e.number===number)?.title;
    const count=(n,singular,plural)=>n+' '+(n===1?singular:plural);
    if(!await offlineAsk('Remove Door Settings?',(target?'Remove '+target+'? ':'')+count(preview.doors,'Door configuration','Door configurations')+', '+count(preview.categories,'category','categories')+' and '+count(preview.entries,'menu entry','menu entries')+' will be removed.',action==='remove_entry'?'The shared Door or category stays configured. Program files are kept.':'This includes shared placements elsewhere. Program files are kept. Active Doors keep their captured settings.','Remove'))return;
    const candidate={revision:preview.revision,operation:doorCatalogState.data.operation,action,entry:{id,number}};
    try{await saveDoorCandidate(candidate);if(action==='remove_menu')doorCatalogState.menu=0;await doorCatalogBrowse(doorCatalogState.menu,true);}
    catch(error){$('config-result').textContent=error.message+' Refresh Saved Settings before another action. This removal will not be repeated.';}
  }finally{busy=false;lockBoard(false);setDirty(dirty);doorCatalogControls();}
}
async function doorCatalogReorder(row,direction){const data=await api('/api/config/doors/catalog?menu='+row.menu),entries=data.entries,index=entries.findIndex(e=>e.number===row.number&&e.kind===row.kind&&e.target===row.target),other=index+direction;if(index<0)throw new Error('This menu entry changed. Refresh and select it again.');if(other<0||other>=entries.length)return;const order=entries.map(e=>e.number);[order[index],order[other]]=[order[other],order[index]];renderDoorCatalogEditor({placement:row,previous_number:row.number,source_menu:0,create:false,move:false,order},'entry',data.revision,data.operation);setDirty(true);await saveBoard();}

// Doors pilot: selected context is separate from the typed editor/write state.
const doorTree={cache:new Map(),expanded:new Set(['menu:0']),selection:null};
function doorContextMenu(){const s=doorTree.selection;return s?.kind==='entry'?s.menu:['menu','category'].includes(s?.kind)?s.id:doorCatalogState.menu;}
function doorMenuName(id){return doorCatalogState.data?.menus.find(m=>m.id===id)?.name||'Door menu';}
function doorPilotControls(){
  if(!doorCatalogRoute(route))return;
  const creating=doorCatalogState.create,editing=!!doorCatalogState.entry;
  $('board-form').hidden=!editing;
  $('door-editor-title').hidden=!editing;$('door-editor-title').textContent=editing?(creating?(creating.savedDoor?'Finish menu placement':'Add '+({native:'Linux program',telnet:'Telnet Door',rlogin:'RLogin Door'}[creating.type]||'Door')):doorCatalogState.action==='menu'?(doorCatalogState.entry.id===1000?'Add Category':'Edit Category — '+doorCatalogState.entry.name):'Edit '+(doorCatalogState.entry.name||doorCatalogState.entry.placement?.title||'menu entry')):'';
  $('door-check-result').hidden=!creating?.pending;
  $('door-check-result').disabled=busy||saving;
  if(editing){$('save-board').textContent=creating?'Save and add to '+doorMenuName(Number($('field-placement_menu')?.value??creating.menu)):'Save Changes';$('discard-board').textContent='Discard Changes';if(creating){board.activation=creating.savedDoor?'Door settings are saved. This editor finishes its menu placement.':'Save Door settings, then add it to the selected category. A saved Door is kept if placement needs correction.';$('discard-board').disabled=busy||saving||!!creating.pending;}}
  $('save-state').textContent=board?.readback_failed?'Save result needs checking':dirty?'Unsaved changes':editing?board.activation||'':'Choose an item, then Edit or Add Door.';
}
function doorTreeKey(s){const trail=(s.trail||[]).map(p=>p.menu+':'+p.number).join('/');return s.kind==='category'?'category:'+(trail||s.menu+':'+s.number):s.kind==='entry'?'entry:'+(trail?trail+'/':'')+s.menu+':'+s.number:s.kind+':'+s.id;}
function doorTreeSelect(s){return doorCatalogTransition(async()=>{
  doorCatalogState.create=null;doorTree.selection=s;doorTree.unavailable=false;
  if(['menu','category'].includes(s.kind)){doorTree.expanded.add(doorTreeKey(s));await doorCatalogBrowse(s.id);}
  else if(s.kind==='entry')await doorCatalogBrowse(s.menu);
  else{renderDoorCatalogEditor(null,null);doorRenderTree();}
  [...$('door-catalog-list').querySelectorAll('[role="treeitem"]')].find(b=>b.dataset.key===doorTreeKey(s))?.focus();
});}
function doorRenderTree(){
  const data=doorCatalogState.data;if(!data)return;
  const list=$('door-catalog-list');list.replaceChildren();list.setAttribute('role','tree');list.setAttribute('aria-label','Door categories and menu entries');
  const chosen=doorTree.selection||{kind:'menu',id:data.menu};doorTree.selection=chosen;
  const row=(parent,label,s,depth,parentKey,expand)=>{
    const b=element('button',label,'door-tree-row');b.type='button';b.dataset.key=doorTreeKey(s);b.dataset.parent=parentKey||'';b.setAttribute('role','treeitem');b.setAttribute('aria-level',String(depth));b.setAttribute('aria-selected',String(b.dataset.key===doorTreeKey(chosen)));b.tabIndex=b.dataset.key===doorTreeKey(chosen)?0:-1;
    b.style.paddingLeft=(18+(depth-1)*24)+'px';b.style.setProperty('--door-branch-left',(18+(depth-2)*24)+'px');if(expand!==undefined)b.setAttribute('aria-expanded',String(expand));
    b.onclick=()=>doorTreeSelect(s).catch(e=>notice(e.message));
    b.onkeydown=event=>{const buttons=[...list.querySelectorAll('[role="treeitem"]')],at=buttons.indexOf(b);let next;
      if(event.key==='ArrowDown')next=buttons[at+1];else if(event.key==='ArrowUp')next=buttons[at-1];else if(event.key==='Home')next=buttons[0];else if(event.key==='End')next=buttons.at(-1);
      else if(event.key==='ArrowRight'&&['menu','category'].includes(s.kind)){if(!doorTree.expanded.has(doorTreeKey(s))||!doorTree.cache.has(s.id)){event.preventDefault();b.click();return;}next=buttons[at+1]?.dataset.parent===b.dataset.key?buttons[at+1]:null;}
      else if(event.key==='ArrowLeft'){if(['menu','category'].includes(s.kind)&&doorTree.expanded.has(doorTreeKey(s))){event.preventDefault();doorTree.expanded.delete(doorTreeKey(s));doorRenderTree();[...list.querySelectorAll('[role="treeitem"]')].find(x=>x.dataset.key===b.dataset.key)?.focus();return;}next=buttons.find(x=>x.dataset.key===parentKey);}
      if(next){event.preventDefault();for(const x of buttons)x.tabIndex=-1;next.tabIndex=0;next.focus();}
    };parent.append(b);return b;
  };
  const tree=(menu,depth,path,parentKey,selection={kind:'menu',id:menu},trail=[])=>{
    const m=data.menus.find(m=>m.id===menu);if(!m)return;
    const key=doorTreeKey(selection),expanded=doorTree.expanded.has(key)&&doorTree.cache.has(menu),appearance=selection.kind==='category'?doorTree.cache.get(selection.menu)?.entries.find(e=>e.number===selection.number&&e.kind===1&&e.target===menu):null,label=appearance?appearance.number+'. '+appearance.title+(appearance.title!==m.name?' → '+m.name:''):m.name;row(list,(expanded?'▾ ':'▸ ')+label,selection,depth,parentKey,expanded);
    if(!expanded||path.has(menu))return;
    const content=doorTree.cache.get(menu);if(!content)return;
    for(const e of content.entries){if(e.kind===1){const next=[...trail,{menu,number:e.number,target:e.target}];tree(e.target,depth+1,new Set([...path,menu]),key,{kind:'category',id:e.target,menu,number:e.number,trail:next},next);}else{const d=data.doors.find(d=>d.id===e.target);row(list,e.number+'. '+e.title+(d&&!d.enabled?' — Disabled':''),{kind:'entry',menu,number:e.number,target:e.target,trail},depth+1,key);}}
  };tree(0,1,new Set(),'');
  if(data.menu!==0&&!doorTree.cache.has(0))tree(data.menu,1,new Set(),'');
  const all=$('door-configured-list');all.replaceChildren();
  for(const d of data.doors){const b=element('button',d.name+(d.enabled?'':' — Disabled'),'door-configured-row');b.type='button';b.onclick=()=>doorTreeSelect({kind:'door',id:d.id}).catch(e=>notice(e.message));all.append(b);}
  const categories=$('door-categories-list');categories.replaceChildren();for(const m of data.menus){const b=element('button',m.name,'door-configured-row');b.type='button';b.onclick=()=>doorTreeSelect({kind:'menu',id:m.id}).catch(e=>notice(e.message));categories.append(b);}
  const s=chosen,entry=['entry','category'].includes(s.kind)?doorTree.cache.get(s.menu)?.entries.find(e=>e.number===s.number):null,door=s.kind==='door'?data.doors.find(d=>d.id===s.id):entry?.kind===0?data.doors.find(d=>d.id===entry.target):null,menu=doorContextMenu();
  $('door-selected-kind').textContent=['menu','category'].includes(s.kind)?'Selected category:':'Selected Door:';
  $('door-selected-title').textContent=['menu','category'].includes(s.kind)?doorMenuName(s.id):entry?.title||door?.name||'Item unavailable';
  $('door-selected-detail').textContent=['menu','category'].includes(s.kind)?'Category · '+(doorTree.cache.get(s.id)?.entries.length??0)+' menu entries'+(entry?' · shown on '+doorMenuName(s.menu)+' as '+entry.number:''):entry?'Door on '+doorMenuName(s.menu)+' · caller choice '+entry.number+(door&&!door.enabled?' · Disabled':''):door?'Configured Door · may appear on more than one menu':'Refresh the catalog to review this item.';
  $('door-selected-detail').textContent='New Doors will be added to '+doorMenuName(menu)+'. '+$('door-selected-detail').textContent;
  const actions=$('door-selection-actions');actions.replaceChildren();const more=$('door-more-actions');more.replaceChildren();
  if(doorTree.unavailable){$('door-selected-kind').textContent='';$('door-selected-title').textContent='Selected item unavailable';$('door-selected-detail').textContent='This entry or its parent path changed. Select its current appearance in the tree before editing. No different item was selected automatically.';doorCatalogControls();doorPilotControls();return;}
  const action=(parent,label,fn)=>{const b=element('button',label);b.type='button';b.onclick=()=>doorCatalogTransition(fn).catch(e=>notice(e.message));parent.append(b);};
  action(actions,'Add Door',()=>doorAddOpen(menu));
  if(['menu','category'].includes(s.kind))action(actions,'Edit Category',()=>renderDoorCatalogEditor(data.menus.find(m=>m.id===s.id),'menu'));
  else if(door)action(actions,'Edit Door',()=>doorCatalogEdit(door.id));
  action(actions,'Add Category',()=>{renderDoorCatalogEditor({id:1000,name:'',access:{mode:0,level:10,sysop_only:false},execution:{mode:0,level:10,sysop_only:false}},'menu');setDirty(true);});
  action(more,'Add Unplaced / Event Door',()=>doorCatalogEdit(0));
  action(more,'Add Existing Door',()=>doorCatalogPlace(0));action(more,'Show Category',()=>doorCatalogPlace(1));
  if(['menu','category'].includes(s.kind)){action(more,'Duplicate Category',()=>{renderDoorCatalogEditor({id:s.id,name:''},'duplicate_menu');setDirty(true);});if(s.id)action(more,'Delete Category Everywhere…',()=>doorCatalogRemove('remove_menu',s.id,0));}
  if(door){action(more,'Duplicate Door',()=>{renderDoorCatalogEditor({id:door.id,name:''},'duplicate_door');setDirty(true);});action(more,'Delete Door Everywhere…',()=>doorCatalogRemove('remove_door',door.id,0));}
  if(entry){action(more,'Edit Menu Entry / Move',()=>renderDoorCatalogEditor({placement:entry,previous_number:entry.number,source_menu:0,create:false,move:false,order:[]},'entry'));
    action(more,'Show on Another Menu',()=>{renderDoorCatalogEditor({placement:entry,previous_number:0,source_menu:0,create:true,move:false,order:[]},'entry');setDirty(true);});
    action(more,'Remove from This Menu…',()=>doorCatalogRemove('remove_entry',s.menu,s.number));
    for(const d of [-1,1])action(more,d<0?'Move Up':'Move Down',()=>doorCatalogReorder(entry,d));}
  doorCatalogControls();doorPilotControls();
}
function doorAddOpen(menu=doorContextMenu()){
  if(doorCatalogLocked())return;doorCatalogState.addMenu=menu;doorCatalogState.addTrail=structuredClone(doorTree.selection?.trail||[]);
  $('door-add-destination').textContent='Add to '+doorMenuName(menu);$('door-add-dialog').showModal();
}
async function doorAddChoose(type){
  $('door-add-dialog').close();const menu=doorCatalogState.addMenu??doorContextMenu();
  if(type==='dos'){await dwOpen({menu});return;}
  const generation=++doorCatalogRead,data=await api('/api/config/doors/door/0');if(generation!==doorCatalogRead||!doorCatalogRoute(route))return;
  doorCatalogState.create={type,menu,contextMenu:menu,trail:structuredClone(doorCatalogState.addTrail||[]),savedDoor:0,pending:null};
  const entry=data.entry;Object.assign(entry,{program_type:1,io:0,command:type==='native'?'':type+' ',drop_file:0});
  renderDoorCatalogEditor(entry,'door',data.revision,data.operation);setDirty(true);
}
function doorCreationFields(fields){
  const c=doorCatalogState.create;if(!c)return fields;
  const used=doorTree.cache.get(c.menu)?.entries||[];let number=1;while(used.some(e=>e.number===number)&&number<=99)++number;
  const add=(key,label,type,value,choices,maximum=99)=>({key,label,type,value,choices,minimum:1,maximum,help:''});
  for(const field of fields)if(field.key==='program_type')field.readonly=true;
  return [...(c.savedDoor?[]:fields),
    add('placement_menu','Add to category','select',c.menu,doorCatalogState.data.menus.map(m=>({value:m.id,label:m.name})),999),
    add('placement_number','Caller menu number','number',c.number??(number<=99?number:'')),
    add('placement_title','Name on this menu (blank uses Door name)','text',c.title||'',null,64),
    add('placement_description','Menu description (optional)','text',c.description||'',null,256)];
}
function doorGroupFields(){
  if(!doorCatalogRoute(route)||doorCatalogState.action!=='door')return;
  const host=$('config-fields'),remaining=new Set((board?.fields||[]).map(f=>f.key));
  const group=(title,keys,parent=host)=>{
    const section=element('fieldset','','door-field-group');section.append(element('legend',title));let count=0;
    for(const key of keys){const label=$('field-'+key+'-label'),input=$('field-'+key);if(!remaining.has(key)||!label?.parentElement)continue;
      remaining.delete(key);const field=element('div','','door-field');field.append(label);if(input.type!=='checkbox')field.append(input);const help=$('field-'+key+'-help');if(help?.parentElement)field.append(help);section.append(field);++count;
    }if(count)parent.append(section);
  };
  group('Door',['name','enabled']);group('Program or remote connection',['command','startup_directory']);
  if(!doorCatalogState.create&&doorCatalogState.entry?.program_type===0)group('DOS caller connection',['program_type','io','drop_file','drop_location','doordrop_utf8']);
  group('Caller access',['access_mode','access_level','access_sysop_only','allow_multiple']);
  group('Menu placement',['placement_menu','placement_number','placement_title','placement_description']);
  if(remaining.size){const advanced=element('details','','door-advanced');advanced.append(element('summary','Advanced settings'));advanced.addEventListener('invalid',()=>{advanced.open=true;},true);group('Additional program, event and authentication settings',[...remaining],advanced);host.append(advanced);}
}
function doorCreationValues(){const v=formValues(),c=doorCatalogState.create;return {menu:Number(v.placement_menu),number:Number(v.placement_number),title:(v.placement_title||c.entry?.name||v.name||'').trim(),description:(v.placement_description||'').trim()};}
function doorCreateRememberPlacement(){if(!doorCatalogState.create)return;Object.assign(doorCatalogState.create,doorCreationValues());}
async function doorCreateSave(){
  const c=doorCatalogState.create;if(c.pending)throw new Error('Check the earlier save result before another save.');
  doorCreateRememberPlacement();let catalog=await api('/api/config/doors/catalog?menu='+c.menu);
  if(!Number.isInteger(c.number)||c.number<1||c.number>99||!c.title||catalog.entries.some(e=>e.number===c.number))throw new Error('Choose an unused menu number from 1 to 99 and a menu title.');
  if(!await offlineAsk(c.savedDoor?'Finish Door Placement?':'Save and Add Door?',c.title+' → '+doorMenuName(c.menu),'Door settings and menu placement save separately. A saved Door is kept if placement needs correction. Program files are not changed.','Save and add'))return false;
  if(!c.savedDoor){
    const request=doorCandidate();if(!request.entry.code)request.entry.code='NEW'+request.operation.slice(0,12).toUpperCase();
    c.entry=structuredClone(request.entry);delete c.entry.password;delete c.entry.pepper;
    c.pending={kind:'door',request:structuredClone(request),secretChange:!!request.entry.password||!!request.entry.pepper};delete c.pending.request.entry.password;delete c.pending.request.entry.pepper;keepDraft();
    try{const result=await api('/api/config/doors/change',request);if(!result.ok||!Number.isInteger(result.door_id)||result.door_id<1)throw new Error('The Door save was not confirmed.');c.savedDoor=result.door_id;c.pending=null;}
    catch(error){if(error.response?.unresolved===false)c.pending=null;else{board.readback_failed=true;board.can_save=false;}throw error;}
    finally{doorClearSecrets();}
  }
  try{
    catalog=await api('/api/config/doors/catalog?menu='+c.menu);
    if(catalog.entries.some(e=>e.number===c.number))throw Object.assign(new Error('That menu number is now used. Choose another number.'),{response:{unresolved:false}});
    const placement={menu:c.menu,number:c.number,kind:0,target:c.savedDoor,title:c.title,description:c.description};
    const request={revision:catalog.revision,operation:catalog.operation,action:'entry',entry:{placement,previous_number:0,source_menu:0,create:true,move:false,order:[]}};
    c.pending={kind:'entry',request};keepDraft();const result=await api('/api/config/doors/change',request);if(!result.ok)throw new Error('Placement was not confirmed.');c.pending=null;c.placementConfirmed=true;
    doorTree.selection={kind:'entry',menu:c.menu,number:c.number,target:c.savedDoor,trail:c.menu===c.contextMenu?c.trail:[]};doorCatalogState.create=null;await doorCatalogBrowse(c.menu,true);setDirty(false);$('config-result').textContent=c.title+' saved and added to '+doorMenuName(c.menu)+'.';return true;
  }catch(error){
    if(c.placementConfirmed){doorCatalogState.create=null;board.readback_failed=false;renderDoorCatalogEditor(null,null);setDirty(false);$('config-result').textContent='Door and menu placement saved. Refresh Catalog to update the list. '+error.message;return true;}
    if(!c.pending||error.response?.unresolved===false){c.pending=null;renderDoorCatalogEditor(c.entry,'door',catalog?.revision||board.revision,catalog?.operation||board.operation);setDirty(true);$('config-result').textContent='Door saved. Finish placing '+c.title+' in '+doorMenuName(c.menu)+'. '+error.message;return false;}
    board.readback_failed=true;board.can_save=false;throw error;
  }finally{doorPilotControls();keepDraft();}
}
async function doorCreateReview(){
  const c=doorCatalogState.create,p=c?.pending;if(!p||busy||saving)return;busy=true;doorPilotControls();
  try{
    if(p.kind==='door'){
      const catalog=await api('/api/config/doors/catalog?menu='+c.menu),wanted=p.request.entry,found=catalog.doors.find(d=>d.code===wanted.code&&d.name===wanted.name);
      if(!found)throw new Error('The Door save is unconfirmed. Review All Configured Doors; no save was repeated.');
      const saved=await api('/api/config/doors/door/'+found.id),keys=Object.keys(wanted).filter(k=>!['id','password','pepper','clear_password','clear_pepper'].includes(k));
      const access=r=>r?(r.sysop_only?'Sysops only':({0:'Minimum',1:'Maximum',2:'Exact'}[r.mode]||'Level')+' '+r.level):'Unavailable';
      $('door-saved-review').textContent=['Current saved settings; credential value not confirmed.','Door: '+saved.entry.name,'Code: '+saved.entry.code,'Command: '+saved.entry.command,'Caller access: '+access(saved.entry.access),'Execution access: '+access(saved.entry.execution),'Remote password present: '+(saved.saved_password?'Yes':'No'),'Hash pepper present: '+(saved.saved_pepper?'Yes':'No'),'Presence does not prove the requested secret. The tracked request stays locked unless its exact result can be confirmed.'].join('\n');$('door-saved-review').hidden=false;
      if(p.secretChange)throw new Error('This Door is visible, but the requested password or pepper cannot be read back. Its save remains unconfirmed; no secret or save was repeated.');
      if(keys.some(k=>JSON.stringify(saved.entry[k])!==JSON.stringify(wanted[k])))throw new Error('The saved Door differs from your request, including any remote command formatting. Its save remains unconfirmed; no save was repeated.');
      c.savedDoor=found.id;c.pending=null;board.readback_failed=false;renderDoorCatalogEditor(c.entry,'door',catalog.revision,catalog.operation);setDirty(true);$('config-result').textContent='Door save confirmed. Finish its menu placement.';
    }else{
      const wanted=p.request.entry.placement,catalog=await api('/api/config/doors/catalog?menu='+wanted.menu);
      if(!catalog.entries.some(e=>['menu','number','kind','target','title','description'].every(k=>e[k]===wanted[k])))throw new Error('Menu placement is unconfirmed. The saved Door is kept; no save was repeated.');
      c.pending=null;doorCatalogState.create=null;board.readback_failed=false;doorTree.selection={kind:'entry',menu:wanted.menu,number:wanted.number,target:wanted.target,trail:wanted.menu===c.contextMenu?c.trail:[]};await doorCatalogBrowse(wanted.menu,true);setDirty(false);$('config-result').textContent='Door and placement confirmed. No save was repeated.';
    }
  }catch(e){$('config-result').textContent=e.message;}finally{busy=false;doorPilotControls();doorCatalogControls();keepDraft();}
}

function doorPilotLayout(){
  const active=page==='config'&&doorCatalogRoute(route);$('config').classList.toggle('doors-pilot',active);$('workspace').classList.toggle('doors-workspace',active);
  const home=$('board-form-home');if(active)$('door-editor-anchor').append(home);else $('settings-form-anchor').append(home);
}
let doorLeave=null;
function doorLeaveAsk({detail,save,discard,keep}){
  doorLeave={save,discard,keep};$('door-leave-detail').textContent=detail;
  $('door-leave-save').disabled=!save;$('door-leave-discard').disabled=!discard;$('door-leave-keep').hidden=!keep;
  $('door-leave-dialog').showModal();
}
async function doorLeaveAnswer(kind){
  const request=doorLeave;if(!request)return;if(kind==='stay'){doorLeave=null;$('door-leave-dialog').close();return;}
  const action=request[kind];if(!action)return;
  for(const id of ['save','discard','stay','keep'])$('door-leave-'+id).disabled=true;
  try{if(await action()===false)return;doorLeave=null;$('door-leave-dialog').close();}
  finally{for(const id of ['save','discard','stay','keep'])$('door-leave-'+id).disabled=false;if(doorLeave){$('door-leave-save').disabled=!request.save;$('door-leave-discard').disabled=!request.discard;}}
}
const configHelp={"general.board_identity":"/help/sysop/get-ready.html#board-name","general.regional_time":"/help/sysop/get-ready.html#regional-time","general.displays_menus":"/help/sysop/customization-reference.html#display-settings","general.system_information":"/help/sysop/get-ready.html#opening-date","messages.local_echoes":"/help/sysop/messages-mail.html#groups-echoes","messages.clone_permissions":"/help/sysop/messages-mail.html#clone-options","files.groups_areas":"/help/sysop/files-transfers.html#groups-areas","files.transfer_policy":"/help/sysop/files-transfers.html#transfer-policy","files.count_rules":"/help/sysop/files-transfers.html#count-rules","files.caller_exemptions":"/help/sysop/files-transfers.html#caller-exemptions","doors.manager":"/help/sysop/doors.html#linux-doors-pilot","chat.rooms":"/help/sysop/callers-chat.html#chat-rooms","ftn.networks":"/help/sysop/ftn-networks.html#network","system.runtime_behavior":"/help/sysop/linux-controls.html#startup","system.nodes":"/help/sysop/get-ready.html#node-count","system.system_password":"/help/sysop/get-ready.html#system-password","system.event_sounds":"/help/sysop/daily-operation.html#event-sounds","users.accounts":"/help/sysop/callers-chat.html#accounts","users.security_levels":"/help/sysop/callers-chat.html#security-levels","users.new_user_registration":"/help/sysop/callers-chat.html#registration","users.caller_password_policy":"/help/sysop/callers-chat.html#caller-password-policy","connections.telnet_ssh":"/help/sysop/get-ready.html#listeners","connections.caller_idle_limits":"/help/sysop/callers-chat.html#idle-limits","connections.external_client":"/help/sysop/linux-controls.html#desktop","security.botgate":"/help/sysop/get-ready.html#bbs-security","security.connection_limits":"/help/sysop/get-ready.html#connection-limits","security.throttle_recovery":"/help/sysop/get-ready.html#failed-login-delays","security.clamav":"/help/sysop/files-transfers.html#clamav","security.web_access":"/help/sysop/linux-controls.html#lan","tools.backup_restore":"/help/sysop/maintenance-recovery.html#backups","tools.scheduled_jobs":"/help/sysop/maintenance-recovery.html#scheduled-jobs","tools.logging":"/help/sysop/daily-operation.html#logging","tools.bbs_health":"/help/sysop/maintenance-recovery.html#health"};
// SFXConfig presentation only. Typed API/state adapters remain feature-owned.
const configUI={selected:{messages:null,files:null},recordRoute:'',recordGeneration:0};
const configPageGroups={
 'general.board_identity':[['Board identity',['bbs_name']]],
 'general.regional_time':[['BBS time zone',['time_zone']]],
 'general.displays_menus':[['Text and prompts',['text_color','input_color']],['Selection and light bars',['lightbars','selected_text','selected_background']],['Caller menus',['compact_footer']],['Message display',['legacy_message_pipe_colors']]],
 'general.system_information':[['Board history',['opening_date']]],
 'messages.local_echoes':[['Name and availability',['name','enabled']],['Caller access',['access','reading','posting']],['Message display and retention',['pipe_colors','maximum_count','maximum_age']],['Advanced catalog settings',['order','group']]],
 'messages.clone_permissions':[['Fields to copy',['clone_access','clone_reading','clone_posting','clone_count','clone_age']]],
 'files.groups_areas':[['Name and availability',['name','enabled','description']],['Storage on the Linux BBS computer',['storage_path','backup_contents']],['Caller access',['access','upload','download','uploads_enabled','downloads_sysop_only']],['Uploads and descriptions',['own_description_edit','sysop_review','maximum_upload_bytes']],['Transfer counts',['ratio_uploads','ratio_downloads']],['Advanced catalog settings',['order','group']]],
 'files.transfer_policy':[['Uploads and file listings',['maximum_upload_bytes','lightbar_default']],['Daily download allowance',['daily_enabled','daily_files']],['Upload/download ratio',['ratio_enabled','ratio_downloads','ratio_uploads']]],
 'files.count_rules':[['Inheritance and exemptions',['inherit','daily_exempt','ratio_exempt']],['Daily download allowance',['daily_enabled','daily_files']],['Upload/download ratio',['ratio_enabled','ratio_downloads','ratio_uploads']]],
 'files.caller_exemptions':[['Exemptions for this caller',['daily_exempt','ratio_exempt']]],
 'chat.rooms':[['Room name and availability',['name','enabled']],['Caller access',['security_mode','security_level','sysop_only']]],
 'general.door_commands':[['Caller menu command',['menu','door','key','name','enabled']],['Caller access',['security_mode','security_level','sysop_only']]],
 'system.runtime_behavior':[['Linux hosting',['start_bbs_at_boot']]],
 'system.nodes':[['Caller nodes',['logical_nodes']]],
 'users.accounts':[['Caller identity',['name','real_name','location']],['Contact details',['phone','email','birth_date','sex']],['Access and retention',['security_level','purge_allowed']]],
 'users.security_levels':[['Security level',['level','label','description']],['BBS and chat time',['call_minutes','day_minutes','chat_minutes','chat_day_minutes']],['New-user and Sysop policy',['new_user_level','sysop_threshold']]],
 'users.new_user_registration':[['New callers',['allow_new_users']]],
 'users.caller_password_policy':[['Caller password rules',['strong_passwords']]],
 'connections.telnet_ssh':[['Telnet listener',['telnet_enabled','telnet_address','telnet_port']],['SSH listener',['ssh_enabled','ssh_address','ssh_port']],['Caller address display',['hostname_lookup']]],
 'connections.caller_idle_limits':[['Caller and login time limits',['caller_minutes','login_seconds']]],
 'connections.external_client':[['Terminal client on the Linux desktop',['type','executable','arguments']]],
 'security.botgate':[['Caller protection',['enabled']],['Login challenges',['telnet_challenge','new_ssh_challenge','challenge_seconds']]],
 'security.connection_limits':[['Connections from one address',['source_limit']],['Address exceptions',['source_exceptions']]],
 'security.throttle_recovery':[['Review a temporary login delay',['scope','subject']]],
 'security.clamav':[['Caller upload scan policy',['scan_uploads']]],
 'security.web_access':[['LAN management listener',['enabled','address','server_name','port']],['Allowed Sysop addresses',['approved']],['Certificate on the Linux computer',['custom_certificate','certificate_path','key_path','regenerate']],['Current LAN sessions',['end_sessions']]],
 'tools.backup_schedule':[['Schedule',['enabled','weekday','hour','minute']],['Backup destination and retention',['destination','retained']]],
 'tools.scheduled_jobs':[
  ['Command',['event_name','event_enabled','event_program','event_directory','event_arguments']],
  ['Schedule (BBS local time)',['event_months','event_dates','event_weekdays','event_timing','event_time']],
  ['Caller access',['event_exclusive','event_cutoff','event_wait']],
  ['Startup and alerts',['event_startup','event_missed','event_warning']]
 ],
 'tools.logging':[['Runtime log',['Runtime_size','Runtime_archives']],['Caller log',['Callers_size','Callers_archives']],['Attention log',['Attention_size','Attention_archives']]]
};
const configPurpose={
 'general.board_identity':'Set the BBS name here. QWK and message settings below have their own save buttons.',
 'general.regional_time':'Choose the time zone SFX uses for callers and scheduled work. This does not change the Linux system clock.',
 'general.displays_menus':'Set caller-screen colors and menu behavior. Preview is read-only; Door Commands is a separate editor.',
 'general.system_information':'Set the opening date. The statistics policy below saves separately.',
 'messages.local_echoes':'Browse named groups and local echoes. Choose an item, then use its actions below. Disable keeps messages.',
 'messages.clone_permissions':'Choose one source, explicit target echoes and the fields to copy. Review the clone before saving.',
 'files.groups_areas':'Browse File Groups and Areas. Paths are on the Linux BBS computer; folder settings do not move files.',
 'files.transfer_policy':'Set the board-wide upload and download rules. Per-level and per-caller rules remain separate.',
 'files.count_rules':'Choose a security level, then review its inherited limits and exemptions.',
 'files.caller_exemptions':'Find a caller, then edit only that caller’s daily and ratio exemptions.',
 'chat.rooms':'Choose or add a named chat room. Removing a room requires the normal empty-room review.',
 'ftn.networks':'Choose a network and its related settings. Apply to Draft and final Save FTN Settings are separate steps.',
 'system.runtime_behavior':'Choose the existing Linux hosting behavior. Saving a choice does not bypass host or ownership checks.',
 'system.nodes':'Choose the number of caller nodes. Read the saved/active status before restarting the BBS.',
 'system.system_password':'Replace the permanent System Password using this dedicated editor. Password text is never kept as a draft.',
 'system.event_sounds':'Assign WAV files from the Linux BBS computer. Preview, saving assignments and browser sound permission are separate.',
 'users.accounts':'Find a caller by name or number. Confirm the selected caller before account or password actions.',
 'users.security_levels':'Choose a level or the new-user/Sysop policy. Protected changes retain their existing review.',
 'users.new_user_registration':'Choose whether new callers may register. SSH new-user challenges remain under BBS Security.',
 'users.caller_password_policy':'Choose the rules for caller passwords. This does not rewrite existing passwords.',
 'connections.telnet_ssh':'Configure Telnet and SSH together. Saved listener settings and running listeners can differ until activation.',
 'connections.caller_idle_limits':'Set the existing caller and login limits; use the host’s activation note for when they apply.',
 'connections.external_client':'Configure the terminal client used on the Linux desktop, rather than the browser workstation.',
 'security.botgate':'Review BBS protection and the separate Telnet/new-SSH challenge choices.',
 'security.connection_limits':'Set per-source connection limits and exact address exceptions.',
 'security.throttle_recovery':'Review and clear one temporary login delay. This is an audited action, not a persistent policy save.',
 'security.clamav':'Save caller-upload scan policy separately from component status or definition-update work.',
 'security.web_access':'Control LAN HTTPS management access. Certificate or address changes can end this session; retain local-launch recovery.',
 'tools.backup_restore':'Use verified backup and recovery actions. Inspection, review, confirmation and preparation remain distinct.',
 'tools.backup_schedule':'Choose a built-in job. Save its schedule separately from running a backup or other maintenance.',
 'tools.scheduled_jobs':'Select an external event to edit its command, schedule and caller access.',
 'tools.logging':'Set rotation limits for the Runtime, Callers and Attention logs.',
 'tools.bbs_health':'Run a report while the BBS is stopped. This does not repair data, clear holds or prepare the board.'
};
function configPageLayout(){
 const active=page==='config'&&!doorCatalogRoute(route);$('config').classList.toggle('config-rollout',active);$('workspace').classList.toggle('config-workspace',active);
 $('config-page-guide').hidden=!active;if(!active){$('config-editor-title').hidden=true;$('config-save-scope').hidden=true;return;}
 $('config-page-purpose').textContent=configPurpose[route]||'Review the selected settings and their save scope.';
 const help=$('config-page-help');help.href=route==='general.door_commands'?'/help/sysop/doors.html#door-commands':configHelp[route]||'/help/sysop/wiki.html';
 const home=$('board-form-home');if(catalogRoute(route))$('config-message-editor').append(home);else if(fileCatalogRoute(route))$('config-file-editor').append(home);else $('settings-form-anchor').append(home);
}
function configGroupFields(current){
 if(doorCatalogRoute(current.route)||!current.fields?.length)return;
 const host=$('config-fields'),remaining=new Set(current.fields.map(f=>f.key)),groups=configPageGroups[current.route]||[['Settings',current.fields.map(f=>f.key)]];
 for(const [title,keys] of [...groups,['Additional settings',[...remaining]]]){
  const group=element('fieldset','','cfg-field-group');group.append(element('legend',title));let count=0;
  for(const key of keys){if(!remaining.has(key))continue;const label=$('field-'+key+'-label'),input=$('field-'+key);if(!label?.parentElement||!input)continue;remaining.delete(key);
   const field=element('div','','cfg-field');field.dataset.key=key;field.append(label);if(input.type!=='checkbox')field.append(input.closest('.event-calendar')||input);for(const suffix of ['-help','-active']){const part=$('field-'+key+suffix);if(part?.parentElement)field.append(part);}group.append(field);++count;
  }if(count)host.append(group);
 }
}
function configPresentationControls(){
 if(page!=='config'||doorCatalogRoute(route))return;
 const known=!!board,editing=catalogRoute(route)?!!catalogState.entry||catalogState.action==='clone':fileCatalogRoute(route)?!!fileCatalogState.entry:recordRoute(route)?board?.record!=null:true;
 if(catalogRoute(route)||fileCatalogRoute(route)||recordRoute(route))$('board-form').hidden=!editing;
 $('config-editor-title').textContent=editing&&board?.fields.length?'Editing '+(catalogRoute(route)?catalogState.entry?.name||'Clone Options':fileCatalogRoute(route)?fileCatalogState.entry?.name||'File settings':recordRoute(route)?board.records?.find(r=>String(r.id)===String(board.record))?.name||board.title:board?.title||'settings'):'';
 $('config-editor-title').hidden=!$('config-editor-title').textContent;
 $('config-save-scope').hidden=!known||!editing||!board.fields.length;
 $('config-save-scope').textContent=(throttleRoute(route)?'Clear Login Delay affects only the reviewed temporary delay; it does not save a policy.':'This editor saves separately. Other sections keep their own save buttons.')+' '+(board?.activation||'');
 if(!mainPending&&!throttleRoute(route)&&route!=='connections.external_client')$('save-board').textContent='Save Changes';
 else if(mainPending&&!systemPasswordRoute(route)&&!accountRoute(route)&&!securityRoute(route))$('save-board').textContent='Check Save Result';
 const locked=busy||saving||offlineBusy||!!mainPending||!!board?.readback_failed||!!offline?.active||maintenanceActive;
 for(const kind of ['messages','files'])for(const b of $('config-'+kind+'-actions').querySelectorAll('button'))b.disabled=locked;
 for(const b of $('config-record-list').querySelectorAll('button'))b.disabled=$('record-picker').disabled;
}
function configCatalogRows(kind,list){
 const selection=configUI.selected[kind],rows=[...list.querySelectorAll('.catalog-row')],saved=selection?.key;
 for(const item of rows){
  const title=item.querySelector('h4')?.textContent||'',buttons=[...item.querySelectorAll('button')],key=item.dataset.key;
  const pick=element('button',item.dataset.label??title,'cfg-browse-row');pick.type='button';pick.dataset.key=key;pick.setAttribute('aria-pressed',String(key===saved));
  pick.onclick=()=>{if(busy||saving||mainPending||board?.readback_failed||maintenanceActive)return;configUI.selected[kind]={key,title:pick.textContent,buttons};configCatalogSelection(kind,list);};
  item.replaceChildren(pick);item.cfgChoice={key,title:pick.textContent,buttons};
 }
 if(!rows.length)list.append(element('p','Nothing is listed on this page. Choose a group or another page, or use an available Add action.','cfg-empty field-help'));
 const current=rows.find(item=>item.cfgChoice.key===saved);configUI.selected[kind]=current?.cfgChoice||null;configCatalogSelection(kind,list);
}
function configCatalogSelection(kind,list){
 const selected=configUI.selected[kind];$('config-'+kind+'-selected').textContent=selected?'Selected: '+selected.title:'Select a named group or '+(kind==='messages'?'echo':'area');
 $('config-'+kind+'-actions').replaceChildren(...(selected?.buttons||[]));
 for(const b of list.querySelectorAll('.cfg-browse-row'))b.setAttribute('aria-pressed',String(b.dataset.key===selected?.key));configPresentationControls();
}
function configRecordPresentation(current){
 const list=$('config-record-list');list.replaceChildren();if(!recordRoute(route))return;
 const noun=route==='users.accounts'?'caller':route==='files.caller_exemptions'?'caller exemption':route==='tools.scheduled_jobs'?'job':route==='general.door_commands'?'menu command':route==='chat.rooms'?'chat room':'security level';
 $('config-record-heading').textContent='Choose a '+noun;
 for(const row of current.records||[]){const b=element('button',row.name,'cfg-browse-row');b.type='button';b.dataset.record=String(row.id);b.setAttribute('aria-pressed',String(String(row.id)===String(current.record)));b.onclick=()=>selectRecord(callerSearchRoute(route)?String(row.id):Number(row.id)).catch(e=>notice(e.message));list.append(b);}
 if(!current.records?.length)list.append(element('p','No saved records are listed. Use the available search or Add action.','cfg-empty field-help'));
 $('config-record-selected').textContent='Selected: '+(current.records?.find(r=>String(r.id)===String(current.record))?.name||(current.record===0||current.record==='0'?'New '+noun:'Choose a '+noun+' above'));
 $('record-picker').hidden=!!current.records?.length;$('record-label').hidden=$('record-picker').hidden;configPresentationControls();
}
function configFtnPresentation(){
 if(page!=='config'||route!=='ftn.networks')return;
 const section=ftnState.section;$('config-ftn-selected').textContent='Selected network / task: '+($('ftn-location').textContent||({setup:'Network setup',held:'Held mail'}[section])||section);
 $('ftn-form').classList.add('cfg-scoped-form');$('ftn-search-form').classList.add('cfg-search-form');
 for(const b of $('ftn-list').querySelectorAll('button'))b.classList.add('cfg-browse-row');
 $('config-ftn-scope').textContent=section==='held'?'Held-mail actions have their own reviews and receipts.':section==='setup'?'Follow the setup steps. Applied draft changes still need the final network save.':'Apply to Draft changes this task in the FTN draft. Save FTN Settings reviews and commits the complete draft.';
}
function configFtnFieldSlot(key){
 const section=ftnState.section,host=$('ftn-fields');let title='Settings';
 if(key.startsWith('setup_'))title='Network setup details';
 else if(section==='networks')title=['name','description','enabled','address','additional_addresses','origin_line'].includes(key)?'Network identity':['message_group','new_group_name','default_link'].includes(key)?'Local mapping':key.startsWith('echo_list')?'Echo list source':'NetMail, nodelist and retention';
 else if(section==='links')title=['name','enabled','use','address','additional_addresses'].includes(key)?'Connection identity':key.startsWith('tls')||key==='allow_legacy_plain_auth'?'Transport security':['contact_on_mail','poll_minutes'].includes(key)?'When to contact':key.includes('forward')||key==='areafix'?'Routing and AreaFix':'Remote connection';
 else if(section==='settings')title=['mode','external_executable'].includes(key)?'Mailer mode':key.startsWith('tls')?'TLS listener':'Incoming connections';
 else if(section==='echoes')title=['tag','description','store_locally','local_echo','local_name','update_local_name'].includes(key)?'Echo and local mapping':['access_level','read_level','post_level','update_local_permissions'].includes(key)?'Caller access':key.includes('maximum')||key==='update_local_retention'?'Message retention':'Names and message display';
 else if(section==='files')title='File echo and local area';else if(section==='routes')title='Routing';else if(section==='clone')title='Fields to copy';
 const groups=host.cfgGroups||(host.cfgGroups=new Map());let group=groups.get(title);
 if(!group||group.parentElement!==host){group=element('fieldset','','cfg-field-group');group.append(element('legend',title));host.append(group);groups.set(title,group);}
 const slot=element('div','','cfg-field');group.append(slot);return slot;
}

const catalogRoute=value=>value==='messages.local_echoes'||value==='messages.clone_permissions';
let catalogState={query:{group:null,revision:'0',after_order:'-1',after_id:'0'},data:null,action:'group',entry:null,source:null,targets:[]},catalogRead=0;
function catalogInteger(value,minimum=0n){try{return typeof value==='string'&&/^(0|[1-9][0-9]{0,18})$/.test(value)&&BigInt(value)>=minimum&&BigInt(value)<9223372036854775807n;}catch{return false;}}
function catalogEntry(value,kind){
  const keys=kind==='group'?['id','revision','name','order','access','enabled']:['id','revision','group','number','name','order','access','reading','posting','enabled','pipe_colors','maximum_count','maximum_age',...(kind==='insert'?['before','before_revision']:[])];
  if(!value||typeof value!=='object'||Object.keys(value).length!==keys.length||keys.some(k=>!Object.hasOwn(value,k))||!catalogInteger(value.id)||!catalogInteger(value.revision)||(value.id==='0')!==(value.revision==='0')||!catalogInteger(value.order)||typeof value.name!=='string'||value.name.length>64||typeof value.enabled!=='boolean')return null;
  for(const key of kind==='group'?['access']:['access','reading','posting'])if(!Number.isInteger(value[key])||value[key]<1||value[key]>999)return null;
  if(kind!=='group'){
    if(!catalogInteger(value.group,1n)||!Number.isInteger(value.number)||value.number<(value.id==='0'?0:1)||value.number>999||typeof value.pipe_colors!=='boolean')return null;
    for(const [key,max] of [['maximum_count',999999999],['maximum_age',65535]])if(value[key]!==null&&(!Number.isInteger(value[key])||value[key]<1||value[key]>max))return null;
    if(kind==='insert'&&(!catalogInteger(value.before,1n)||!catalogInteger(value.before_revision,1n)||value.id!=='0'))return null;
  }return Object.fromEntries(keys.map(k=>[k,value[k]]));
}
function catalogRequest(value){
  if(!value||typeof value!=='object'||Object.keys(value).length!==4||!catalogInteger(value.revision,1n)||typeof value.operation!=='string'||!/^[a-f0-9]{32}$/.test(value.operation)||!['group','echo','insert','up','down','clone'].includes(value.action))return null;
  let entry;if(['group','echo','insert'].includes(value.action))entry=catalogEntry(value.entry,value.action);
  else if(value.action==='up'||value.action==='down'){const row=value.entry;if(row&&Object.keys(row).length===2&&catalogInteger(row.id,1n)&&catalogInteger(row.revision,1n))entry={id:row.id,revision:row.revision};}
  else{const row=value.entry;if(!row||Object.keys(row).length!==5||!catalogInteger(row.source,1n)||!catalogInteger(row.source_revision,1n)||!Array.isArray(row.targets)||row.targets.length>999||!Number.isInteger(row.permission_mask)||row.permission_mask<0||row.permission_mask>7||!Number.isInteger(row.retention_mask)||row.retention_mask<0||row.retention_mask>3)return null;
    const ids=new Set();for(const target of row.targets){if(!target||Object.keys(target).length!==2||!catalogInteger(target.id,1n)||!catalogInteger(target.revision,1n)||target.id===row.source||ids.has(target.id))return null;ids.add(target.id);}entry={source:row.source,source_revision:row.source_revision,targets:row.targets.map(t=>({id:t.id,revision:t.revision})),permission_mask:row.permission_mask,retention_mask:row.retention_mask};}
  return entry?{revision:value.revision,operation:value.operation,action:value.action,entry}:null;
}
function catalogDraft(){return {action:catalogState.action,entry:catalogState.entry,query:catalogState.query,source:catalogState.source,targets:catalogState.targets,revision:board.revision,operation:board.operation,pending:mainPending?catalogRequest(mainPending):null};}
function restoreCatalogDraft(saved){
  if(!saved||!catalogInteger(saved.revision,1n)||typeof saved.operation!=='string'||!/^[a-f0-9]{32}$/.test(saved.operation)||!['group','echo','insert','clone','up','down'].includes(saved.action))return false;
  const action=saved.action,entry=['group','echo','insert'].includes(action)?catalogEntry(saved.entry,action):null;
  if(['group','echo','insert'].includes(action)&&!entry)return false;
  const source=saved.source?catalogEntry(saved.source,'echo'):null,targets=Array.isArray(saved.targets)&&saved.targets.length<=999?saved.targets.map(t=>catalogEntry(t,'echo')):[];
  if(targets.some(t=>!t)||new Set(targets.map(t=>t.id)).size!==targets.length)return false;
  const query=saved.query||{};if(query.group!==null&&!catalogInteger(query.group,1n))return false;
  catalogState={...catalogState,action,entry,source,targets,query:{group:query.group,revision:'0',after_order:'-1',after_id:'0'}};
  renderCatalogEditor(entry,action,saved.revision,saved.operation);const pending=catalogRequest(saved.pending);if(pending){mainPending=pending;board.revision=pending.revision;board.operation=pending.operation;}
  renderCloneSelection();return true;
}
function catalogControls(){
  if(!catalogRoute(route))return;const locked=busy||saving||offlineBusy||!!mainPending||!!board?.readback_failed||!!offline?.active||maintenanceActive,clone=route==='messages.clone_permissions',selected=!!catalogState.entry&&catalogState.entry.id!=='0'&&catalogState.action==='echo';
  for(const id of ['catalog-groups','catalog-new-group','catalog-new-echo','catalog-clone','catalog-clear-targets'])$(id).disabled=locked;
  $('catalog-next').disabled=locked||!catalogState.data?.next;$('catalog-new-echo').disabled=locked||!catalogState.query.group;
  for(const id of ['catalog-insert','catalog-up','catalog-down']){$(id).hidden=clone;$(id).disabled=locked||dirty||!selected;}
  $('catalog-delete').hidden=clone;$('catalog-delete').disabled=locked||dirty||bbsRunning||!catalogState.entry||catalogState.entry.id==='0'||!['group','echo'].includes(catalogState.action);
  $('catalog-clear-targets').hidden=!clone;$('catalog-new-group').hidden=$('catalog-new-echo').hidden=clone;$('catalog-clone').hidden=clone;
  for(const button of $('catalog-list').querySelectorAll('button'))button.disabled=locked;
  if(!catalogState.entry&&!clone)$('save-board').disabled=true;
}
function catalogFields(entry,action){
  if(action==='up'||action==='down')return [];
  if(action==='clone')return [['clone_access','Access'],['clone_reading','Reading'],['clone_posting','Posting'],['clone_count','Maximum Message Count'],['clone_age','Maximum Message Age']].map(([key,label])=>({key,label,type:'checkbox',value:false,help:'Only checked fields are copied.'}));
  if(!entry)return [];const fields=[];const add=(key,label,type,help='',minimum=1,maximum=999)=>fields.push({key,label,type,value:entry[key],help,minimum,maximum});
  add('name','Name','text','Use 1 to 64 printable characters, without leading or trailing spaces.',1,64);add('order','Display order','text','A whole number. IDs remain permanent when order changes.',0,19);add('access','Access minimum','number');
  if(action!=='group'){add('reading','Reading minimum','number');add('posting','Posting minimum','number');add('group','Parent group ID','text','Browse Groups and choose Move Here to select a different parent.',1,19);fields.at(-1).readonly=true;}
  add('enabled','Enabled','checkbox','Disable preserves all messages.');if(action!=='group'){add('pipe_colors','Renegade pipe colors','checkbox');add('maximum_count','Maximum message count','optional_number','Blank means Unlimited. Save itself removes no messages.',1,999999999);fields.at(-1).allow_empty=true;add('maximum_age','Maximum age in full days','optional_number','Blank means Unlimited. Applies at the next retention run.',1,65535);fields.at(-1).allow_empty=true;}return fields;
}
function renderCatalogEditor(entry,action,revision=catalogState.data?.revision||'1',operation=catalogState.data?.operation||''){
  if(board?.readback_failed)return;
  catalogState.entry=entry?structuredClone(entry):null;catalogState.action=action;const fields=catalogFields(entry,action);
  board={route,title:route==='messages.clone_permissions'?'Clone Options':'Groups & Local Echoes',category:'Messages',revision,operation,fields,active_fields:fields,activation:entry?'Review the selected stable area before saving.':action==='clone'?'Choose a source echo, explicit targets and fields to copy.':'Choose a group or echo to edit.',can_save:!!entry||action==='clone'};
  renderForm(board);setDirty(false);renderCloneSelection();
}
async function catalogTransition(action){if(busy||saving||offlineBusy||mainPending||board?.readback_failed||offline?.active||maintenanceActive)return;if(hasDraft()){pendingNavigation={catalogAction:action};$('dirty-page').textContent=board?.title||'Message Catalog';$('dirty-discard').disabled=false;$('dirty-dialog').showModal();return;}await action();}
function renderCloneSelection(){
  const pane=$('catalog-clone-selection');pane.replaceChildren();pane.hidden=route!=='messages.clone_permissions';if(pane.hidden)return;
  pane.append(element('h3','Source: '+(catalogState.source?.name||'None selected')),element('p',catalogState.targets.length+' explicit target echoes selected. Browse other groups or pages to add targets.'));
  for(const row of catalogState.targets){const line=element('p',row.name+' (#'+row.number+') '),remove=element('button','Remove Target');remove.type='button';remove.disabled=busy||!!mainPending||!!board?.readback_failed;remove.onclick=()=>{if(busy||mainPending||board?.readback_failed)return;catalogState.targets=catalogState.targets.filter(t=>t.id!==row.id);setDirty(true);renderCloneSelection();};line.append(remove);pane.append(line);}
}
async function catalogBrowse(query={group:null,revision:'0',after_order:'-1',after_id:'0'},refreshReadback=false){
  if(mainPending||(board?.readback_failed&&!refreshReadback))return;const generation=++catalogRead;const requested=route;const params=new URLSearchParams(Object.entries(query).filter(([,value])=>value!==null));
  const data=await api('/api/config/messages/catalog?'+params);if(generation!==catalogRead||route!==requested||!catalogRoute(route))return;
  catalogState.query={...query};catalogState.data=data;$('catalog-list').replaceChildren();$('catalog-page').textContent=(query.group?'Echoes in group #'+query.group:'Groups')+' — up to 64 entries per page.';
  const button=(parent,label,action)=>{const value=element('button',label);value.type='button';value.onclick=()=>{if(busy||saving||mainPending||board?.readback_failed)return;return Promise.resolve(action()).catch(error=>{$('catalog-result').textContent=error.message;});};parent.append(value);};
  const clone=route==='messages.clone_permissions';
  for(const row of data.groups){const item=element('section','','catalog-row');item.dataset.key='group:'+row.id;item.dataset.label=row.name+(row.enabled?'':' — Disabled');item.append(element('h4',row.name+' (#'+row.id+')'+(row.enabled?'':' — Disabled')));button(item,'Open Echoes',()=>catalogBrowse({group:row.id,revision:data.revision,after_order:'-1',after_id:'0'}));
    if(!clone){button(item,'Edit Group',()=>catalogTransition(()=>renderCatalogEditor(row,'group',data.revision,data.operation)));if(catalogState.entry&&catalogState.action!=='group')button(item,'Move Here',()=>{if(mainPending||busy)return;$('field-group').value=row.id;formChanged();});}$('catalog-list').append(item);}
  for(const row of data.echoes){const item=element('section','','catalog-row');item.dataset.key='echo:'+row.id;item.dataset.label='#'+row.number+' '+row.name+(row.enabled?'':' — Disabled');item.append(element('h4','#'+row.number+' '+row.name+(row.enabled?'':' — Disabled')));
    if(clone){button(item,'Use as Source',()=>{if(mainPending||busy)return;if(catalogState.source&&catalogState.source.id!==row.id&&catalogState.targets.length){$('catalog-result').textContent='Clear the current targets before choosing another source.';return;}catalogState.source=structuredClone(row);catalogState.targets=catalogState.targets.filter(t=>t.id!==row.id);board.revision=data.revision;board.operation=data.operation;setDirty(true);renderCloneSelection();});button(item,'Add Target',()=>{if(mainPending||busy||!catalogState.source||row.id===catalogState.source.id)return;if(data.revision!==board.revision){$('catalog-result').textContent='The catalog changed. Discard this clone draft and select the source and targets again.';return;}if(!catalogState.targets.some(t=>t.id===row.id)&&catalogState.targets.length<999)catalogState.targets.push(structuredClone(row));setDirty(true);renderCloneSelection();});}
    else button(item,'Edit Echo',()=>catalogTransition(()=>renderCatalogEditor(row,'echo',data.revision,data.operation)));$('catalog-list').append(item);}
  configCatalogRows('messages',$('catalog-list'));catalogControls();configPresentationControls();keepDraft();
}
async function loadMessageCatalog(){
  const generation=++boardLoad,requested=route;await catalogBrowse({group:catalogState.query.group,revision:'0',after_order:'-1',after_id:'0'},true);if(generation!==boardLoad||requested!==route||dirty||mainPending)return;
  if(board)board.readback_failed=false;
  catalogState.source=null;catalogState.targets=[];renderCatalogEditor(null,route==='messages.clone_permissions'?'clone':'group');$('config-result').textContent='';
}
function catalogCandidate(){
  const values=formValues();let entry,action=catalogState.action;
  if(action==='clone'){if(!catalogState.source)return null;entry={source:catalogState.source.id,source_revision:catalogState.source.revision,targets:catalogState.targets.map(t=>({id:t.id,revision:t.revision})),permission_mask:(values.clone_access?1:0)|(values.clone_reading?2:0)|(values.clone_posting?4:0),retention_mask:(values.clone_count?1:0)|(values.clone_age?2:0)};}
  else if(action==='up'||action==='down')entry=catalogState.entry;else entry={...catalogState.entry,...values};
  return catalogRequest({revision:board.revision,operation:board.operation,action,entry});
}
async function saveMessageCatalog(){
  if(!mainPending){const candidate=catalogCandidate();if(!candidate)throw new Error('Choose the exact group/echo and check all values.');
    if(candidate.action==='clone'&&(!candidate.entry.targets.length||!(candidate.entry.permission_mask||candidate.entry.retention_mask))){$('config-result').textContent='No targets or fields selected; nothing changed.';return false;}
    const lines=[];if(candidate.action==='clone'){
      const source=catalogState.source,fields=[['clone_access','access','Access'],['clone_reading','reading','Reading'],['clone_posting','posting','Posting'],['clone_count','maximum_count','Count'],['clone_age','maximum_age','Age']],values=formValues();
      lines.push('Source: '+source.name+'. Only checked fields are copied. No message bodies, Permanent flags, pipe colors or scan state change.');
      for(const target of catalogState.targets)lines.push(target.name+': '+fields.filter(([key])=>values[key]).map(([,key,label])=>label+' '+(target[key]??'Unlimited')+' -> '+(source[key]??'Unlimited')).join('; '));
    }else{lines.push('Save '+(candidate.entry.name||catalogState.entry?.name||'the selected echo')+'? Moving or disabling preserves messages. Retention limits apply at the next run; Save removes nothing.');for(const field of board.fields)lines.push(field.label+': '+(candidate.entry[field.key]??'Unlimited'));}
    $('message-utility-warning').textContent=lines.join('\n');if(!await new Promise(resolve=>{messageConfirmation=resolve;$('message-utility-dialog').showModal();}))return false;
    mainPending=candidate;keepDraft();fieldControls();catalogControls();
  }
  const result=await api('/api/config/messages/change',mainPending);mainPending=null;setDirty(false);catalogState.entry=null;catalogState.source=null;catalogState.targets=[];await loadAfterKnownSave(result.detail);return true;
}
const fileCatalogRoute=value=>value==='files.groups_areas';
let fileCatalogState={query:{group:null,revision:'0',after_id:'0'},data:null,entry:null,action:'group'},fileCatalogRead=0;
function fileCatalogEntry(value,action){
  const group=action==='group',insert=action==='insert_before'||action==='insert_after';
  const keys=group?['id','revision','name','order','access','enabled']:['id','revision','group','number','kind','name','description','order','access','upload','download','uploads_enabled','downloads_sysop_only','enabled','own_description_edit','backup_contents','ratio_uploads','ratio_downloads','sysop_review','maximum_upload_bytes','storage_path',...(insert?['anchor','anchor_revision']:[])];
  if(!value||typeof value!=='object'||Object.keys(value).length!==keys.length||keys.some(k=>!Object.hasOwn(value,k))||!catalogInteger(value.id)||!catalogInteger(value.revision,1n)||(!+value.id&&value.revision!=='1')||!catalogInteger(value.order)||typeof value.name!=='string'||value.name.length>64||typeof value.enabled!=='boolean')return null;
  for(const key of group?['access']:['access','upload','download'])if(!Number.isInteger(value[key])||value[key]<1||value[key]>999)return null;
  if(!group){
    if(!catalogInteger(value.group,1n)||!Number.isInteger(value.number)||value.number<0||value.number>999||!Number.isInteger(value.kind)||value.kind<0||value.kind>2||typeof value.description!=='string'||value.description.length>256||typeof value.storage_path!=='string'||value.storage_path.length>2048)return null;
    for(const key of ['uploads_enabled','downloads_sysop_only','own_description_edit','backup_contents','ratio_uploads','ratio_downloads','sysop_review'])if(typeof value[key]!=='boolean')return null;
    if(value.maximum_upload_bytes!==null&&(!Number.isInteger(value.maximum_upload_bytes)||value.maximum_upload_bytes<1||value.maximum_upload_bytes>1073741824))return null;
    if(insert&&(value.id!=='0'||!catalogInteger(value.anchor,1n)||!catalogInteger(value.anchor_revision,1n)))return null;
  }return Object.fromEntries(keys.map(k=>[k,value[k]]));
}
function fileCatalogRequest(value){
  if(!value||Object.keys(value).length!==4||!catalogInteger(value.revision,1n)||typeof value.operation!=='string'||!/^[a-f0-9]{32}$/.test(value.operation)||!['group','area','insert_before','insert_after','up','down'].includes(value.action))return null;
  const entry=fileCatalogEntry(value.entry,value.action);return entry?{revision:value.revision,operation:value.operation,action:value.action,entry}:null;
}
function fileCatalogDraft(){return {entry:fileCatalogState.entry,action:fileCatalogState.action,group:fileCatalogState.query.group,revision:board.revision,operation:board.operation,pending:mainPending?fileCatalogRequest(mainPending):null};}
function restoreFileCatalogDraft(value){
  if(!value||!catalogInteger(value.revision,1n)||typeof value.operation!=='string'||!/^[a-f0-9]{32}$/.test(value.operation)||!['group','area','insert_before','insert_after','up','down'].includes(value.action)||(value.group!==null&&!catalogInteger(value.group,1n)))return false;
  const entry=fileCatalogEntry(value.entry,value.action);if(!entry)return false;fileCatalogState.query={group:value.group,revision:'0',after_id:'0'};renderFileCatalogEditor(entry,value.action,value.revision,value.operation);mainPending=fileCatalogRequest(value.pending);return true;
}
function fileCatalogLocked(){return busy||saving||offlineBusy||!!mainPending||!!board?.readback_failed||!!offline?.active||maintenanceActive;}
function fileCatalogControls(){
  if(!fileCatalogRoute(route))return;const locked=fileCatalogLocked(),selected=fileCatalogState.entry?.id!=='0'&&fileCatalogState.action==='area';
  for(const id of ['file-catalog-groups','file-catalog-new-group'])$(id).disabled=locked;
  $('file-catalog-next').disabled=locked||!fileCatalogState.data?.next;$('file-catalog-new-area').disabled=locked||!fileCatalogState.query.group;
  for(const id of ['file-catalog-before','file-catalog-after','file-catalog-up','file-catalog-down'])$(id).disabled=locked||dirty||!selected;
  for(const button of $('file-catalog-list').querySelectorAll('button'))button.disabled=locked;
  $('file-catalog-delete').disabled=locked||dirty||bbsRunning||!fileCatalogState.entry||fileCatalogState.entry.id==='0'||fileCatalogState.action!=='group';
  if(!fileCatalogState.entry)$('save-board').disabled=true;
}
function fileCatalogFields(entry,action){
  if(!entry)return [];const fields=[];const add=(key,label,type,help='',minimum=1,maximum=999)=>fields.push({key,label,type,value:entry[key],help,minimum,maximum});
  add('name',action==='group'?'Group name':'Area name','text','Use 1 to 64 printable characters, without leading or trailing spaces.',1,64);add('order','Display order','text','A whole number. Area numbers are permanent.',0,19);add('access','Access minimum','number');
  add('enabled','Enabled','checkbox','Disable keeps the files and listings.');
  if(action!=='group'){
    add('group','Parent group ID','text','Browse Groups and choose Move Here.',1,19);fields.at(-1).readonly=true;
    add('description','Description','text','Up to 256 bytes of text.',0,256);fields.at(-1).allow_empty=true;
    add('storage_path','Existing storage folder','text','On the Linux computer, relative to this board or an absolute path. Save validates ownership and safety. It never creates or moves files.',1,2048);
    add('upload','Upload minimum','number');add('download','Download minimum','number');add('uploads_enabled','Allow uploads','checkbox');add('downloads_sysop_only','Downloads for Sysops only','checkbox');fields.at(-1).readonly=entry.kind!==0;
    add('own_description_edit','Allow uploader to edit descriptions','checkbox');add('backup_contents','Include contents in backup','checkbox');add('ratio_uploads','Count uploads for ratios','checkbox');add('ratio_downloads','Count downloads for ratios','checkbox');
    add('sysop_review','Review uploads before publication','checkbox');fields.at(-1).readonly=entry.kind!==0;
    add('maximum_upload_bytes','Maximum upload bytes','optional_number','Blank inherits the global maximum.',1,1073741824);fields.at(-1).allow_empty=true;
    if(action.startsWith('insert_')){fields.find(f=>f.key==='order').readonly=true;}
  }return fields;
}
function renderFileCatalogEditor(entry,action,revision=fileCatalogState.data?.revision||'1',operation=fileCatalogState.data?.operation||''){
  if(board?.readback_failed)return;fileCatalogState.entry=entry?structuredClone(entry):null;fileCatalogState.action=action;const fields=fileCatalogFields(entry,action);
  board={route,title:'Groups & Areas',category:'Files',revision,operation,fields,active_fields:fields,can_save:!!entry,activation:entry?(action==='group'?'Review the selected File Group.':'Area number '+entry.number+' stays permanent. Folder changes never move files; nonempty or pending areas cannot be redirected.'):'Choose a File Group or Area.'};renderForm(board);setDirty(false);
}
async function fileCatalogTransition(action){if(fileCatalogLocked())return;if(hasDraft()){pendingNavigation={catalogAction:action};$('dirty-page').textContent='File Groups & Areas';$('dirty-discard').disabled=false;$('dirty-dialog').showModal();return;}await action();}
async function fileCatalogBrowse(query={group:null,revision:'0',after_id:'0'},refreshReadback=false){
  if(mainPending||(board?.readback_failed&&!refreshReadback))return;const generation=++fileCatalogRead,requested=route;const data=await api('/api/config/files/catalog?'+new URLSearchParams(Object.entries(query).filter(([,v])=>v!==null)));
  if(generation!==fileCatalogRead||requested!==route||!fileCatalogRoute(route))return;fileCatalogState.query={...query};fileCatalogState.data=data;
  const list=$('file-catalog-list');list.replaceChildren();$('file-catalog-page').textContent=(query.group?'Areas in group #'+query.group:'File Groups')+' — up to 12 entries per page, by permanent ID. Display order is shown separately.';
  const button=(parent,label,action)=>{const b=element('button',label);b.type='button';b.onclick=()=>{if(fileCatalogLocked())return;return Promise.resolve(action()).catch(e=>notice(e.message));};parent.append(b);};
  for(const row of data.groups){const item=element('section','','catalog-row'),deleting=data.deleting_groups.includes(row.id);item.dataset.key='group:'+row.id;item.dataset.label=row.name+(row.enabled?'':' — Disabled')+(deleting?' — Deletion pending':'');item.append(element('h4',row.name+' (#'+row.id+'), order '+row.order+(row.enabled?'':' — Disabled')+(deleting?' — Deletion pending':'')));button(item,'Open Areas',()=>fileCatalogBrowse({group:row.id,revision:data.revision,after_id:'0'}));
    if(!deleting){button(item,'Edit Group',()=>fileCatalogTransition(()=>renderFileCatalogEditor(row,'group',data.revision,data.operation)));if(fileCatalogState.entry&&fileCatalogState.action==='area')button(item,'Move Here',()=>{$('field-group').value=row.id;formChanged();});}else button(item,'Review Saved Deletion',()=>fileCatalogTransition(()=>{renderFileCatalogEditor(row,'group',data.revision,data.operation);board.can_save=false;setDirty(false);}));list.append(item);}
  for(const row of data.areas){const item=element('section','','catalog-row');item.dataset.key='area:'+row.id;item.dataset.label='#'+row.number+' '+row.name+(row.enabled?'':' — Disabled');item.append(element('h4','#'+row.number+' '+row.name+', order '+row.order+(row.enabled?'':' — Disabled')));button(item,'Edit Area',()=>fileCatalogTransition(()=>renderFileCatalogEditor(row,'area',data.revision,data.operation)));list.append(item);}
  configCatalogRows('files',$('file-catalog-list'));fileCatalogControls();configPresentationControls();keepDraft();
}
async function loadFileCatalog(){const generation=++boardLoad,requested=route;await fileCatalogBrowse({group:fileCatalogState.query.group,revision:'0',after_id:'0'},true);if(generation!==boardLoad||requested!==route||dirty||mainPending)return;if(board)board.readback_failed=false;renderFileCatalogEditor(null,'group');$('config-result').textContent='';}
async function saveFileCatalog(){
  if(!mainPending){const candidate=fileCatalogRequest({revision:board.revision,operation:board.operation,action:fileCatalogState.action,entry:{...fileCatalogState.entry,...formValues()}});if(!candidate)throw new Error('Choose the exact Group or Area and check all values.');
    $('message-utility-warning').textContent='Save '+candidate.entry.name+'? Files are not moved or deleted. Folder changes must pass the existing-folder safety checks.\n'+board.fields.map(f=>f.label+': '+(candidate.entry[f.key]??'Inherit global maximum')).join('\n');
    if(!await new Promise(resolve=>{messageConfirmation=resolve;$('message-utility-dialog').showModal();}))return false;mainPending=candidate;keepDraft();fieldControls();fileCatalogControls();
  }
  const result=await api('/api/config/files/change',mainPending);mainPending=null;setDirty(false);fileCatalogState.entry=null;await loadAfterKnownSave(result.detail);return true;
}

const $=id=>document.getElementById(id);
let csrf='',authenticated=false,page='panel',board=null,dirty=false,busy=false,saving=false,pendingNavigation=null,confirmation='',boardLoad=0,route='general.board_identity';
let qwk=null,qwkDirty=false,qwkPending=null,qwkLoad=0;
let qwkConfirmation=null;
let statistics=null,statisticsDirty=false,statisticsLoad=0;
let recordId=null,deleteConfirmation=null,mainPending=null;
let draftKey='',restoringDraft=false;
let antivirus=null,avLoad=0,avBusy=false,avPolling=false;
let previewLoad=0,previewBusy=false;
const filePolicyRoute=value=>value==='files.transfer_policy'||value==='security.clamav';
let accountSearch={prefix:'',offset:0};
const countRecordRoute=value=>value==='files.count_rules'||value==='files.caller_exemptions';
const securityRoute=value=>value==='users.security_levels';
const accountRoute=value=>value==='users.accounts';
const callerSearchRoute=value=>value==='files.caller_exemptions'||accountRoute(value);
let accountConfirmation=null;
const systemPasswordRoute=value=>value==='system.system_password';
let systemPasswordConfirmation=null;
const throttleRoute=value=>value==='security.throttle_recovery';
let throttleUnconfirmed=false,throttleConfirmation=null;
const commandRoute=value=>value==='general.door_commands';
const jobsRoute=value=>value==='tools.backup_schedule';
const externalEventsRoute=value=>value==='tools.scheduled_jobs';
const offlineRoute=value=>value==='tools.bbs_health'||value==='tools.backup_restore';
let offline=null,offlineBusy=false,offlineReadOk=false,offlineRead=0,offlineUnknown=null,offlineDialog=null,bbsRunning=false;
let jobRunUnconfirmed=false,jobConfirmation=null;
let compaction=null,compactConfirmation=null,compactReviewConfirmation=null,compactUnknown=false,compactBefore="0",compactBusy=false,compactRead=0,compactReadOk=false,maintenanceActive=false;
const recordRoute=value=>jobsRoute(value)||externalEventsRoute(value)||commandRoute(value)||value==='chat.rooms'||countRecordRoute(value)||securityRoute(value)||accountRoute(value);
let securityConfirmation=null,securityRemoval=null,securityReviewLoad=0;
let messageUtilities={},messageLoad=0,messageConfirmation=null;
const safeDraftFields={
  'connections.external_client':['type','executable','arguments'],
  'system.runtime_behavior':['start_bbs_at_boot'],
  'doors.manager':doorSafeKeys,
  'security.web_access':['enabled','address','server_name','port','approved','custom_certificate','certificate_path','key_path','regenerate','end_sessions'],
  'files.groups_areas':['name','order','access','enabled','group','description','storage_path','upload','download','uploads_enabled','downloads_sysop_only','own_description_edit','backup_contents','ratio_uploads','ratio_downloads','sysop_review','maximum_upload_bytes'],
  'messages.local_echoes':['name','order','access','reading','posting','enabled','pipe_colors','maximum_count','maximum_age','group'],
  'messages.clone_permissions':['clone_access','clone_reading','clone_posting','clone_count','clone_age'],
  'tools.backup_schedule':['enabled','weekday','hour','minute','destination','retained'],
  'tools.scheduled_jobs':['event_name','event_enabled','event_program','event_directory','event_arguments',
    'event_months','event_dates','event_weekdays','event_timing','event_time','event_exclusive','event_cutoff',
    'event_wait','event_startup','event_missed','event_warning'],
  'general.door_commands':['menu','door','key','name','enabled','security_mode','security_level','sysop_only'],
  'system.system_password':[],
  'security.throttle_recovery':['scope','subject'],
  'users.accounts':['name','real_name','location','phone','email','birth_date','sex','security_level','purge_allowed'],
  'users.security_levels':['level','label','description','call_minutes','day_minutes','chat_minutes','chat_day_minutes','new_user_level','sysop_threshold'],
  'files.caller_exemptions':['daily_exempt','ratio_exempt'],
  'files.count_rules':['inherit','daily_exempt','ratio_exempt','daily_enabled','ratio_enabled','daily_files','ratio_downloads','ratio_uploads'],
  'security.clamav':['scan_uploads'],
  'files.transfer_policy':['maximum_upload_bytes','lightbar_default','daily_enabled','ratio_enabled','daily_files','ratio_downloads','ratio_uploads'],
  'chat.rooms':['name','enabled','security_mode','security_level','sysop_only'],
  'general.system_information':['opening_date'],'general.board_identity':['bbs_name'],'general.regional_time':['time_zone'],
  'general.displays_menus':['text_color','input_color','lightbars','selected_text','selected_background','compact_footer'],
  'system.nodes':['logical_nodes'],'users.new_user_registration':['allow_new_users'],'users.caller_password_policy':['strong_passwords'],
  'connections.telnet_ssh':['telnet_enabled','telnet_address','telnet_port','ssh_enabled','ssh_address','ssh_port','hostname_lookup'],
  'connections.caller_idle_limits':['caller_minutes','login_seconds'],'security.botgate':['enabled','telnet_challenge','new_ssh_challenge','challenge_seconds'],
  'security.connection_limits':['source_limit','source_exceptions'],'tools.logging':['Runtime_size','Runtime_archives','Callers_size','Callers_archives','Attention_size','Attention_archives']
};
function transferBaseline(value){
  const keys=['revision','catalog_revision','maximum_upload_bytes','lightbar_default','scan_uploads','daily_downloads','ratio_downloads','ratio_uploads'];
  if(!value||typeof value!=='object'||Object.keys(value).length!==keys.length||keys.some(key=>!Object.hasOwn(value,key)))return null;
  if(!/^[1-9][0-9]{0,18}$/.test(value.revision)||!/^[1-9][0-9]{0,18}$/.test(value.catalog_revision)||!Number.isInteger(value.maximum_upload_bytes)||value.maximum_upload_bytes<1||value.maximum_upload_bytes>1073741824||typeof value.lightbar_default!=='boolean'||typeof value.scan_uploads!=='boolean')return null;
  for(const key of ['daily_downloads','ratio_downloads','ratio_uploads'])if(value[key]!==null&&(!Number.isInteger(value[key])||value[key]<1||value[key]>1000000))return null;
  return Object.fromEntries(keys.map(key=>[key,value[key]]));
}
function transferRequest(value,requested=route){
  if(!value||typeof value!=='object'||Object.keys(value).length!==4||!/^[1-9][0-9]{0,18}$/.test(value.revision)||!/^[a-f0-9]{32}$/.test(value.operation))return null;
  const baseline=transferBaseline(value.baseline),values=value.values,keys=safeDraftFields[requested];
  if(!filePolicyRoute(requested))return null;
  if(!baseline||baseline.catalog_revision!==value.revision||!values||typeof values!=='object'||Object.keys(values).length!==keys.length||keys.some(key=>!Object.hasOwn(values,key)))return null;
  if(requested==='security.clamav'){if(typeof values.scan_uploads!=='boolean')return null;}else{
  for(const key of ['lightbar_default','daily_enabled','ratio_enabled'])if(typeof values[key]!=='boolean')return null;
  if(!Number.isInteger(values.maximum_upload_bytes)||values.maximum_upload_bytes<1||values.maximum_upload_bytes>1073741824)return null;
  for(const key of ['daily_files','ratio_downloads','ratio_uploads'])if(values[key]!==null&&(!Number.isInteger(values[key])||values[key]<1||values[key]>1000000))return null;
  }
  return {revision:value.revision,operation:value.operation,baseline,values:Object.fromEntries(keys.map(key=>[key,values[key]]))};
}
function ruleRequest(value,requested=route){
  if(!countRecordRoute(requested))return null;const exemption=requested==='files.caller_exemptions';
  const keys=['revision','operation','record','record_revision','values','kind'],fields=safeDraftFields[requested];
  if(!value||typeof value!=='object'||Object.keys(value).length!==keys.length||keys.some(key=>!Object.hasOwn(value,key))||value.kind!==(exemption?'caller_exemption':'security_rule')||!/^[1-9][0-9]{0,18}$/.test(value.revision)||!/^[1-9][0-9]{0,18}$/.test(value.record_revision)||!/^[a-f0-9]{32}$/.test(value.operation)||(exemption?typeof value.record!=='string'||!/^[1-9][0-9]{0,18}$/.test(value.record):!Number.isInteger(value.record)||value.record<1||value.record>999))return null;
  const values=value.values;if(!values||typeof values!=='object'||Object.keys(values).length!==fields.length||fields.some(key=>!Object.hasOwn(values,key)))return null;
  for(const key of (exemption?['daily_exempt','ratio_exempt']:['inherit','daily_exempt','ratio_exempt','daily_enabled','ratio_enabled']))if(typeof values[key]!=='boolean')return null;
  if(!exemption)for(const key of ['daily_files','ratio_downloads','ratio_uploads'])if(values[key]!==null&&(!Number.isInteger(values[key])||values[key]<1||values[key]>1000000))return null;
  return {revision:value.revision,operation:value.operation,record:value.record,record_revision:value.record_revision,kind:value.kind,values:Object.fromEntries(fields.map(key=>[key,values[key]]))};
}
function securityRequest(value){
  if(!value||typeof value!=='object'||Object.keys(value).length!==3||!/^[1-9][0-9]{0,18}$/.test(value.revision)||!Number.isInteger(value.record)||value.record<0||value.record>1000)return null;
  const keys=value.record===1000?['new_user_level','sysop_threshold']:['level','label','description','call_minutes','day_minutes','chat_minutes','chat_day_minutes'],values=value.values;
  if(!values||typeof values!=='object'||Object.keys(values).length!==keys.length||keys.some(key=>!Object.hasOwn(values,key)))return null;
  for(const key of keys){const item=values[key];if(key==='label'||key==='description'){if(typeof item!=='string'||item.length>(key==='label'?32:128))return null;}else if(!Number.isInteger(item)||item<(key.endsWith('minutes')?0:1)||item>(key.endsWith('minutes')?4294967295:999))return null;}
  return {revision:value.revision,record:value.record,values:Object.fromEntries(keys.map(key=>[key,values[key]]))};
}
function keepDraft(){
  if(!draftKey||restoringDraft)return;
  try{
    if(!hasDraft()){sessionStorage.removeItem(draftKey);if($('notice').textContent.startsWith('Your draft was restored in this tab.'))notice('');return;}
    const allowed=safeDraftFields[route];if(!allowed)return;
    const values={};if((dirty||mainPending)&&board)for(const field of board.fields){if(!allowed.includes(field.key)){if(doorCatalogRoute(route)&&field.type==='password')continue;return;}const input=$('field-'+field.key);values[field.key]=field.type==='checkbox'?input.checked:input.value;}
    const qwkDraft=route==='general.board_identity'&&qwk&&(qwkDirty||qwkPending)?{base:qwk,board_id:$('qwk-id').value,maximum_messages:$('qwk-maximum').value,pending:qwkPending}:null;
    sessionStorage.setItem(draftKey,JSON.stringify({version:1,route,record:recordId,botgate:botgateDraft(),messages:messageDrafts(),search:callerSearchRoute(route)?accountSearch:null,main:(dirty||mainPending)&&board?{revision:board.revision,values,...(route==='connections.external_client'?{desktop:{unconfirmed:board.desktop_unconfirmed===true,saved:board.desktop_saved===true}}:route==='system.runtime_behavior'?{hosting:{unconfirmed:board.hosting_unconfirmed===true}}:doorCatalogRoute(route)?{door_catalog:doorCatalogDraft()}:fileCatalogRoute(route)?{file_catalog:fileCatalogDraft()}:catalogRoute(route)?{catalog:catalogDraft()}:jobsRoute(route)?{job:{unconfirmed:jobRunUnconfirmed}}:throttleRoute(route)?{throttle:{unconfirmed:throttleUnconfirmed}}:filePolicyRoute(route)?{transfer:{operation:board.operation,baseline:transferBaseline(board.baseline),pending:mainPending?transferRequest(mainPending):null}}:countRecordRoute(route)?{rule:{operation:board.operation,record_revision:board.record_revision,pending:mainPending?ruleRequest(mainPending):null}}:systemPasswordRoute(route)?{system:{pending:systemPasswordPending(mainPending)}}:accountRoute(route)?{account:{policy_revision:board.policy_revision,pending:accountPending(mainPending)}}:securityRoute(route)?{security:{pending:mainPending?securityRequest(mainPending):null}}:{})}:null,qwk:qwkDraft,statistics:route==='general.system_information'&&statisticsDirty&&statistics?{revision:statistics.revision,exclude_sysop_statistics:$('exclude-sysop-statistics').checked}:null}));
  }catch{notice('This browser cannot keep a draft across reloads. Save your changes before closing this page.');}
}
function retainedDraft(){try{const raw=sessionStorage.getItem(draftKey);if(!raw||raw.length>1048576)return null;const value=JSON.parse(raw);return value.version===1&&Object.hasOwn(safeDraftFields,value.route)?value:null;}catch{return null;}}
function restoreDraft(value){
  if(value.route!==route||!board)return;
  if(doorCatalogRoute(route)&&!restoreDoorCatalogDraft(value.main?.door_catalog))return;
  if(fileCatalogRoute(route)&&!restoreFileCatalogDraft(value.main?.file_catalog))return;
  if(catalogRoute(route)&&!restoreCatalogDraft(value.main?.catalog))return;
  if(value.main&&/^[1-9][0-9]{0,19}$/.test(value.main.revision)&&value.main.values&&typeof value.main.values==='object'){
    for(const field of board.fields){const saved=value.main.values[field.key];if(!safeDraftFields[route].includes(field.key))continue;if(field.type==='checkbox'){if(typeof saved==='boolean')$('field-'+field.key).checked=saved;}else if(typeof saved==='string'&&saved.length<=8192)$('field-'+field.key).value=saved;}
    formChanged();if(dirty)board.revision=value.main.revision;if(doorCatalogRoute(route)&&value.main?.door_catalog?.unconfirmed){board.readback_failed=true;board.can_save=false;setDirty(true);}
  }
  if(route==='connections.external_client'&&(value.main?.desktop?.unconfirmed===true||value.main?.desktop?.saved===true)){board.desktop_unconfirmed=value.main.desktop.unconfirmed===true;board.desktop_saved=value.main.desktop.saved===true;board.readback_failed=true;board.can_save=false;setDirty(true);$('config-result').textContent=board.desktop_saved?'The client was saved, but reading it back failed. Discard Changes reads the current profile without saving again.':'The client save was not confirmed. Discard Changes reads its saved profile without repeating the save.';}
  if(route==='system.runtime_behavior'&&value.main?.hosting?.unconfirmed===true){board.hosting_unconfirmed=true;board.readback_failed=true;board.can_save=false;setDirty(true);$('config-result').textContent='The previous boot-setting save was not confirmed. Discard Changes reads the saved choice without repeating the save.';}
  if(filePolicyRoute(route)&&value.main?.transfer){const saved=value.main.transfer,baseline=transferBaseline(saved.baseline);
    if(baseline&&baseline.catalog_revision===value.main.revision&&/^[a-f0-9]{32}$/.test(saved.operation)){
      board.revision=value.main.revision;board.operation=saved.operation;board.baseline=baseline;
      const pending=transferRequest(saved.pending);if(pending&&pending.operation===saved.operation&&pending.revision===value.main.revision){mainPending=pending;for(const field of board.fields){const control=$('field-'+field.key),savedValue=pending.values[field.key];if(field.type==='checkbox')control.checked=savedValue;else control.value=savedValue??'';}formChanged();}
    }
  }
  if(countRecordRoute(route)&&value.main?.rule){const saved=value.main.rule;
    if(/^[1-9][0-9]{0,18}$/.test(value.main.revision)&&/^[1-9][0-9]{0,18}$/.test(saved.record_revision)&&/^[a-f0-9]{32}$/.test(saved.operation)){
      board.revision=value.main.revision;board.record_revision=saved.record_revision;board.operation=saved.operation;
      const pending=ruleRequest(saved.pending);if(pending&&pending.operation===saved.operation&&pending.revision===value.main.revision&&pending.record===recordId&&pending.record_revision===saved.record_revision){mainPending=pending;for(const field of board.fields){const control=$('field-'+field.key),savedValue=pending.values[field.key];if(field.type==='checkbox')control.checked=savedValue;else control.value=savedValue??'';}formChanged();}
    }
  }
  if(securityRoute(route)&&value.main?.security?.pending){const pending=securityRequest(value.main.security.pending);if(pending&&pending.record===recordId&&pending.revision===value.main.revision){mainPending=pending;board.revision=pending.revision;for(const field of board.fields)$('field-'+field.key).value=pending.values[field.key]??'';formChanged();}}
  if(accountRoute(route)&&value.main?.account){const saved=value.main.account;if(/^[1-9][0-9]{0,18}$/.test(saved.policy_revision))board.policy_revision=saved.policy_revision;const pending=accountPending(saved.pending);if(pending&&pending.base.record===recordId){mainPending=pending;setDirty(dirty);}}
  if(systemPasswordRoute(route)&&value.main?.system){const pending=systemPasswordPending(value.main.system.pending);if(pending){mainPending=pending;setDirty(false);}}
  if(jobsRoute(route)&&value.main?.job?.unconfirmed===true){jobRunUnconfirmed=true;board.can_save=false;setDirty(true);$('config-result').textContent='The previous Run Now outcome was not confirmed. Discard Changes reads current job status without repeating the run.';}
  if(throttleRoute(route)&&value.main?.throttle?.unconfirmed===true){throttleUnconfirmed=true;board.can_save=false;setDirty(true);$('config-result').textContent='The previous clear outcome was not confirmed. Check the caller and logs. Discard Changes reloads this page; no action was repeated.';}
  const saved=value.qwk;
  if(route==='general.board_identity'&&qwk&&saved&&typeof saved.board_id==='string'&&saved.board_id.length<=8&&typeof saved.maximum_messages==='string'&&saved.maximum_messages.length<=6&&saved.base&&/^[1-9][0-9]{0,18}$/.test(saved.base.revision)&&/^[a-f0-9]{32}$/.test(saved.base.operation)){
    $('qwk-id').value=saved.board_id;$('qwk-maximum').value=saved.maximum_messages;qwkChanged();
    if(qwkDirty||saved.pending)qwk={...qwk,revision:saved.base.revision,operation:saved.base.operation};
    if(saved.pending&&saved.pending.operation===saved.base.operation&&saved.pending.revision===saved.base.revision&&saved.pending.board_id===saved.board_id&&saved.pending.maximum_messages===Number(saved.maximum_messages))qwkPending=saved.pending;
    qwkControls();if(qwkPending)$('qwk-result').textContent='A QWK save needs reconciliation. Use Reconcile QWK to check the same operation.';
  }
  const stats=value.statistics;
  if(route==='general.system_information'&&statistics&&stats&&/^[1-9][0-9]{0,18}$/.test(stats.revision)&&typeof stats.exclude_sysop_statistics==='boolean'){
    $('exclude-sysop-statistics').checked=stats.exclude_sysop_statistics;statisticsChanged();if(statisticsDirty)statistics.revision=stats.revision;
  }
  restoreMessageDrafts(value.messages);
  botgateRestoreDraft(value.botgate);
  if(hasDraft())notice('Your draft was restored in this tab. Review it before saving; changed settings still require a fresh revision.');
}
function hasDraft(){return botgateHasDraft()||(ftnRoute(route)&&ftnPending())||dirty||qwkDirty||!!qwkPending||statisticsDirty||!!mainPending||Object.values(messageUtilities).some(item=>item.dirty||item.pending)||accountSecrets();}
function notice(text){$('notice').textContent=text;}
function lockWebAccess(){doorLeave=null;if($('door-leave-dialog').open)$('door-leave-dialog').close();if($('door-add-dialog').open)$('door-add-dialog').close();dwLock();dbulLock();setupClearSecrets();setupLoaded=false;soundClear();cpClearCallerViews();ftnClearPasswords();clearNodeTerminal();cpCloseView();cpSelection=null;doorClearSecrets();keepDraft();authenticated=false;$('workspace').hidden=true;$('locked').hidden=false;const lan=location.protocol==='https:';$('lan-login').hidden=!lan;$('local-launch-help').hidden=lan;$('lan-password').value='';$('connection').textContent='Not connected';}
async function api(path,body){
  const options={credentials:'same-origin',headers:{}};
  if(body!==undefined){options.method='POST';options.headers['Content-Type']='application/json';options.headers['X-SFX-CSRF']=csrf;options.body=JSON.stringify(body);}
  const response=await fetch(path,options);const result=await response.json();
  if(!response.ok){let message=result.detail||result.diagnostic||result.error||'';if(!message||/^[a-z0-9_]+$/.test(message))message=response.status===400?'Check the values on this page and try again.':response.status===409?'The settings changed or could not be applied. Reload the saved settings before trying again.':'The request could not be completed.';const error=new Error(message);error.status=response.status;error.response=result;throw error;}
  return result;
}
function element(tag,text,className){const node=document.createElement(tag);node.textContent=text;if(className)node.className=className;return node;}
function nodePresentation(data,running){return {logon:!!running&&data?.ready===true,label:data?.connected?data.caller||'Logging in':!running?'Offline':data?.enabled===false?'Disabled':data?.ready?'Waiting for caller':data?.activity||(data?.available===false?'Unavailable':'Not ready')};}
function showRecoveryNotice(state){if(state.recovery_required)notice('Recovery required: '+state.diagnostic+'. Open BBS Health Check or Backup & Restore.');else if($('notice').textContent.startsWith('Recovery required: '))notice('');}
async function refresh(){
  if(!authenticated)return;
  void ftnRenew();void ftnPollImport();
  try{const state=await api('/api/status');if(await setupRefresh(state))return;maintenanceActive=!!state.maintenance;$('config-tab').disabled=maintenanceActive;if(maintenanceActive){cpRefresh(state);cpCloseView();$('start').disabled=$('stop').disabled=true;$('bbs-state').textContent='Database maintenance';$('connection').textContent='Maintenance in progress';if(state.maintenance_kind==='offline')await refreshOffline();else await refreshCompaction();return;}bbsRunning=!!state.running;$('bbs-name').textContent=state.bbs_name||'Your BBS';$('bbs-state').textContent=state.recovery_required?'Recovery required':state.setup_required?'Setup required':state.running?'Running':'Stopped';$('bbs-state').classList.toggle('running',state.running);
    $('start').disabled=busy||state.running||state.setup_required||state.recovery_required;$('stop').disabled=busy||!state.running;$('caller-count').textContent=state.callers+' caller'+(state.callers===1?'':'s')+' online';
    cpRefresh(state);$('nodes').replaceChildren();for(let i=1;i<=state.configured_nodes;i++){const data=state.nodes.find(n=>n.number===i),presentation=nodePresentation(data,state.running);const row=element('div','','node'+(data?.connected?' connected':''));const select=element('button','Node '+i);select.disabled=busy;select.setAttribute('aria-pressed',String(cpSelection?.number===i));select.onclick=()=>cpSelect(data,state);row.append(select,element('span',presentation.label));const logon=element('button','Logon');logon.disabled=!presentation.logon||terminalBusy||!!terminalState;logon.onclick=()=>openNodeTerminal(i);row.append(logon);const lights=element('div','','modem-lights');for(const [key,label] of Object.entries({AA:'Accepting calls',CD:'Caller connected',RD:'Receiving data',SD:'Sending data',SC:'Secure connection',MR:'Node ready'})){const lamp=element('span',key,'modem-light'+(data?.modem?.[key]?' on':''));lamp.title=label;lamp.setAttribute('aria-label',label+': '+(data?.modem?.[key]?'on':'off'));lights.append(lamp);}row.append(lights);$('nodes').append(row);}
    $('activity').replaceChildren();const callers=state.nodes.filter(n=>n.connected);if(!callers.length)$('activity').append(element('p',state.running?'No callers are online.':'The BBS is stopped.','muted'));
    for(const caller of callers){const row=element('div','','activity-row');row.append(element('span','Node '+caller.number),element('span',caller.caller||'Logging in'),element('span',caller.activity));$('activity').append(row);}
    $('listeners').textContent=state.running?'Telnet '+state.telnet_port+(state.ssh_port?' · SSH '+state.ssh_port:''):'Management is available while the BBS is stopped.';
    if(!terminalBusy)await refreshNodeTerminal();
    await cpRefreshView();await cpRefreshCallers();await cpRefreshObservations();await cpRefreshBinkp();await cpRefreshMonitor();await fmCheck();await botgateRefreshFeed();
    $('checked').textContent='Updated '+new Date().toLocaleTimeString();$('connection').textContent=location.protocol==='https:'?'LAN controls':'Local controls';showRecoveryNotice(state);await refreshOffline();
  }catch(error){$('connection').textContent='Connection unavailable';if(error.status===401)lockWebAccess();notice(error.status===401?(location.protocol==='https:'?'Your session ended. Sign in with the System Password again.':'Your session ended. Open the Spitfire-X launcher again.'):error.message);}
}
function setDirty(value){dirty=value;$('clear-desktop-client').disabled=busy||saving||offlineBusy||maintenanceActive||board?.can_save===false||!!mainPending;doorCatalogControls();$('reload-board').disabled=busy||hasDraft();$('save-board').disabled=(!dirty&&!mainPending)||busy||(board?.can_save===false&&!mainPending);$('save-board').textContent=mainPending?(systemPasswordRoute(route)?'Review Password Status':accountRoute(route)?'Review Saved Account':securityRoute(route)?'Review Saved Policy':'Reconcile Save'):throttleRoute(route)?'Clear Login Delay...':route==='connections.external_client'?'Save Client':'Save';$('discard-board').disabled=!dirty||busy||!!mainPending;$('save-state').textContent=dirty?'Unsaved changes':board?.activation||'';statisticsControls();recordControls();fieldControls();jobControls();avControls();messageControls();accountControls();systemPasswordControls();throttleControls();jobControls();offlineControls();catalogControls();fileCatalogControls();doorFieldVisibility();doorPilotControls();configPresentationControls();botgateControls();keepDraft();}
function formValues(){const values={};for(const field of board?.fields||[]){const control=$('field-'+field.key);values[field.key]=field.type==='checkbox'?(field.disabled_by&&$('field-'+field.disabled_by).checked?false:control.checked):field.type==='optional_number'?((field.disabled_by&&$('field-'+field.disabled_by).checked)||(field.enabled_by&&!$('field-'+field.enabled_by).checked)||control.value===''?null:Number(control.value)):field.type==='number'||field.type==='select'?Number(control.value):control.value;}return values;}
function formChanged(){doorAdjustProgramFields();doorCreateRememberPlacement();const values=formValues();if(doorCatalogRoute(route)&&doorCatalogState.action==='entry'&&Object.hasOwn(values,'kind')&&values.kind!==doorCatalogState.entry.placement.kind){const entry=structuredClone(doorCatalogState.entry),targets=values.kind===0?doorCatalogState.data.doors:doorCatalogState.data.menus.filter(m=>m.id!==0);if(targets.length){entry.placement={...entry.placement,menu:values.destination,number:values.number,description:values.description,kind:values.kind,target:targets[0].id,title:targets[0].name};renderDoorCatalogEditor(entry,'entry');setDirty(true);return;}}setDirty(!!board&&(board.fields.some(field=>values[field.key]!==field.value)||(doorCatalogRoute(route)&&doorCatalogState.restoredDraft)));if(catalogRoute(route)&&route==='messages.clone_permissions'&&(catalogState.source||catalogState.targets.length))setDirty(true);if(route==='general.displays_menus')invalidateMenuPreview();}
const eventCalendarChoices={
  event_months:['Any month',['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec']],
  event_dates:['Any date',Array.from({length:31},(_,i)=>String(i+1))],
  event_weekdays:['All 7 days',['Sun','Mon','Tue','Wed','Thu','Fri','Sat']]
};
function eventCalendarControl(field,id){
  const [all,labels]=eventCalendarChoices[field.key];
  const wrapper=element('div','','event-calendar'),button=element('button'),choices=element('div','','event-calendar-choices');
  const error=element('p','','field-help');error.id=id+'-error';error.setAttribute('role','status');
  button.type='button';button.id=id;button.value=field.value;button.setAttribute('aria-labelledby',id+'-label '+id);
  button.setAttribute('aria-describedby',error.id);
  choices.id=id+'-choices';button.setAttribute('aria-controls',choices.id);
  const open=value=>{choices.hidden=!value;button.setAttribute('aria-expanded',String(value));};
  const inputs=['Any',...labels].map((name,index)=>{
    const box=document.createElement('input'),label=element('label',name,'checkbox-label');
    box.type='checkbox';label.prepend(box);choices.append(label);
    box.addEventListener('change',()=>{
      if(index===0)button.value=box.checked?all:'';
      else button.value=inputs.slice(1).flatMap((item,i)=>item.checked?[i+1]:[]).join(', ');
      button.dispatchEvent(new Event('input'));if(index===0){open(false);button.focus();}
    });
    return box;
  });
  button.updateCalendar=()=>{
    const any=button.value===all,selected=any?[]:button.value.split(',').map(Number).filter(i=>i>=1&&i<=labels.length);
    inputs.forEach((box,i)=>{box.checked=i===0?any:selected.includes(i);box.disabled=button.disabled;});
    button.textContent=(any?'Any':!selected.length?'Choose':selected.length>3?selected.length+' selected':selected.map(i=>labels[i-1]).join(', '))+' ▾';
    if(button.value){error.textContent='';button.removeAttribute('aria-invalid');}
    if(button.disabled)open(false);
  };
  button.requireCalendar=()=>{
    if(button.value)return true;
    button.setAttribute('aria-invalid','true');error.textContent='Choose '+field.label.toLowerCase()+' or Any.';
    button.focus();return false;
  };
  const done=element('button','Done');done.type='button';done.onclick=()=>{open(false);button.focus();};
  button.onclick=()=>open(choices.hidden);
  wrapper.onfocusout=event=>{if(!wrapper.contains(event.relatedTarget))open(false);};
  wrapper.onkeydown=event=>{if(event.key==='Escape'&&!choices.hidden){event.preventDefault();open(false);button.focus();}};
  choices.append(done);wrapper.append(button,error,choices);open(false);button.updateCalendar();return wrapper;
}
function renderForm(current){
  if(current.route==='general.displays_menus')invalidateMenuPreview();
  $('desktop-client-tools').hidden=current.route!=='connections.external_client';$('desktop-client-command').textContent=current.desktop_command||'Save the Linux profile here. Open the local launcher on the Linux desktop to view its command.';
  $('config-title').textContent=current.title;$('config-category').textContent=current.category;$('config-fields').replaceChildren();
  for(const field of current.fields){
    const id='field-'+field.key,label=element('label',field.label);label.htmlFor=id;label.id=id+'-label';
    if(current.route==='tools.scheduled_jobs'&&eventCalendarChoices[field.key]){
      const picker=eventCalendarControl(field,id);
      picker.firstChild.addEventListener('input',formChanged);
      $('config-fields').append(label,picker);
      continue;
    }
    const control=document.createElement(field.type==='select'?'select':field.type==='multiline'?'textarea':'input');control.id=id;control.name=field.key;
    if(field.type==='select'){for(const choice of field.choices){const option=element('option',choice.label);option.value=choice.value;control.append(option);}}
    else if(field.type==='checkbox')control.type='checkbox';else if(field.type==='number'||field.type==='optional_number'){control.type='number';control.min=field.minimum;control.max=field.maximum;control.step='1';control.required=field.type==='number';}
    else{if(field.type==='password')control.type='password';control.maxLength=field.maximum;control.autocomplete='off';if(field.type==='multiline')control.rows=5;}
    if(field.key==='time_zone'){control.setAttribute('list','bbs-time-zones');control.placeholder='Type a city or region';}if(field.type==='checkbox'){control.checked=field.value;label.className='checkbox-label';label.prepend(control);$('config-fields').append(label);}else{control.value=field.value??'';$('config-fields').append(label,control);}
    const help=element('p',field.help,'field-help');help.id=id+'-help';control.setAttribute('aria-describedby',help.id);if(field.help)$('config-fields').append(help);
    const active=current.active_fields.find(item=>item.key===field.key);if(active&&active.value!==field.value){const value=active.type==='optional_number'&&active.value===null?'Off':active.type==='checkbox'?(active.value?'Yes':'No'):active.type==='select'?(active.choices.find(choice=>choice.value===active.value)?.label||active.value):active.value;const activeValue=element('p','Currently active: '+value,'active-value');if(!doorCatalogRoute(current.route))activeValue.id=id+'-active';$('config-fields').append(activeValue);}
    control.disabled=!!field.readonly;control.addEventListener('input',formChanged);control.addEventListener('change',formChanged);
  }
  configGroupFields(current);configPageLayout();configPresentationControls();
}
async function loadBoard(){const generation=++boardLoad,requested=route;if(route==='system.event_sounds'){$('config-title').textContent='Event Sounds';$('config-category').textContent='System';$('desktop-client-tools').hidden=true;$('config-result').textContent='';await soundLoad();return;}if(!timeZonesLoaded)await loadTimeZones();if(generation!==boardLoad||page!=='config'||requested!==route)return;if(ftnRoute(route))return ftnWork(loadFtn);if(doorCatalogRoute(route))return loadDoorCatalog();if(fileCatalogRoute(route))return loadFileCatalog();if(catalogRoute(route))return loadMessageCatalog();const current=await api('/api/config/page/'+encodeURIComponent(requested)+(callerSearchRoute(requested)?'?prefix='+encodeURIComponent(accountSearch.prefix)+'&offset='+accountSearch.offset+(recordId!==null?'&record='+recordId:''):recordRoute(requested)&&recordId!==null?'?record='+recordId:''));if(dirty||mainPending||generation!==boardLoad||page!=='config'||requested!==route)return;board=current;if(recordRoute(requested))recordId=current.record;renderForm(current);renderRecords(current);renderJob(current);if(accountRoute(route))renderAccount();setDirty(false);$('config-result').textContent='';if(requested==='general.board_identity'&&!qwkDirty&&!qwkPending)await loadQwk();if(requested==='general.system_information'&&!statisticsDirty)await loadStatistics();if(requested==='security.clamav')await refreshAntivirus();if(requested==='security.botgate')await botgateLoad();await loadMessageUtilities();if(jobsRoute(requested))await refreshCompaction();if(offlineRoute(requested))await refreshOffline();}
const configNavOpen=new Map();
async function loadRoutes(){
  await loadTimeZones();const routes=await api('/api/config/routes'),list=$('config-routes'),current=routes.find(r=>r.id===(commandRoute(route)?'general.displays_menus':route))?.category;list.replaceChildren();let category='',group;
  for(const item of routes){if(category!==item.category){category=item.category;const key=category;group=element('details','','config-nav-group');group.open=configNavOpen.has(key)?configNavOpen.get(key):key===current;const disclosure=group;group.addEventListener('toggle',()=>configNavOpen.set(key,disclosure.open));const heading=element('summary',category);group.append(heading);list.append(group);}
    const button=element('button',item.title,'route');button.type='button';button.dataset.route=item.id;button.disabled=!item.available;button.classList.toggle('selected',route===item.id);if(route===item.id)button.setAttribute('aria-current','page');button.onclick=()=>navigate('config',item.id).catch(e=>notice(e.message));group.append(button);
  }
} 
async function navigate(target,nextRoute=route){if(soundSaving)return;if(soundOpen&&(target!=='config'||nextRoute!=='system.event_sounds')&&!await soundClose())return;if(ftnBusy||saving||(target===page&&(target!=='config'||nextRoute===route)))return;if(hasDraft()){pendingNavigation={page:target,route:nextRoute};$('dirty-page').textContent=board?.title||'this page';$('dirty-discard').disabled=!!qwkPending||!!mainPending||messageUnresolved()||!!doorCatalogState.create?.pending||!botgateCanDiscard();$('dirty-dialog').showModal();return;}await showPage(target,nextRoute);}
async function showPage(target,nextRoute=route,nextRecord=null,nextSearch=null){if(target!=='panel')soundDisable();ftnClearPasswords();$('ftn-section').hidden=target!=='config'||!ftnRoute(nextRoute);doorClearSecrets();$('door-catalog-section').hidden=target!=='config'||!doorCatalogRoute(nextRoute);$('file-catalog-section').hidden=target!=='config'||!fileCatalogRoute(nextRoute);$('catalog-section').hidden=target!=='config'||!catalogRoute(nextRoute);if(maintenanceActive){if(offline?.active)await refreshOffline();else await refreshCompaction();return;}$('compaction-section').hidden=target!=='config'||!jobsRoute(nextRoute);++compactRead;compaction=null;jobRunUnconfirmed=false;$('job-section').hidden=target!=='config'||!jobsRoute(nextRoute);$('door-command-back').hidden=target!=='config'||!commandRoute(nextRoute);++previewLoad;previewBusy=false;$('menu-preview-section').hidden=target!=='config'||nextRoute!=='general.displays_menus';$('preview-screen').replaceChildren();$('preview-samples').replaceChildren();$('preview-detail').textContent='';$('preview-folder').textContent='';$('preview-refresh').disabled=false;++messageLoad;messageUtilities={};$('message-utilities').replaceChildren();if(callerSearchRoute(nextRoute))accountSearch=nextSearch||(callerSearchRoute(route)?accountSearch:{prefix:'',offset:0});page=target;route=nextRoute;botgatePageChanged();doorPilotLayout();configPageLayout();recordId=nextRecord;throttleUnconfirmed=false;clearSystemPasswords();$('system-password-section').hidden=target!=='config'||!systemPasswordRoute(nextRoute);$('sound-editor').hidden=target!=='config'||nextRoute!=='system.event_sounds';$('board-form').hidden=nextRoute==='system.event_sounds'||ftnRoute(nextRoute)||systemPasswordRoute(nextRoute)||offlineRoute(nextRoute);$('offline-tools').hidden=target!=='config'||!offlineRoute(nextRoute);$('restore-tools').hidden=nextRoute!=='tools.backup_restore';$('health-tools').hidden=nextRoute!=='tools.bbs_health';++avLoad;antivirus=null;$('antivirus-section').hidden=target!=='config'||nextRoute!=='security.clamav';$('antivirus-status').textContent='Reading ClamAV status…';$('antivirus-result').textContent='';$('antivirus-log').hidden=true;avControls();mainPending=null;$('record-section').hidden=target!=='config'||!recordRoute(nextRoute);$('add-record').hidden=$('delete-record').hidden=nextRoute!=='chat.rooms'&&!securityRoute(nextRoute)&&!accountRoute(nextRoute)&&!commandRoute(nextRoute);$('add-record').textContent=commandRoute(nextRoute)?'Add Command':accountRoute(nextRoute)?'New User':securityRoute(nextRoute)?'Add Level':'Add Room';$('delete-record').textContent=commandRoute(nextRoute)?'Remove Command':accountRoute(nextRoute)?'Delete User':securityRoute(nextRoute)?'Remove Level':'Delete Room';$('record-label').textContent=jobsRoute(nextRoute)?'Built-in job':commandRoute(nextRoute)?'Menu command':(nextRoute==='files.count_rules'||securityRoute(nextRoute))?'Security level':callerSearchRoute(nextRoute)?'Caller':'Chat room';$('caller-search-section').hidden=!callerSearchRoute(nextRoute);$('caller-prefix-label').textContent=accountRoute(nextRoute)?'Name prefix or user number':'Caller name starts with';$('account-section').hidden=target!=='config'||!accountRoute(nextRoute);clearAccountSecrets();if(callerSearchRoute(nextRoute))$('caller-prefix').value=accountSearch.prefix;++boardLoad;++qwkLoad;++statisticsLoad;qwk=null;qwkDirty=false;qwkPending=null;statistics=null;statisticsDirty=false;$('statistics-section').hidden=target!=='config'||nextRoute!=='general.system_information';statisticsControls();$('qwk-section').hidden=target!=='config'||nextRoute!=='general.board_identity';qwkControls();$('panel').hidden=target!=='panel';$('config').hidden=target!=='config';$('control-tab').classList.toggle('selected',target==='panel');$('config-tab').classList.toggle('selected',target==='config');for(const button of $('config-routes').querySelectorAll('button')){const selected=button.dataset.route===(commandRoute(route)?'general.displays_menus':route);button.classList.toggle('selected',selected);if(selected){button.setAttribute('aria-current','page');button.parentElement.open=true;}else button.removeAttribute('aria-current');}if(target==='config'){board=null;$('config-fields').replaceChildren();$('config-title').textContent='Loading…';setDirty(false);await loadBoard();}}
function fieldControls(){for(const field of board?.fields||[]){const control=$('field-'+field.key),off=(field.disabled_by&&$('field-'+field.disabled_by).checked)||(field.enabled_by&&!$('field-'+field.enabled_by).checked);control.disabled=busy||saving||offlineBusy||!!offline?.active||maintenanceActive||!!mainPending||(board?.can_save===false)||!!field.readonly||!!off;if(field.type==='optional_number')control.required=!off&&!field.allow_empty;control.updateCalendar?.();}}
function lockBoard(value){saving=value;fieldControls();for(const id of ['dirty-save','dirty-discard','dirty-cancel','control-tab','config-tab'])$(id).disabled=value;$('dirty-discard').disabled=value||!!qwkPending||!!mainPending||messageUnresolved()||!!doorCatalogState.create?.pending;qwkControls();statisticsControls();recordControls();avControls();messageControls();accountControls();systemPasswordControls();throttleControls();botgateControls();}
async function saveBoard(){if(!board||busy||offlineBusy||offline?.active||maintenanceActive||(!mainPending&&(board.can_save===false||!$('board-form').reportValidity())))return false;
  if(externalEventsRoute(route)&&Object.keys(eventCalendarChoices).some(key=>!$('field-'+key).requireCalendar()))return false;
  busy=true;lockBoard(true);++boardLoad;setDirty(dirty);try{
    if(doorCatalogRoute(route))return await saveDoorCatalog();
    if(fileCatalogRoute(route))return await saveFileCatalog();
    if(catalogRoute(route))return await saveMessageCatalog();
    if(throttleRoute(route))return await clearLoginDelay();
    if(systemPasswordRoute(route))return await saveSystemPassword();
    if(accountRoute(route))return await saveAccount();
    let candidate={revision:board.revision,values:formValues(),...(recordRoute(route)?{record:recordId}:{})};
    if(route==='connections.external_client'){
      board.desktop_unconfirmed=true;board.desktop_saved=false;keepDraft();let result;
      try{result=await api('/api/config/page/connections.external_client',candidate);}
      catch(error){if(error.response?.unresolved===false){board.desktop_unconfirmed=false;keepDraft();}else{board.can_save=false;board.readback_failed=true;setDirty(true);}throw new Error(error.message+' No automatic retry will occur.');}
      board.desktop_unconfirmed=false;board.desktop_saved=true;keepDraft();
      try{const current=await api('/api/config/page/connections.external_client');board=current;renderForm(current);setDirty(false);$('config-result').textContent=result.detail;return true;}
      catch(error){board.can_save=false;board.readback_failed=true;setDirty(true);throw new Error('Client saved, but reading it back failed. Discard Changes reads the current profile without saving again.');}
    }
    if(route==='system.runtime_behavior'){
      board.hosting_unconfirmed=true;keepDraft();
      try{const result=await api('/api/config/page/system.runtime_behavior',candidate),current=await api('/api/config/page/system.runtime_behavior');board=current;renderForm(current);setDirty(false);$('config-result').textContent=result.detail;return true;}
      catch(error){board.can_save=false;board.readback_failed=true;setDirty(true);throw new Error(error.message+' Use Discard Changes to read the saved boot choice. This save will not be repeated.');}
    }
    if(route==='security.web_access'){
      if(!await offlineAsk('Save Web Access?',candidate.values.enabled?'Allow only the listed IP addresses to use HTTPS controls with the System Password.':'Turn LAN controls off. The local launcher and BBS callers remain available.',candidate.values.regenerate?'This replaces the generated certificate. Verify its new fingerprint before trusting it.':candidate.values.certificate_path?'This imports a certificate and private key from the selected Linux files. Check the resulting fingerprint.':'Address changes may end your LAN session. Use the local launcher to recover access.','Save Web Access'))return false;
      try{const result=await api('/api/config/page/security.web_access',candidate);setDirty(false);await loadAfterKnownSave(result.detail||'Saved.');await refresh();return true;}
      catch(error){board.can_save=false;board.readback_failed=true;throw new Error(error.message+' Use Discard Changes to reload the saved settings and certificate fingerprint before another Save. This request will not be repeated.');}
    }
    if(filePolicyRoute(route)){
      if(!mainPending){candidate={...candidate,operation:board.operation,baseline:board.baseline};mainPending=transferRequest(candidate);if(!mainPending)throw new Error('Check the values on this page.');keepDraft();fieldControls();}
      candidate=mainPending;
    }
    if(countRecordRoute(route)){if(!mainPending){mainPending=ruleRequest({...candidate,operation:board.operation,record_revision:board.record_revision,kind:route==='files.caller_exemptions'?'caller_exemption':'security_rule'});if(!mainPending)throw new Error('Check the values on this page.');keepDraft();fieldControls();}candidate=mainPending;}
    if(securityRoute(route)){
      if(mainPending)return await reviewSavedSecurity();
      candidate=securityRequest(candidate);if(!candidate)throw new Error('Check the security level values.');
      if(!await confirmSecurity(candidate))return false;
      mainPending=candidate;keepDraft();fieldControls();
    }
    const result=await api('/api/config/page/'+encodeURIComponent(route),candidate);if(recordRoute(route))recordId=result.record;mainPending=null;setDirty(false);await loadAfterKnownSave(result.detail||'Saved.');await refresh();return true;
  }catch(error){if(error.response&&(error.response.unresolved===false||((securityRoute(route)||accountRoute(route))&&error.response.committed===false)))mainPending=null;$('config-result').textContent=error.message+(mainPending?(systemPasswordRoute(route)?' Use Review Password Status. This replacement will not be repeated.':accountRoute(route)?' Use Review Saved Account. This action will not be sent again.':securityRoute(route)?' Use Review Saved Policy. This change will not be sent again.':' Use Reconcile Save to check this same operation.'):dirty?' Your edits have been kept.':' Reload this page to check the saved settings.');return false;}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
}
function throttleControls(){
  if(!throttleRoute(route)||!board)return;
  const fields=formValues(),valid=Number.isInteger(fields.scope)&&fields.scope>=0&&fields.scope<=3&&typeof fields.subject==='string'&&(fields.scope<2?!!fields.subject:!fields.subject);
  $('save-board').disabled=busy||saving||board.can_save===false||!valid;
}
function answerThrottle(accepted){const resolve=throttleConfirmation;throttleConfirmation=null;$('throttle-dialog').close();if(resolve)resolve(accepted);}
async function clearLoginDelay(){
  const request=formValues(),names=['Source IP: ','Account ID: ','Shared System Password delay','Shared BBS login delay'];
  $('throttle-target').textContent=names[request.scope]+(request.scope<2?request.subject:'');
  if(!await new Promise(resolve=>{throttleConfirmation=resolve;$('throttle-dialog').showModal();}))return false;
  try{
    const result=await api('/api/config/throttle/clear',request);
    for(const field of board.fields)field.value=request[field.key];throttleUnconfirmed=false;setDirty(false);$('config-result').textContent=result.detail;return true;
  }catch(error){
    if(error.response?.unresolved!==false){throttleUnconfirmed=true;board.can_save=false;setDirty(true);}
    $('config-result').textContent=error.message+(throttleUnconfirmed?' Check the caller and logs before trying again. Discard Changes reloads this page; no automatic retry will occur.':'');return false;
  }
}
function systemPasswordPending(value){return value&&typeof value==='object'&&Object.keys(value).length===2&&value.action==='system_password'&&typeof value.revision==='string'&&/^[1-9][0-9]{0,18}$/.test(value.revision)?{action:value.action,revision:value.revision}:null;}
function clearSystemPasswords(){$('system-password').value='';$('system-password-confirm').value='';}
function systemPasswordControls(){
  const locked=busy||saving,enabled=systemPasswordRoute(route)&&board?.can_save;
  for(const id of ['system-password','system-password-confirm'])$(id).disabled=locked||!enabled||!!mainPending;
  $('system-password-save').disabled=locked||(!mainPending&&(!enabled||!dirty));
  $('system-password-save').textContent=mainPending&&systemPasswordRoute(route)?'Review Password Status':'Replace System Password';
  $('system-password-clear').disabled=locked||!!mainPending||!dirty;
  $('system-password-refresh').disabled=locked||!!mainPending||dirty;
}
function answerSystemPassword(accepted){const resolve=systemPasswordConfirmation;systemPasswordConfirmation=null;$('system-password-dialog').close();if(resolve)resolve(accepted);}
async function saveSystemPassword(){
  if(mainPending){
    const before=mainPending.revision,current=await api('/api/config/page/system.system_password');
    mainPending=null;clearSystemPasswords();board=current;renderForm(current);setDirty(false);
    $('config-result').textContent='Current permanent password revision: '+current.revision+' (before submission: '+before+'). This reads status only; it cannot confirm which password is stored. Check the result before replacing it again. No replacement was repeated.';return true;
  }
  const accepted=await new Promise(resolve=>{systemPasswordConfirmation=resolve;$('system-password-dialog').showModal();});if(!accepted)return false;
  const request={revision:board.revision,password:$('system-password').value,confirmation:$('system-password-confirm').value};clearSystemPasswords();setDirty(false);
  try{
    if(!request.password||request.password.length>128||request.password!==request.confirmation)throw new Error('Enter the same permanent System Password twice. Password fields were cleared.');
    mainPending={action:'system_password',revision:request.revision};keepDraft();systemPasswordControls();
    let result;try{result=await api('/api/config/page/system.system_password',request);}catch(error){
      if(error.response?.committed===true)result=error.response;
      else{if(error.response?.committed===false)mainPending=null;throw error;}
    }
    mainPending=null;setDirty(false);
    try{await loadBoard();$('config-result').textContent=result.detail;}
    catch(error){board.can_save=false;systemPasswordControls();$('config-result').textContent=result.detail+' Current readback is unavailable. Use Refresh Status; do not repeat the replacement. '+error.message;}
    return true;
  }catch(error){$('config-result').textContent=error.message+(mainPending?' Use Review Password Status. The password will not be sent again.':'');return false;}
  finally{request.password='';request.confirmation='';keepDraft();}
}
function accountSecrets(){return accountRoute(route)&&!!($('account-password').value||$('account-password-confirm').value);}
function clearAccountSecrets(){$('account-password').value='';$('account-password-confirm').value='';}
function accountDraftRequest(value){
  const keys=['revision','policy_revision','record','values'],fields=safeDraftFields['users.accounts'];
  if(!value||typeof value!=='object'||Object.keys(value).length!==keys.length||keys.some(key=>!Object.hasOwn(value,key))||typeof value.record!=='string'||!(/^(0|[1-9][0-9]{0,18})$/).test(value.record)||!/^[1-9][0-9]{0,18}$/.test(value.revision)||!/^[1-9][0-9]{0,18}$/.test(value.policy_revision))return null;
  const values=value.values;if(!values||typeof values!=='object'||Object.keys(values).length!==fields.length||fields.some(key=>!Object.hasOwn(values,key)))return null;
  for(const key of fields){const item=values[key];if(key==='security_level'){if(!Number.isInteger(item)||item<1||item>999)return null;}else if(key==='purge_allowed'){if(typeof item!=='boolean')return null;}else if(typeof item!=='string'||item.length>254)return null;}
  return {revision:value.revision,policy_revision:value.policy_revision,record:value.record,values:Object.fromEntries(fields.map(key=>[key,values[key]]))};
}
function accountPending(value){
  if(!value||typeof value!=='object'||Object.keys(value).length!==2||!['save','create','deactivate','reactivate','delete','reset_password'].includes(value.action))return null;
  const base=accountDraftRequest(value.base);if(!base||(value.action==='create')!==(base.record==='0'))return null;return {action:value.action,base};
}
function renderAccount(){
  const state=board?.account;if(!state)return;
  const lines=[recordId==='0'?'New User':recordId?'Caller #'+recordId+' - '+state.state+(state.online?' - Online':' - Offline'):'Select a caller.'];
  if(state.successful_logins!==undefined)lines.push('Successful logins: '+state.successful_logins+'; session seconds: '+state.session_seconds);
  if(state.temporary)lines.push('Temporary password: '+state.temporary.state+'; expires '+avTime(Number(state.temporary.expires_utc)));
  $('account-status').textContent=lines.join('\n');$('account-toggle').textContent=state.state==='active'?'Deactivate User':'Reactivate User';accountControls();
}
function accountControls(){
  const enabled=accountRoute(route)&&board?.can_save,locked=busy||saving||!!mainPending;
  for(const id of ['account-password','account-password-confirm'])$(id).disabled=locked||!enabled;
  $('account-reset').disabled=locked||!enabled||recordId==='0'||dirty;
  $('account-toggle').disabled=locked||!enabled||recordId==='0'||dirty||accountSecrets();
  $('account-clear-password').disabled=locked||!accountSecrets();
  $('account-reload').disabled=locked||dirty||accountSecrets();
  if(accountRoute(route))$('delete-record').disabled=locked||!enabled||dirty||accountSecrets()||!board?.can_remove;
}
function confirmAccount(text){$('account-warning').textContent=text;return new Promise(resolve=>{accountConfirmation=resolve;$('account-dialog').showModal();});}
function answerAccount(accepted){const resolve=accountConfirmation;accountConfirmation=null;$('account-dialog').close();if(resolve)resolve(accepted);}
function consumeAccountPasswords(){
  const password=$('account-password').value,confirmation=$('account-password-confirm').value;clearAccountSecrets();
  if(!password||password.length>128||password!==confirmation)throw new Error('Enter the same password twice. Password fields were cleared; enter them again.');
  return {password,confirmation};
}
async function accountSavedReadback(detail){
  try{await loadBoard();$('config-result').textContent=detail;}
  catch(error){if(board)board.can_save=false;fieldControls();accountControls();$('config-result').textContent=detail+' Current readback is unavailable. Displayed values are read-only; use Refresh Account. Do not repeat the saved action. '+error.message;}
}
async function reviewSavedAccount(){
  const pending=mainPending,base=pending.base;
  const current=await api('/api/config/page/users.accounts'+(pending.action==='create'?'?prefix='+encodeURIComponent(base.values.name):'?record='+base.record));
  mainPending=null;clearAccountSecrets();
  if(pending.action==='save'){
    const matches=current.record===base.record&&current.can_save&&BigInt(current.revision)===BigInt(base.revision)+1n&&current.fields.every(field=>field.value===base.values[field.key]);
    if(!matches){board.can_save=false;setDirty(true);$('config-result').textContent='Current account values differ or have a newer revision. Your original draft is retained. Discard Changes reloads the saved record; no change was repeated.';return false;}
  }
  board=current;recordId=current.record;if(pending.action==='create')accountSearch={prefix:base.values.name,offset:0};setDirty(false);renderForm(current);renderRecords(current);renderAccount();$('config-result').textContent=pending.action==='save'?'Saved account matches the reviewed values. No change was repeated.':'Current account state loaded for review. No action was repeated. For a new user, select the matching record; password fields are empty.';return true;
}
async function saveAccount(){
  if(mainPending)return reviewSavedAccount();
  const creating=recordId==='0',base=accountDraftRequest({revision:board.revision,policy_revision:board.policy_revision,record:recordId,values:formValues()});
  if(!base)throw new Error('Select a caller and review the profile fields.');
  if(!creating&&accountSecrets())throw new Error('Use Reset Password for password changes, or clear the password fields before saving the caller record.');
  if(!await confirmAccount(creating?'Create this user? The current New User security level will be assigned.':'Save caller #'+recordId+'? Renaming disconnects this caller. Reduced access applies online; new Sysop access requires a fresh login and SY authentication.'))return false;
  const request={...base,...(creating?consumeAccountPasswords():{})};mainPending={action:creating?'create':'save',base};keepDraft();fieldControls();
  try{const result=await api('/api/config/page/users.accounts',request);mainPending=null;setDirty(false);recordId=result.record;if(creating){accountSearch={prefix:result.created_name,offset:0};$('caller-prefix').value=accountSearch.prefix;}await accountSavedReadback(result.detail);return true;}
  finally{delete request.password;delete request.confirmation;}
}
async function performAccountAction(action){
  if(!accountRoute(route)||busy||dirty||mainPending||!board?.can_save||!recordId||recordId==='0')return;
  if(action!=='reset_password'&&accountSecrets()){$('config-result').textContent='Clear the password fields before this account action.';return;}
  busy=true;lockBoard(true);const base=accountDraftRequest({revision:board.revision,policy_revision:board.policy_revision,record:recordId,values:formValues()});
  let request=null;try{
    const warning=action==='delete'?'Delete caller #'+recordId+'? Login will be blocked and online sessions disconnected. A deleted-account record and session/audit history remain.':action==='deactivate'?'Deactivate caller #'+recordId+'? Online sessions disconnect and login stays blocked until reactivated.':action==='reactivate'?'Reactivate caller #'+recordId+'? Normal password, access and network checks still apply.':'Issue a temporary password to caller #'+recordId+' and disconnect their sessions? It expires after 72 hours or first use. The caller must replace it at login.';
    if(!base||!await confirmAccount(warning))return;
    request={record:base.record,revision:base.revision,...(action==='reset_password'?consumeAccountPasswords():{})};mainPending={action,base};keepDraft();fieldControls();
    const result=await api('/api/config/accounts/'+action,request);mainPending=null;keepDraft();await accountSavedReadback(result.detail);
  }catch(error){if(error.response?.committed===false)mainPending=null;$('config-result').textContent=error.message+(mainPending?' Use Review Saved Account. This action will not be repeated.':' Reload and review before trying again.');}
  finally{if(request){delete request.password;delete request.confirmation;}busy=false;lockBoard(false);setDirty(dirty);}
}
const messageKinds=['line_limit','private_colors','system_sysop','unread_age'];
function messageRequest(value){
  if(!value||typeof value!=='object'||!messageKinds.includes(value.kind)||value.policy!=='message')return null;
  const sysop=value.kind==='system_sysop',keys=['kind','policy','revision','operation','record','record_revision','value',...(sysop?['assignment_revision']:[])];
  if(Object.keys(value).length!==keys.length||keys.some(key=>!Object.hasOwn(value,key))||!/^[1-9][0-9]{0,18}$/.test(value.revision)||!/^[1-9][0-9]{0,18}$/.test(value.record_revision)||!/^[a-f0-9]{32}$/.test(value.operation))return null;
  if(value.kind==='line_limit'&&(!Number.isInteger(value.record)||value.record<1||value.record>999||!Number.isInteger(value.value)||value.value<1||value.value>65535))return null;
  if(value.kind==='unread_age'&&(value.record!==null||(value.value!==null&&(!Number.isInteger(value.value)||value.value<1||value.value>65535))))return null;
  if(value.kind==='private_colors'&&(value.record!==null||typeof value.value!=='boolean'))return null;
  if(sysop&&(typeof value.record!=='string'||!/^[1-9][0-9]{0,18}$/.test(value.record)||value.value!==value.record||!/^[1-9][0-9]{0,18}$/.test(value.assignment_revision)))return null;
  return Object.fromEntries(keys.map(key=>[key,value[key]]));
}
function messageCandidate(kind,original=false){
  const item=messageUtilities[kind];if(!item)return null;const current=item.base,control=$('message-value-'+kind);
  const value=original?current.value:kind==='private_colors'?control.checked:kind==='unread_age'?(control.value===''?null:Number(control.value)):kind==='line_limit'?Number(control.value):control.value;
  const choice=kind==='system_sysop'?current.records.find(row=>row.id===value):null;
  return messageRequest({kind,policy:'message',revision:current.revision,operation:current.operation,record:choice?choice.id:current.record,record_revision:choice?choice.revision:current.record_revision,value,...(kind==='system_sysop'?{assignment_revision:current.assignment_revision}:{})});
}
function messageUnresolved(){return Object.values(messageUtilities).some(item=>item.pending);}
function messageDrafts(){
  const drafts={};for(const [kind,item] of Object.entries(messageUtilities))if(item.dirty||item.pending){
    const base=messageCandidate(kind,kind!=='system_sysop');if(!base&&!item.pending)continue;
    drafts[kind]={base:base||item.pending,value:kind==='private_colors'?$('message-value-'+kind).checked:$('message-value-'+kind).value,pending:item.pending?messageRequest(item.pending):null,search:kind==='system_sysop'?{prefix:item.base.prefix,offset:item.base.offset}:null};
  }return drafts;
}
function restoreMessageDrafts(drafts){
  if(!drafts||typeof drafts!=='object')return;
  for(const kind of messageKinds){const saved=drafts[kind],item=messageUtilities[kind];if(!saved||!item)continue;const base=messageRequest(saved.base);if(!base||base.kind!==kind||(kind==='line_limit'&&base.record!==recordId))continue;
    item.base={...item.base,...base};if(kind==='system_sysop'&&!item.base.records.some(row=>row.id===base.record))item.base.records.push({id:base.record,name:'Draft caller #'+base.record,revision:base.record_revision});
    if(kind==='system_sysop'){for(const row of item.base.records)if(row.id===base.record)row.revision=base.record_revision;item.base.value=null;item.base.record=null;}
    renderMessageUtility(kind);const control=$('message-value-'+kind);
    if(kind==='private_colors'&&typeof saved.value==='boolean')control.checked=saved.value;else if(typeof saved.value==='string'&&saved.value.length<=30)control.value=saved.value;
    const pending=messageRequest(saved.pending);if(pending&&pending.kind===kind&&pending.operation===base.operation&&pending.revision===base.revision&&pending.record===base.record){item.pending=pending;if(kind==='private_colors')control.checked=pending.value;else control.value=pending.value??'';}
    messageChanged(kind);
  }
}
function messageControls(){
  const unresolved=messageUnresolved();$('dirty-discard').disabled=busy||saving||!!qwkPending||!!mainPending||unresolved;
  for(const [kind,item] of Object.entries(messageUtilities)){
    const unavailable=item.unavailable||(kind!=='system_sysop'&&!item.base.can_save),locked=busy||saving||unresolved||unavailable;
    $('message-value-'+kind).disabled=locked;$('message-save-'+kind).disabled=busy||saving||dirty||(unresolved&&!item.pending)||(!item.pending&&(!item.dirty||unavailable));$('message-save-'+kind).textContent=item.pending?'Reconcile Setting':'Save Setting';
    $('message-reload-'+kind).disabled=busy||saving||unresolved;
    if(kind==='system_sysop'){const edited=$('sysop-prefix').value!==item.base.prefix;for(const id of ['sysop-prefix','sysop-find'])$(id).disabled=busy||saving||item.dirty||unresolved;$('sysop-previous').disabled=busy||saving||item.dirty||unresolved||edited||!item.base.offset;$('sysop-next').disabled=busy||saving||item.dirty||unresolved||edited||!item.base.has_more;}
  }
}
function messageChanged(kind){const item=messageUtilities[kind],control=$('message-value-'+kind);if(!item)return;item.dirty=kind==='private_colors'?control.checked!==item.base.value:(kind==='line_limit'||kind==='unread_age')?control.value!==String(item.base.value??''):!!control.value;messageControls();accountControls();systemPasswordControls();throttleControls();keepDraft();}
function renderMessageUtility(kind){
  const item=messageUtilities[kind],current=item.base,section=$('message-section-'+kind);section.replaceChildren(element('hr'));
  section.append(element('h2',kind==='unread_age'?'Unread Private Inbox Age':kind==='line_limit'?'Message Body Limit':kind==='private_colors'?'Private Message Colors':'System Sysop'));
  section.append(element('p',kind==='unread_age'?'Enter 1 to 65,535 full days, or leave blank for Unlimited. Save never purges. Read mail and sender copies remain; the next retention job applies this limit.':kind==='line_limit'?'Save this limit separately from access and time settings. Use 1 to 65,535 body lines; new levels start at 200. Existing bodies remain unchanged.':kind==='private_colors'?'Renegade pipe colors apply to private local mail and messages without an area. Stored source bytes remain unchanged.':'Current System Sysop: '+current.assigned+'. Designation grants no privileges and does not transfer old mail. Reassign before locking, removing or demoting this caller.','field-help'));
  if(kind==='system_sysop'){
    const label=element('label','Caller name starts with');label.htmlFor='sysop-prefix';const input=element('input');input.id='sysop-prefix';input.maxLength=30;input.value=current.prefix;input.addEventListener('input',messageControls);
    const buttons=element('div','','form-actions');for(const [id,caption,direction] of [['sysop-find','Find',0],['sysop-previous','Previous Callers',-1],['sysop-next','Next Callers',1]]){const button=element('button',caption);button.id=id;button.type='button';button.onclick=()=>findSysop(direction);buttons.append(button);}section.append(label,input,buttons,element('p','Showing eligible Sysops from caller results '+(current.offset+1)+' onward. A page may have no eligible choices.','field-help'));
  }
  const id='message-value-'+kind,label=element('label',kind==='unread_age'?'Unread inbox age in days (blank = Unlimited)':kind==='line_limit'?'Maximum body lines for level '+recordId:kind==='private_colors'?'Use private pipe colors':'Assign eligible caller');label.htmlFor=id;
  const control=document.createElement(kind==='system_sysop'?'select':'input');control.id=id;
  if(kind==='unread_age'){control.type='number';control.min=1;control.max=65535;control.step=1;control.value=current.value??'';}
  else if(kind==='line_limit'){control.type='number';control.min=1;control.max=65535;control.step=1;control.required=true;control.value=current.value??'';}
  else if(kind==='private_colors'){control.type='checkbox';control.checked=current.value;label.className='checkbox-label';}
  else{const empty=element('option','Choose a caller');empty.value='';control.append(empty);for(const row of current.records){const option=element('option',row.name);option.value=row.id;control.append(option);}control.value=current.value??'';}
  control.addEventListener('input',()=>messageChanged(kind));control.addEventListener('change',()=>messageChanged(kind));if(kind==='private_colors'){label.prepend(control);section.append(label);}else section.append(label,control);
  const actions=element('div','','form-actions'),save=element('button','Save Setting'),reload=element('button','Discard / Reload Setting');save.id='message-save-'+kind;reload.id='message-reload-'+kind;save.type=reload.type='button';save.onclick=()=>saveMessageUtility(kind);reload.onclick=async()=>{if(busy||item.pending)return;item.dirty=false;keepDraft();await loadMessageUtility(kind);};actions.append(save,reload);section.append(actions);const result=element('p');result.id='message-result-'+kind;result.setAttribute('role','status');section.append(result);messageControls();
}
async function loadMessageUtility(kind,search=null){
  const generation=messageLoad,selected=recordId,previous=messageUtilities[kind];if(previous&&(previous.dirty||previous.pending))return;
  try{const suffix=kind==='line_limit'?'?record='+selected:kind==='system_sysop'?'?prefix='+encodeURIComponent(search?.prefix??previous?.base.prefix??'')+'&offset='+(search?.offset??previous?.base.offset??0):'';
    const current=await api('/api/config/message-utility/'+kind+suffix);if(generation!==messageLoad||selected!==recordId||messageUtilities[kind]?.dirty||messageUtilities[kind]?.pending)return;
    if(current.kind!==kind||current.policy!=='message')throw new Error('Unexpected setting response.');
    messageUtilities[kind]={base:current,dirty:false,pending:null,unavailable:false};renderMessageUtility(kind);return true;
  }catch(error){if(generation===messageLoad){
    const retained=messageUtilities[kind];if(retained){retained.unavailable=true;$('message-result-'+kind).textContent='Current message setting unavailable. Displayed values are read-only until refreshed. Discard / Reload Setting retries the read. '+error.message;messageControls();}
    else $('message-section-'+kind).textContent='Message setting unavailable. '+error.message;
  }return false;}
}
async function loadMessageUtilities(){
  const kinds=route==='general.board_identity'?['system_sysop','private_colors']:securityRoute(route)&&recordId>0&&recordId<1000?['line_limit']:jobsRoute(route)&&recordId===2?['unread_age']:[];
  for(const kind of kinds){if(!$('message-section-'+kind)?.isConnected){const section=element('section');section.id='message-section-'+kind;$('message-utilities').append(section);}
    const saved=restoringDraft?retainedDraft()?.messages?.[kind]?.search:null;
    const search=kind==='system_sysop'&&saved&&typeof saved.prefix==='string'&&saved.prefix.length<=30&&Number.isInteger(saved.offset)&&saved.offset>=0&&saved.offset<=1000000?saved:null;
    await loadMessageUtility(kind,search);}
}
async function findSysop(direction){const item=messageUtilities.system_sysop;if(!item||busy||item.dirty||messageUnresolved())return;
  const prefix=direction?item.base.prefix:$('sysop-prefix').value,offset=direction?item.base.offset+direction*32:0;if(prefix.length>30||offset<0||offset>1000000)return;
  busy=true;messageControls();try{await loadMessageUtility('system_sysop',{prefix,offset});}finally{busy=false;messageControls();}
}
function answerMessageUtility(accepted){const resolve=messageConfirmation;messageConfirmation=null;$('message-utility-dialog').close();if(resolve)resolve(accepted);}
async function saveMessageUtility(kind){
  const item=messageUtilities[kind];if(!item||busy||dirty||(item.unavailable&&!item.pending)||(messageUnresolved()&&!item.pending)||(!item.dirty&&!item.pending))return false;
  busy=true;lockBoard(true);try{
    if(!item.pending){const candidate=messageCandidate(kind);if(!candidate)throw new Error('Choose an eligible caller or enter an explicit limit from 1 to 65,535.');
      $('message-utility-warning').textContent=kind==='unread_age'?'Save unread inbox age: '+(candidate.value===null?'Unlimited':candidate.value+' full days')+'? No messages are removed by Save.':kind==='line_limit'?'Save '+candidate.value+' body lines for level '+candidate.record+'? Existing bodies remain unchanged; connected drafts revalidate before saving.':kind==='private_colors'?'Turn private pipe colors '+(candidate.value?'On':'Off')+'? Stored source bytes remain unchanged.':'Assign System Sysop to caller #'+candidate.record+'? No privileges change and old mail stays with its original recipient.';
      if(!await new Promise(resolve=>{messageConfirmation=resolve;$('message-utility-dialog').showModal();}))return false;
      item.pending=candidate;keepDraft();messageControls();
    }
    const result=await api('/api/config/message-utility/'+kind,item.pending);item.pending=null;item.dirty=false;keepDraft();const readback=await loadMessageUtility(kind);if(readback)$('message-result-'+kind).textContent=result.detail;else $('message-result-'+kind).textContent=result.detail+' Current readback is unavailable; displayed values are read-only until refreshed. Use Discard / Reload Setting to refresh.';return true;
  }catch(error){if(error.response?.unresolved===false)item.pending=null;$('message-result-'+kind).textContent=error.message+(item.pending?' Reconcile Setting checks the same operation.':' Your draft is retained.');return false;}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
}
function confirmSecurity(candidate){
  const labels=Object.fromEntries(board.fields.map(field=>[field.key,field.label]));
  $('security-save-summary').replaceChildren(...Object.entries(candidate.values).map(([key,value])=>element('p',labels[key]+': '+value)));
  return new Promise(resolve=>{securityConfirmation=resolve;$('security-save-dialog').showModal();});
}
function answerSecurity(accepted){const resolve=securityConfirmation;securityConfirmation=null;$('security-save-dialog').close();if(resolve)resolve(accepted);}
async function reviewSavedSecurity(){
  const original=mainPending;const selected=original.record||original.values.level;
  const current=await api('/api/config/page/users.security_levels?record='+selected);
  const matches=current.record===selected&&current.can_save&&BigInt(current.revision)===BigInt(original.revision)+1n&&current.fields.every(field=>field.value===original.values[field.key]);
  mainPending=null;
  if(matches){recordId=selected;board=current;renderForm(current);renderRecords(current);if(accountRoute(route))renderAccount();setDirty(false);$('config-result').textContent='Saved policy matches the reviewed values. No new change was sent.';return true;}
  board.can_save=false;setDirty(true);$('config-result').textContent='The saved policy differs or has a newer revision. Your original draft is retained. Discard Changes reloads the current policy before another save; no change was repeated.';return false;
}
async function removalPage(offset){
  const generation=++securityReviewLoad,previous=securityRemoval;const value=await api('/api/config/security/removal?record='+recordId+'&offset='+offset);
  if(generation!==securityReviewLoad)return false;
  if(value.revision!==board.revision||(previous&&(value.revision!==previous.revision||value.affected_hash!==previous.affected_hash)))throw new Error('The security policy or affected callers changed. Cancel and reload before reviewing removal.');
  const replacement=$('security-replacement').value;securityRemoval=value;
  $('security-removal-message').textContent='Remove level '+recordId+' permanently? '+value.total+' callers are assigned. Their replacement also replaces any default using this level. This number cannot be reused.';
  $('security-affected').replaceChildren(...value.accounts.map(account=>element('li','#'+account.id+'  '+account.name)));
  if(!value.accounts.length)$('security-affected').append(element('li','No callers assigned.'));
  $('security-review-page').textContent=value.total?'Callers '+(value.offset+1)+' to '+(value.offset+value.accounts.length)+' of '+value.total:'';
  $('security-review-previous').disabled=value.offset===0;$('security-review-next').disabled=value.offset+value.accounts.length>=value.total;
  $('security-replacement').replaceChildren(element('option','Choose a replacement'));
  $('security-replacement').children[0].value='';
  for(const option of value.replacements){const node=element('option',option.name);node.value=option.id;$('security-replacement').append(node);}
  $('security-replacement').value=value.replacements.some(item=>String(item.id)===replacement)?replacement:'';
  $('security-remove-confirm').disabled=!$('security-replacement').value;
  return true;
}
async function removeSecurityLevel(){
  if(busy||dirty||mainPending||!board?.can_remove)return;busy=true;lockBoard(true);securityRemoval=null;
  try{if(!await removalPage(0))return;$('security-remove-cancel').disabled=false;$('security-replacement').disabled=false;$('security-removal-result').textContent='';$('security-remove-dialog').showModal();}
  catch(error){$('config-result').textContent=error.message;busy=false;lockBoard(false);setDirty(dirty);}
}
function closeSecurityRemoval(){++securityReviewLoad;securityRemoval=null;$('security-remove-dialog').close();busy=false;lockBoard(false);setDirty(dirty);}
async function confirmSecurityRemoval(){
  if(!securityRemoval||!$('security-replacement').value)return;
  const candidate={revision:securityRemoval.revision,record:recordId,replacement:Number($('security-replacement').value),affected_hash:securityRemoval.affected_hash};
  for(const id of ['security-remove-confirm','security-remove-cancel','security-replacement','security-review-previous','security-review-next'])$(id).disabled=true;
  try{const result=await api('/api/config/security/remove',candidate);recordId=result.record;closeSecurityRemoval();await loadBoard();$('config-result').textContent=result.detail;}
  catch(error){$('security-removal-result').textContent=error.message+' No automatic retry was made. Cancel and reload to inspect the saved state.';$('security-remove-cancel').disabled=false;}
}
function qwkControls(){for(const id of ['qwk-id','qwk-maximum'])$(id).disabled=busy||!qwk||!!qwkPending;$('save-qwk').disabled=busy||!qwk||(!qwkDirty&&!qwkPending);$('save-qwk').textContent=qwkPending?'Reconcile QWK':'Save QWK';$('discard-qwk').disabled=busy||!!qwkPending;}
function qwkChanged(){qwkDirty=!!qwk&&($('qwk-id').value!==qwk.board_id||Number($('qwk-maximum').value)!==qwk.maximum_messages);qwkControls();keepDraft();}
async function loadQwk(){const generation=++qwkLoad;try{const current=await api('/api/config/qwk');if(generation!==qwkLoad||page!=='config'||route!=='general.board_identity'||qwkDirty||qwkPending)return;qwk=current;$('qwk-id').value=current.board_id;$('qwk-maximum').value=current.maximum_messages;$('qwk-result').textContent='';qwkControls();}catch(error){if(generation===qwkLoad)$('qwk-result').textContent=error.message;}}
function confirmQwk(changing){$('qwk-warning').textContent=changing?'Change the QWK ID? Older QWK and REP packets will no longer match. Stored messages and conference mappings stay unchanged.':'Save the maximum number of messages per QWK download?';return new Promise(resolve=>{qwkConfirmation=resolve;$('qwk-dialog').showModal();});}
function answerQwk(accepted){const resolve=qwkConfirmation;qwkConfirmation=null;$('qwk-dialog').close();if(resolve)resolve(accepted);}
async function saveQwk(){if(!qwk||busy||(!qwkPending&&!$('qwk-form').reportValidity()))return false;
  busy=true;lockBoard(true);++qwkLoad;try{
    if(!qwkPending){const candidate={operation:qwk.operation,revision:qwk.revision,board_id:$('qwk-id').value,maximum_messages:Number($('qwk-maximum').value)};if(!await confirmQwk(candidate.board_id.toUpperCase()!==qwk.board_id))return false;qwkPending=candidate;keepDraft();}
    const result=await api('/api/config/qwk',qwkPending);qwkPending=null;qwkDirty=false;await loadQwk();$('qwk-result').textContent=result.detail;return true;
  }catch(error){if(error.response&&error.response.unresolved===false)qwkPending=null;$('qwk-result').textContent=error.message+(qwkPending?' Use Reconcile QWK to check the same save.':' Your QWK draft has been kept.');return false;}finally{busy=false;lockBoard(false);setDirty(dirty);qwkControls();}}
function statisticsControls(){
  $('exclude-sysop-statistics').disabled=busy||!statistics;
  $('save-statistics').disabled=busy||!statistics||!statisticsDirty||dirty;
  $('discard-statistics').disabled=busy||!statistics;
}
function statisticsChanged(){statisticsDirty=!!statistics&&$('exclude-sysop-statistics').checked!==statistics.exclude_sysop_statistics;statisticsControls();keepDraft();}
async function loadStatistics(){const generation=++statisticsLoad;try{
  const current=await api('/api/config/statistics');if(generation!==statisticsLoad||page!=='config'||route!=='general.system_information'||statisticsDirty)return;
  statistics=current;$('exclude-sysop-statistics').checked=current.exclude_sysop_statistics;$('statistics-result').textContent='';statisticsControls();
}catch(error){if(generation===statisticsLoad)$('statistics-result').textContent=error.message;}}
async function saveStatistics(){
  if(!statistics||busy||!statisticsDirty)return false;
  if(dirty){$('statistics-result').textContent='Save or discard the opening-date edits first. Both drafts have been kept.';return false;}
  busy=true;lockBoard(true);++statisticsLoad;try{
    const result=await api('/api/config/statistics',{revision:statistics.revision,exclude_sysop_statistics:$('exclude-sysop-statistics').checked});
    statisticsDirty=false;keepDraft();await loadStatistics();$('statistics-result').textContent=result.detail;return true;
  }catch(error){$('statistics-result').textContent=error.message+' Your statistics draft has been kept.';return false;}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
}
function avControls(){
  const running=!!antivirus&&(antivirus.active||antivirus.queued||antivirus.busy);
  $('antivirus-update').disabled=busy||avBusy||hasDraft()||!antivirus?.component_verified||running;
  $('antivirus-cancel').disabled=busy||avBusy||!antivirus||!(antivirus.active||antivirus.queued)||!antivirus.operation||antivirus.operation==='0';
  $('antivirus-refresh').disabled=avBusy;$('antivirus-view-log').disabled=avBusy;
}
function avTime(seconds){if(!Number.isSafeInteger(seconds)||seconds<=0)return 'Unavailable';const value=new Date(seconds*1000);return Number.isNaN(value.getTime())?'Unavailable':value.toISOString().replace('T',' ').replace('.000Z',' UTC');}
function renderAntivirus(value){
  const active=value.active||value.queued||value.busy;
  const lines=[!value.installed?'Portable ClamAV: Not installed.':!value.component_verified?'Portable ClamAV: Installation could not be verified. Use the matched installer to repair it.':'Portable ClamAV: Verified engine '+value.engine_version,
    'Definitions: '+(active?'Check in progress; readiness is not confirmed.':value.definitions_verified?'Verified against the last successful vendor check.':value.definitions_present?'Present, but not verified.':'Not ready or unavailable.'),
    'Last successful check/update: '+avTime(value.last_success),'Last attempt: '+avTime(value.last_attempt)+(value.history_available?(value.manual?' (manual)':' (automatic)'):''),
    'Last result: '+(value.last_result||'Unavailable'),'Last definition change: '+avTime(value.last_change)];
  if(value.definitions_verified&&!active)lines.push('Definition result: '+(['Unavailable','Initial definitions installed','Definitions updated','Definitions already current'][value.change]||'Unavailable'));
  for(const [index,name] of ['Main','Daily','Bytecode'].entries()){const database=value.databases?.[index];lines.push(name+': '+(!active&&value.definitions_verified&&database?'version '+database.version+'; '+avTime(database.published):'Version/date unavailable (not verified).'));}
  if(active)lines.push((value.queued?'Update queued.':'Checking definitions / waiting for another scan or update.')+' Elapsed '+Math.floor((value.elapsed_milliseconds||0)/1000)+' seconds. Cancel Update is available.');
  else if(value.completed_operation&&value.completed_operation!=='0')lines.push(value.update_cancelled?'Update cancelled.':value.update_succeeded?'Latest update completed and verified.':'Latest update did not complete: '+(value.update_diagnostic||'Result unavailable.'));
  if(value.paused)lines.push('Automatic updates are paused for maintenance.');
  if(value.cleanup_complete===false)lines.push('Update working files were preserved for recovery. Check the update log.');
  if(value.diagnostic&&value.diagnostic!==value.last_result)lines.push('Status: '+value.diagnostic);
  $('antivirus-status').replaceChildren(...lines.map(line=>element('p',line)));avControls();
}
async function refreshAntivirus(){
  if(page!=='config'||route!=='security.clamav'||avPolling)return;avPolling=true;const generation=avLoad;
  try{const value=await api('/api/antivirus');if(generation!==avLoad)return;antivirus=value;renderAntivirus(value);}
  catch(error){if(generation===avLoad){antivirus=null;$('antivirus-status').textContent='ClamAV status unavailable. '+error.message;avControls();}}
  finally{avPolling=false;}
}
async function updateAntivirus(cancel=false){
  if(avBusy||busy||page!=='config'||route!=='security.clamav'||!antivirus)return;
  if(cancel?!(antivirus.active||antivirus.queued):hasDraft()||!antivirus.component_verified||antivirus.active||antivirus.queued||antivirus.busy)return;
  const generation=++avLoad,operation=antivirus.operation;avBusy=true;avControls();
  try{await api(cancel?'/api/antivirus/cancel':'/api/antivirus/update',cancel?{operation}:{});
    if(generation===avLoad)$('antivirus-result').textContent=cancel?'Cancellation requested. Waiting for the supervised updater to finish safely.':'Definition check requested. No success is claimed until verification completes.';
  }catch(error){if(generation===avLoad)$('antivirus-result').textContent=error.message+' Refresh Status to check the operation; no automatic retry was made.';}
  finally{avBusy=false;if(generation===avLoad){antivirus=null;avControls();await refreshAntivirus();}}
}
async function viewAntivirusLog(){
  if(avBusy||page!=='config'||route!=='security.clamav')return;const generation=avLoad;avBusy=true;avControls();
  try{const value=await api('/api/antivirus/log');if(generation===avLoad){$('antivirus-log').textContent=value.detail+(value.available?'\n\n'+value.text:'');$('antivirus-log').hidden=false;}}
  catch(error){if(generation===avLoad)$('antivirus-result').textContent=error.message;}
  finally{avBusy=false;avControls();}
}
function recordControls(){
  $('record-picker').disabled=busy||!board||!!mainPending||!!board.readback_failed;$('add-record').disabled=busy||!!mainPending||!board||!!board.readback_failed;
  $('delete-record').disabled=busy||dirty||!!mainPending||!board?.can_remove;accountSearchControls();if(accountRoute(route))accountControls();
}
function renderRecords(current){if(externalEventsRoute(route)){renderExternalEvents(current);return;}if(!recordRoute(route))return;const picker=$('record-picker');picker.replaceChildren();
  if(callerSearchRoute(route)){const option=element('option','Choose a caller');option.value='';picker.append(option);}
  if(current.record===0||current.record==='0'){const option=element('option',commandRoute(route)?'New command':accountRoute(route)?'New User':securityRoute(route)?'New level':'New room');option.value=0;picker.append(option);}
  for(const row of current.records){const option=element('option',row.name);option.value=row.id;picker.append(option);}picker.value=current.record??'';recordControls();configRecordPresentation(current);
}
function renderExternalEvents(current){
  $('record-section').hidden=true;
  const host=$('config-fields'), list=element('section','','external-event-list');
  const heading=element('div','','cfg-browser-heading');
  const actions=element('div','','form-actions');
  const add=element('button','New Event'), remove=element('button','Delete Event');
  add.type=remove.type='button';
  add.onclick=()=>selectRecord('0').catch(error=>notice(error.message));
  remove.disabled=!current.can_remove;
  remove.onclick=deleteExternalEvent;
  actions.append(add,remove);
  heading.append(element('h3','External Events'),actions);
  const table=element('table','','cp-data-table');
  const header=element('tr');
  for(const title of ['Event','Schedule','Next scheduled run (BBS local)','Enabled'])header.append(element('th',title));
  const thead=element('thead'), body=element('tbody');
  thead.append(header);
  for(const event of current.records){
    const row=element('tr'), title=element('td');
    const button=element('button',event.name,'cfg-browse-row');
    button.type='button';
    button.setAttribute('aria-pressed',String(event.id===current.record));
    button.onclick=()=>selectRecord(event.id).catch(error=>notice(error.message));
    title.append(button);
    row.append(title,element('td',event.schedule),element('td',event.next),element('td',event.enabled?'Yes':'No'));
    body.append(row);
  }
  table.append(thead,body);
  list.append(heading,table);
  const editor=element('div','','external-event-editor');
  for(const group of [...host.children])editor.append(group);
  host.append(list,editor);
}
async function deleteExternalEvent(){
  if(busy||saving||hasDraft()||!board?.can_remove||!externalEventsRoute(route))return;
  const event=board.records.find(item=>item.id===recordId);
  if(!event)return;
  const candidate={revision:board.revision,record:recordId};
  busy=true;
  lockBoard(true);
  try{
    if(!await offlineAsk('Delete External Event?',event.name,'A run already in progress will finish.','Delete Event'))return;
    const result=await api('/api/config/external-events/delete',candidate);
    recordId=null;
    setDirty(false);
    await loadAfterKnownSave(result.detail);
  }catch(error){$('config-result').textContent=error.message;}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
}
function accountSearchControls(){
  const locked=busy||saving||!!mainPending;
  $('caller-prefix').disabled=locked;$('caller-find').disabled=locked;
  const edited=$('caller-prefix').value!==accountSearch.prefix;
  $('caller-previous').disabled=locked||edited||!board||accountSearch.offset===0;
  $('caller-next').disabled=locked||edited||!board?.has_more||accountSearch.offset>999968;
  $('caller-page').textContent=callerSearchRoute(route)&&board?'Results '+(accountSearch.offset+1)+' onward. Choose a caller for '+(accountRoute(route)?'account details.':'current exemptions.'):'';
}
async function findCallers(direction=0){
  if(busy||saving||mainPending||!callerSearchRoute(route))return;
  const search=direction?{prefix:accountSearch.prefix,offset:Math.max(0,accountSearch.offset+direction*32)}:{prefix:$('caller-prefix').value,offset:0};
  if(search.offset>1000000||search.prefix.length>30)return;
  if(hasDraft()){pendingNavigation={page:'config',route,record:null,search};$('dirty-page').textContent=board?.title||'Caller Exemptions';$('dirty-dialog').showModal();return;}
  await showPage('config',route,null,search);
}
function retainedRecord(value){
  if(externalEventsRoute(value.route))return typeof value.record==='string'&&/^(0|[1-9][0-9]{0,19})$/.test(value.record)?value.record:null;
  if(accountRoute(value.route)&&value.record==='0')return '0';
  if(callerSearchRoute(value.route))return typeof value.record==='string'&&/^[1-9][0-9]{0,18}$/.test(value.record)?value.record:null;
  return recordRoute(value.route)&&Number.isInteger(value.record)&&value.record>=((value.route==='chat.rooms'||commandRoute(value.route)||securityRoute(value.route))?0:1)&&value.record<=(value.route==='chat.rooms'?65535:commandRoute(value.route)?234:securityRoute(value.route)?1000:jobsRoute(value.route)?3:999)?value.record:null;
}
function retainedSearch(value){const search=value.search;return callerSearchRoute(value.route)&&search&&typeof search.prefix==='string'&&search.prefix.length<=30&&Number.isInteger(search.offset)&&search.offset>=0&&search.offset<=1000000?{prefix:search.prefix,offset:search.offset}:null;}
async function selectRecord(nextRecord){if(busy||nextRecord===recordId)return;$('record-picker').value=recordId;
  if(hasDraft()){pendingNavigation={page:'config',route,record:nextRecord};$('dirty-page').textContent=board?.title||'Chat Rooms';$('dirty-dialog').showModal();return;}
  await showPage('config',route,nextRecord);
}
function answerDeleteRecord(accepted){const resolve=deleteConfirmation;deleteConfirmation=null;$('delete-record-dialog').close();if(resolve)resolve(accepted);}
async function deleteRecord(){if(accountRoute(route))return performAccountAction('delete');if(securityRoute(route))return removeSecurityLevel();if(busy||dirty||!board?.can_remove||(route!=='chat.rooms'&&!commandRoute(route)))return;
  const candidate={revision:board.revision,record:recordId};const name=board.records.find(row=>row.id===recordId)?.name||(commandRoute(route)?'this command':'this room');
  busy=true;lockBoard(true);try{
    $('delete-record-title').textContent=commandRoute(route)?'Remove Door Command?':'Delete Chat Room?';$('delete-record-detail').textContent=commandRoute(route)?'Only this menu command is removed. The registered Door and its files remain.':'The room must be empty. This cannot be undone.';$('confirm-delete-record').textContent=commandRoute(route)?'Remove Command':'Delete Room';$('delete-record-message').textContent=(commandRoute(route)?'Remove ':'Delete ')+name+'?';const accepted=await new Promise(resolve=>{deleteConfirmation=resolve;$('delete-record-dialog').showModal();});if(!accepted)return;
    const result=await api(commandRoute(route)?'/api/config/door-commands/delete':'/api/config/chat/delete',candidate);recordId=result.record;setDirty(false);await loadAfterKnownSave(result.detail||'Removed.');
  }catch(error){$('config-result').textContent=error.message+' Reload the list to check the saved state.';}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
}
async function action(work){if(busy)return;busy=true;notice('');await refresh();try{await work();}catch(error){notice(error.message);}finally{busy=false;await refresh();}}
async function stopBbs(ticket){const result=await api('/api/stop',ticket?{confirmation:ticket}:{});if(result.confirmation){confirmation=result.confirmation;$('stop-message').textContent=result.callers+' caller'+(result.callers===1?' is':'s are')+' online. Stopping will disconnect '+(result.callers===1?'this caller.':'these callers.');if(!$('stop-dialog').open)$('stop-dialog').showModal();}else{confirmation='';$('stop-dialog').close();}}
$('record-picker').onchange=()=>selectRecord(callerSearchRoute(route)?($('record-picker').value||null):Number($('record-picker').value)).catch(e=>notice(e.message));$('add-record').onclick=()=>selectRecord(accountRoute(route)?'0':0).catch(e=>notice(e.message));$('delete-record').onclick=deleteRecord;
$('caller-prefix').oninput=accountSearchControls;$('caller-search-form').onsubmit=event=>{event.preventDefault();findCallers().catch(e=>notice(e.message));};$('caller-next').onclick=()=>findCallers(1).catch(e=>notice(e.message));$('caller-previous').onclick=()=>findCallers(-1).catch(e=>notice(e.message));
$('security-save-confirm').onclick=()=>answerSecurity(true);$('security-save-cancel').onclick=()=>answerSecurity(false);$('security-save-dialog').oncancel=event=>{event.preventDefault();answerSecurity(false);};
$('message-utility-confirm').onclick=()=>answerMessageUtility(true);$('message-utility-cancel').onclick=()=>answerMessageUtility(false);$('message-utility-dialog').oncancel=event=>{event.preventDefault();answerMessageUtility(false);};
$('account-confirm').onclick=()=>answerAccount(true);$('account-cancel').onclick=()=>answerAccount(false);$('account-dialog').oncancel=event=>{event.preventDefault();answerAccount(false);};
$('account-password').oninput=$('account-password-confirm').oninput=()=>{accountControls();systemPasswordControls();throttleControls();keepDraft();};$('account-clear-password').onclick=()=>{if(busy)return;clearAccountSecrets();accountControls();systemPasswordControls();throttleControls();keepDraft();};
$('account-reset').onclick=()=>performAccountAction('reset_password');$('account-toggle').onclick=()=>performAccountAction(board?.account.state==='active'?'deactivate':'reactivate');
$('account-reload').onclick=async()=>{if(busy||mainPending||dirty||accountSecrets())return;busy=true;lockBoard(true);try{await loadBoard();}catch(error){if(board)board.can_save=false;$('config-result').textContent='Account readback is still unavailable. No action was repeated. '+error.message;}finally{busy=false;lockBoard(false);setDirty(dirty);}};
$('security-remove-confirm').onclick=confirmSecurityRemoval;$('security-remove-cancel').onclick=closeSecurityRemoval;$('security-remove-dialog').oncancel=event=>{event.preventDefault();if(!$('security-remove-cancel').disabled)closeSecurityRemoval();};
$('security-replacement').onchange=()=>{$('security-remove-confirm').disabled=!securityRemoval||!$('security-replacement').value;};
for(const [id,direction] of [['security-review-previous',-1],['security-review-next',1]])$(id).onclick=async()=>{
  if(!securityRemoval)return;const offset=securityRemoval.offset+direction*32;
  for(const key of ['security-review-previous','security-review-next','security-remove-confirm'])$(key).disabled=true;
  try{await removalPage(offset);}catch(error){$('security-removal-result').textContent=error.message;}
};
$('antivirus-refresh').onclick=refreshAntivirus;$('antivirus-update').onclick=()=>updateAntivirus();$('antivirus-cancel').onclick=()=>updateAntivirus(true);$('antivirus-view-log').onclick=viewAntivirusLog;
$('confirm-delete-record').onclick=()=>answerDeleteRecord(true);$('cancel-delete-record').onclick=()=>answerDeleteRecord(false);$('delete-record-dialog').addEventListener('cancel',event=>{event.preventDefault();answerDeleteRecord(false);});
$('control-tab').onclick=()=>navigate('panel').catch(e=>notice(e.message));$('config-tab').onclick=()=>navigate('config').catch(e=>notice(e.message));
$('start').onclick=()=>action(()=>api('/api/start',{}));$('stop').onclick=()=>action(()=>stopBbs(''));
$('cp-restart').onclick=cpRestart;$('cp-check-restart').onclick=cpCheckRestart;$('cp-retry-restart').onclick=cpRetryRestart;
$('cp-drain').onclick=()=>cpDrain();$('cp-cancel-drain').onclick=()=>cpDrain(true);
for(const command of ['enable','disable','disconnect','terminate'])$('cp-'+command).onclick=()=>cpNodeAction(command);
$('cp-view').onclick=cpView;$('cp-close-view').onclick=cpCloseView;
$('confirm-stop').onclick=()=>action(()=>stopBbs(confirmation));
async function cancelStop(){confirmation='';$('stop-dialog').close();await action(()=>api('/api/stop/cancel',{}));}
$('cancel-stop').onclick=cancelStop;$('stop-dialog').addEventListener('cancel',event=>{event.preventDefault();cancelStop();});
$('throttle-accept').onclick=()=>answerThrottle(true);$('throttle-cancel').onclick=()=>answerThrottle(false);$('throttle-dialog').oncancel=event=>{event.preventDefault();answerThrottle(false);};
$('system-password').oninput=$('system-password-confirm').oninput=()=>setDirty(!!($('system-password').value||$('system-password-confirm').value));
$('system-password-clear').onclick=()=>{if(busy||mainPending)return;clearSystemPasswords();setDirty(false);};
$('system-password-save').onclick=saveBoard;$('system-password-refresh').onclick=()=>{if(busy||dirty||mainPending)return;return loadBoard().catch(e=>notice(e.message));};
$('system-password-accept').onclick=()=>answerSystemPassword(true);$('system-password-cancel').onclick=()=>answerSystemPassword(false);$('system-password-dialog').oncancel=event=>{event.preventDefault();answerSystemPassword(false);};
$('clear-desktop-client').onclick=()=>{if(route!=='connections.external_client'||busy||saving||offlineBusy||maintenanceActive||board?.can_save===false)return;$('field-type').value='0';$('field-executable').value='';$('field-arguments').value='';setDirty(true);saveBoard();};
$('door-add-native').onclick=()=>doorAddChoose('native').catch(e=>notice(e.message));
$('door-add-dos').onclick=()=>doorAddChoose('dos').catch(e=>notice(e.message));
$('door-add-telnet').onclick=()=>doorAddChoose('telnet').catch(e=>notice(e.message));
$('door-add-rlogin').onclick=()=>doorAddChoose('rlogin').catch(e=>notice(e.message));
$('door-add-cancel').onclick=()=>$('door-add-dialog').close();
$('door-check-result').onclick=doorCreateReview;
$('door-configured-open').onclick=()=>{if(doorCatalogLocked())return;const panel=$('door-configured-panel');panel.hidden=!panel.hidden;$('door-configured-open').setAttribute('aria-expanded',String(!panel.hidden));if(!panel.hidden)panel.scrollIntoView?.({block:'nearest'});};
$('door-runtime-open').onclick=()=>doorCatalogTransition(()=>dwOpen({runtimeOnly:true})).catch(e=>notice(e.message));
$('door-refresh').onclick=()=>doorCatalogTransition(()=>doorCatalogBrowse(doorCatalogState.menu)).catch(e=>notice(e.message));
for(const action of ['save','discard','stay','keep'])$('door-leave-'+action).onclick=()=>doorLeaveAnswer(action).catch(e=>notice(e.message));
$('door-leave-dialog').addEventListener('cancel',event=>{event.preventDefault();doorLeaveAnswer('stay');});
$('board-form').onsubmit=event=>{event.preventDefault();saveBoard();};$('discard-board').onclick=()=>{if(busy||mainPending||doorCatalogState.create?.pending)return;if(doorCatalogRoute(route))doorCatalogState.create=null;throttleUnconfirmed=false;jobRunUnconfirmed=false;clearSystemPasswords();setDirty(false);loadBoard().catch(e=>notice(e.message));};
$('qwk-id').oninput=$('qwk-maximum').oninput=qwkChanged;$('qwk-form').onsubmit=event=>{event.preventDefault();saveQwk();};$('discard-qwk').onclick=()=>{if(qwkPending||busy)return;qwkDirty=false;keepDraft();loadQwk();};
$('exclude-sysop-statistics').onchange=statisticsChanged;$('statistics-form').onsubmit=event=>{event.preventDefault();saveStatistics();};$('discard-statistics').onclick=()=>{if(busy)return;statisticsDirty=false;keepDraft();loadStatistics();};
$('qwk-confirm').onclick=()=>answerQwk(true);$('qwk-cancel').onclick=()=>answerQwk(false);$('qwk-dialog').addEventListener('cancel',event=>{event.preventDefault();answerQwk(false);});
$('dirty-dialog').addEventListener('cancel',event=>{if(saving)event.preventDefault();else pendingNavigation=null;});
$('dirty-cancel').onclick=()=>{if(saving)return;pendingNavigation=null;$('dirty-dialog').close();};
$('dirty-discard').onclick=async()=>{if(qwkPending||mainPending||messageUnresolved()||saving||doorCatalogState.create?.pending||!botgateCanDiscard())return;if(doorCatalogRoute(route))doorCatalogState.create=null;if(ftnRoute(route)&&!await ftnDiscard(false))return;const target=pendingNavigation;pendingNavigation=null;botgateDiscard();setDirty(false);qwkDirty=false;statisticsDirty=false;messageUtilities={};keepDraft();$('dirty-dialog').close();if(target?.catalogAction)await target.catalogAction();else if(target)await showPage(target.page,target.route,target.record??null,target.search??null);};
$('dirty-save').onclick=async()=>{if(ftnRoute(route)&&ftnPending()&&!await ftnWork(ftnSave))return;if(accountSecrets()&&!dirty&&!mainPending){$('config-result').textContent='Use Reset Password to submit a temporary password, or clear the password fields before leaving.';return;}if((dirty||mainPending)&&!await saveBoard())return;if(botgateHasDraft()&&!await botgateSave())return;if((qwkDirty||qwkPending)&&!await saveQwk())return;if(statisticsDirty&&!await saveStatistics())return;for(const [kind,item] of Object.entries(messageUtilities))if((item.dirty||item.pending)&&!await saveMessageUtility(kind))return;const target=pendingNavigation;pendingNavigation=null;$('dirty-dialog').close();if(target?.catalogAction)await target.catalogAction();else if(target)await showPage(target.page,target.route,target.record??null,target.search??null);};
// No native leave-page dialog: non-secret drafts survive reloads in this tab.
// Passwords and other future secret fields are never in the explicit allowlist.
window.addEventListener('pagehide',keepDraft);
let webRefreshTimer=null;
async function connectWeb(){try{const token=location.hash.startsWith('#launch=')?location.hash.slice(8):'';if(location.hash)history.replaceState(null,'',location.pathname);
  const session=token?await api('/api/launch',{token}):await api('/api/session');csrf=session.csrf;draftKey=/^[a-f0-9]{64}$/.test(session.draft_scope)?'sfx-draft-'+session.draft_scope:'';authenticated=true;$('locked').hidden=true;$('workspace').hidden=false;await loadRoutes();await refresh();if(draftKey&&!maintenanceActive){const retained=retainedDraft();if(retained){restoringDraft=true;try{await showPage('config',retained.route,retainedRecord(retained),retainedSearch(retained));restoreDraft(retained);}finally{restoringDraft=false;}keepDraft();}}if(!webRefreshTimer)webRefreshTimer=setInterval(()=>{refresh();if(page==='config'&&route==='security.clamav'&&(!antivirus||antivirus.active||antivirus.queued))refreshAntivirus();},2000);
}catch(error){$('locked').hidden=false;const lan=location.protocol==='https:';$('lan-login').hidden=!lan;$('local-launch-help').hidden=lan;$('connection').textContent='Not connected';notice(error.status===401?(lan?'Sign in with the System Password to continue.':'Open the Spitfire-X launcher to continue.'):error.message);} }
$('lan-login').addEventListener('submit',async event=>{
  event.preventDefault();const field=$('lan-password'),button=$('lan-signin');button.disabled=true;
  try{await api('/api/login',{password:field.value});field.value='';await connectWeb();}
  catch(error){notice(error.message);}
  finally{field.value='';button.disabled=false;}
});
connectWeb();

const previewCp437=" ☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼ !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~⌂ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■ ";
function invalidateMenuPreview(){++previewLoad;previewBusy=false;$('preview-refresh').disabled=false;$('preview-screen').replaceChildren();$('preview-samples').replaceChildren();$('preview-folder').textContent='';$('preview-detail').textContent='Refresh Preview to see your current choices.';}
function previewCells(cells,cursorIndex=-1){
  if(!Array.isArray(cells)||cells.length!==2000||cells.some(cell=>!Array.isArray(cell)||cell.length!==2||cell.some(value=>!Number.isInteger(value)||value<0||value>255)))throw new Error('The preview screen was incomplete. Refresh to try again.');
  const rows=[];for(let row=0;row<25;row++){const line=element('span','','bbs-row');let start=row*80;
    while(start<(row+1)*80){const attr=cells[start][1],cursor=start===cursorIndex;let text='',end=start;while(end<(row+1)*80&&cells[end][1]===attr&&(cursor?end===start:end!==cursorIndex))text+=previewCp437[cells[end++][0]];
      line.append(element('span',text,'ega-fg'+(attr&15)+' ega-bg'+((attr>>4)&15)+(cursor?' terminal-cursor':'')));start=end;}
    rows.push(line);
  }return rows;
}
async function refreshMenuPreview(){
  if(page!=='config'||route!=='general.displays_menus'||!board||previewBusy||saving||!$('board-form').reportValidity()||!$('menu-preview-form').reportValidity())return;
  const generation=++previewLoad,values=formValues();previewBusy=true;$('preview-refresh').disabled=true;$('preview-detail').textContent='Preparing preview...';
  try{const result=await api('/api/config/menu-preview',{values:{...values,artwork:$('preview-artwork').checked},menu:Number($('preview-menu').value),ansi:$('preview-format').value==='ansi',level:Number($('preview-level').value),sysop:$('preview-sysop').checked});
    if(generation!==previewLoad||page!=='config'||route!=='general.displays_menus')return;
    if(result.columns!==80||result.rows!==25)throw new Error('The preview dimensions were invalid. Refresh to try again.');
    const rows=previewCells(result.cells);$('preview-screen').replaceChildren(...rows);$('preview-samples').replaceChildren();
    for(const [text,fg,bg] of [['Default text preview',values.text_color,0],['Caller input preview',values.input_color,0],['1. Selected light-bar item (direct number entry also works)',values.selected_text,values.selected_background]])$('preview-samples').append(element('span',text.padEnd(80,' '),'bbs-row ega-fg'+fg+' ega-bg'+bg));
    $('preview-detail').textContent=result.detail;$('preview-folder').textContent='Display folder on the Linux computer: '+result.folder;
  }catch(error){if(generation===previewLoad){$('preview-screen').replaceChildren();$('preview-samples').replaceChildren();$('preview-detail').textContent=error.message;}}
  finally{if(generation===previewLoad){previewBusy=false;$('preview-refresh').disabled=false;}}
}
$('menu-preview-form').onsubmit=event=>{event.preventDefault();return refreshMenuPreview();};
for(const id of ['preview-menu','preview-format','preview-level','preview-sysop','preview-artwork'])$(id).addEventListener('input',invalidateMenuPreview);

$('open-door-commands').onclick=()=>navigate('config','general.door_commands').catch(e=>notice(e.message));
$('door-command-back').onclick=()=>navigate('config','general.displays_menus').catch(e=>notice(e.message));

async function loadAfterKnownSave(detail){
  try{await loadBoard();$('config-result').textContent=detail;}
  catch(error){if(board){board.can_save=false;board.can_remove=false;board.readback_failed=true;board.acknowledged_detail=detail;}fieldControls();recordControls();catalogControls();$('config-result').textContent=detail+' Saved settings could not be refreshed. Do not repeat the saved action. Use Refresh Saved Settings to read the current state. '+error.message;}
}
$('reload-board').onclick=async()=>{
  if(busy||saving||hasDraft())return;busy=true;lockBoard(true);
  try{await loadBoard();$('config-result').textContent='Saved settings refreshed. No change was sent.';}
  catch(error){if(board){board.can_save=false;board.can_remove=false;board.readback_failed=true;}$('config-result').textContent=(board?.acknowledged_detail?board.acknowledged_detail+' ':'')+'Saved settings are still unavailable. No change was sent. '+error.message;}
  finally{busy=false;lockBoard(false);setDirty(dirty);}
};

function jobControls(){
  compactControls();
  $('job-run').disabled=busy||saving||hasDraft()||!jobsRoute(route)||!board?.can_run||!!board?.readback_failed||jobRunUnconfirmed;
}
function renderJob(current){
  if(!jobsRoute(route))return;const state=current.job_status;
  $('job-title').textContent=current.job_title+' Status';$('job-time-zone').textContent='Schedule and status times use the BBS time zone: '+(current.time_zone||'unavailable');
  $('job-status').textContent=(state.queued?'Queued. ':state.active?'Running. ':state.any_active?'Another job is active. ':'Idle. ')+(state.active?(state.active_scheduled?'Scheduled':'Manual')+' run '+state.active_run+'; accepted '+(state.active_started_local||'time unavailable')+'. ':'')+(state.paused?'Automatic jobs are paused while the BBS is stopped. ':'')+'Last result: '+state.result+'. Next scheduled run: '+(state.next_local||'None')+'. Last run: '+(state.last_local||'Never')+'.';
  $('job-detail').textContent=state.detail||'No completed run details.';
  $('job-holds').textContent=(state.verification_required?'A fresh Message Integrity check is required before retention can remove messages. ':'')+(state.holds?.length?'Integrity holds: '+state.holds.map(h=>'scope '+h.scope+', findings '+h.count).join('; ')+'. ':'')+(state.totals?'Last maintenance totals: '+state.totals.removed_public+' public messages removed; '+state.totals.expired_unread+' unread inbox entries expired; '+state.totals.committed_batches+' committed batches.':'');
  $('job-history').replaceChildren();if(!state.history.length)$('job-history').append(element('p','No runs recorded.'));
  for(const item of state.history.slice(-32).reverse())$('job-history').append(element('p',(item.started_local||'Time unavailable')+' to '+(item.finished_local||'not finished')+' - '+item.result+'. '+item.detail));
  jobControls();
}
function answerJob(accepted){const resolve=jobConfirmation;jobConfirmation=null;$('job-dialog').close();if(resolve)resolve(accepted);}
async function runSelectedJob(){
  if(busy||saving||hasDraft()||!jobsRoute(route)||!board?.can_run||board.readback_failed||jobRunUnconfirmed)return;
  busy=true;lockBoard(true);jobControls();
  try{
    $('job-run-title').textContent='Run '+board.job_title+' now?';
    $('job-run-warning').textContent=recordId===1?'Create one backup in the saved Linux folder. Only verified managed archives are eligible for saved retention.':recordId===2?'Apply saved message retention limits. Eligible old public messages and unread private inbox entries may be removed. Read mail, sender copies and Permanent messages keep their existing protection.':'Check the shared message database. A complete fresh check can release the retention safety hold.';
    if(!await new Promise(resolve=>{jobConfirmation=resolve;$('job-dialog').showModal();}))return;
    // Save a non-secret uncertainty marker before sending. Reload never retries.
    jobRunUnconfirmed=true;setDirty(true);
    const result=await api('/api/config/jobs/run',{record:recordId});jobRunUnconfirmed=false;setDirty(false);
    await loadAfterKnownSave(result.detail||'Job accepted. Check its actual result.');
  }catch(error){board.can_save=false;jobRunUnconfirmed=true;setDirty(true);$('config-result').textContent=error.message+' Run Now will not be repeated. Discard Changes reads current status and history.';}
  finally{busy=false;lockBoard(false);setDirty(dirty);jobControls();}
}
$('job-run').onclick=runSelectedJob;$('job-accept').onclick=()=>answerJob(true);$('job-cancel').onclick=()=>answerJob(false);
$('job-dialog').addEventListener('cancel',event=>{event.preventDefault();answerJob(false);});

function compactMarker(){return draftKey?draftKey+'.compaction':'';}
function retainCompact(before){compactUnknown=before!==null;if(compactUnknown)compactBefore=before;const key=compactMarker();if(key)try{if(compactUnknown)sessionStorage.setItem(key,compactBefore);else sessionStorage.removeItem(key);}catch{}compactControls();}
function compactControls(){
  const enabled=jobsRoute(route)&&!!board?.job_status?.paused&&!board?.job_status?.any_active;
  $('compact-start').disabled=busy||saving||compactBusy||hasDraft()||!enabled||!!compaction?.active||compactUnknown||!!board?.readback_failed||(compaction?.completed&&!compaction.reopened);
  $('compact-refresh').disabled=compactBusy;$('compact-cancel').disabled=compactBusy||!compaction?.active;
  $('compact-review').hidden=!compactUnknown;$('compact-review').disabled=compactBusy||!compactReadOk||!!compaction?.active;
}
async function refreshCompaction(){
  if(compactBusy)return;const generation=++compactRead;compactReadOk=false;
  try{const value=await api('/api/compaction');if(generation!==compactRead)return;
    if(typeof value.operation!=='string'||!/^(0|[1-9][0-9]{0,19})$/.test(value.operation)||typeof value.active!=='boolean'||typeof value.completed!=='boolean')throw new Error('Unexpected compaction status.');
    compaction=value;compactReadOk=true;
    const marker=compactMarker(),before=marker?sessionStorage.getItem(marker):null;if(before!==null&&/^(0|[1-9][0-9]{0,19})$/.test(before)){compactUnknown=true;compactBefore=before;}
    if(compactUnknown&&/^(0|[1-9][0-9]{0,19})$/.test(value.operation)&&BigInt(value.operation)>BigInt(compactBefore))retainCompact(null);
    $('compaction-section').hidden=!(jobsRoute(route)||compactUnknown||value.operation!=='0'||value.active);
    $('compact-detail').textContent=compactUnknown?'The latest request has no confirmed new result. Inspect the existing backup/work and logs; no request was repeated. ':value.operation==='0'?(compactUnknown?'No result is available in this management process. Inspect preserved backup/work and logs before another explicit request. No request was repeated.':'No compaction has been requested in this management process.'):value.stage+': '+value.detail;
    $('compact-evidence').textContent=value.operation==='0'?'':(value.backup_verified?'Verified backup: '+value.backup+'. ':'No verified prerequisite backup reported. ')+(value.completed?'Physical commit: '+(value.physical_commit?'Yes':'No')+'; logical contents verified: '+(value.logical_verified?'Yes':'Not confirmed')+'; management reopened: '+(value.reopened?'Yes':'No')+'; bytes '+value.bytes_before+' to '+value.bytes_after+'. ':'')+(value.temporary?'Work folder: '+value.temporary+'. Empty verified temporary work may have been retired.':'');
  }catch(error){$('compact-detail').textContent='Compaction status unavailable. No request was repeated. '+error.message;}
  finally{compactControls();}return compactReadOk;
}
function answerCompactReview(accepted){const resolve=compactReviewConfirmation;compactReviewConfirmation=null;$('compact-review-dialog').close();if(resolve)resolve(accepted);}
async function reviewCompactRequest(){
  if(compactBusy||!compactUnknown)return;
  if(!await refreshCompaction()||!compactUnknown||compaction.active)return;
  compactBusy=true;compactControls();
  let accepted=false;
  try{accepted=await new Promise(resolve=>{compactReviewConfirmation=resolve;$('compact-review-dialog').showModal();});}
  finally{compactBusy=false;compactControls();}
  if(!accepted||!await refreshCompaction()||!compactUnknown||compaction.active)return;
  retainCompact(null);$('compact-detail').textContent='Review finished. The earlier request is still unconfirmed. Nothing was repeated; any new compaction needs a separate confirmation.';
}
function answerCompact(accepted){const resolve=compactConfirmation;compactConfirmation=null;$('compact-dialog').close();if(resolve)resolve(accepted);}
async function startCompaction(){
  compactControls();if($('compact-start').disabled)return;compactBusy=true;busy=true;lockBoard(true);compactControls();
  try{if(!await new Promise(resolve=>{compactConfirmation=resolve;$('compact-dialog').showModal();}))return;
    retainCompact(compaction?.operation||'0');const result=await api('/api/compaction/start',{});$('compact-detail').textContent=result.detail;
  }catch(error){if(error.response?.operation==='0')retainCompact(null);$('compact-detail').textContent=error.message+' Use Refresh Compaction Status. This request will not be repeated.';}
  finally{compactBusy=false;busy=false;lockBoard(false);setDirty(dirty);compactControls();}
  await refreshCompaction();
}
$('compact-start').onclick=startCompaction;$('compact-accept').onclick=()=>answerCompact(true);$('compact-decline').onclick=()=>answerCompact(false);
$('compact-dialog').addEventListener('cancel',event=>{event.preventDefault();answerCompact(false);});$('compact-refresh').onclick=refreshCompaction;
$('compact-review').onclick=reviewCompactRequest;$('compact-review-accept').onclick=()=>answerCompactReview(true);$('compact-review-decline').onclick=()=>answerCompactReview(false);
$('compact-review-dialog').addEventListener('cancel',event=>{event.preventDefault();answerCompactReview(false);});
$('compact-cancel').onclick=async()=>{if(compactBusy||!compaction?.active)return;compactBusy=true;compactControls();try{const result=await api('/api/compaction/cancel',{operation:compaction.operation});$('compact-detail').textContent=result.detail;}catch(error){$('compact-detail').textContent=error.message+' Refresh the actual result.';}finally{compactBusy=false;compactControls();}await refreshCompaction();};

function offlineMarker(){return draftKey?draftKey+'.maintenance':'';}
function retainOffline(value){offlineUnknown=value;const key=offlineMarker();if(key)try{if(value)sessionStorage.setItem(key,JSON.stringify(value));else sessionStorage.removeItem(key);}catch{}offlineControls();}
function offlineControls(){
  const unavailable=offlineBusy||busy||saving||hasDraft()||bbsRunning||maintenanceActive||!!offline?.active||!!offlineUnknown;
  $('ftn-history-recover').disabled=$('health-run').disabled=$('restore-inspect').disabled=$('contents-inspect').disabled=$('prepare-files').disabled=unavailable;contentsControls();
  $('offline-refresh').disabled=offlineBusy;$('offline-cancel').disabled=offlineBusy||!offline?.active||(offline?.kind==='file_group_deletion'&&offline?.file_deletion?.started);
  for(const id of ['deletion-backup-yes','deletion-backup-no']){$(id).hidden=!offline?.awaiting_backup_choice;$(id).disabled=offlineBusy||!offlineReadOk||!!offlineUnknown;}
  $('file-delete-mode').hidden=offline?.kind!=='file_group_deletion'||!offline?.awaiting_confirmation;$('file-delete-disk').disabled=offlineBusy||!offlineReadOk||!!offlineUnknown||!!offline?.file_deletion?.resume;
  $('offline-confirm').textContent=offline?.kind==='ftn_history'?'Review FTN History Recovery...':offline?.kind==='file_group_deletion'?'Review File Group Deletion...':offline?.kind==='message_deletion'?(offline.deletion_reconcile?'Review Exact Reconciliation...':'Review Final Deletion...'):'Review Restore...';
  $('offline-confirm').hidden=!offline?.awaiting_confirmation;$('offline-confirm').disabled=offlineBusy||!offlineReadOk||!!offlineUnknown;
  $('offline-review').hidden=!offlineUnknown;$('offline-review').disabled=offlineBusy||!offlineReadOk||!!offline?.active;
}
async function refreshOffline(){
  if(offlineBusy)return false;const generation=++offlineRead;offlineReadOk=false;
  try{const value=await api('/api/maintenance');if(generation!==offlineRead)return false;
    if(typeof value.operation!=='string'||!/^(0|[1-9][0-9]{0,19})$/.test(value.operation)||typeof value.active!=='boolean'||typeof value.awaiting_confirmation!=='boolean'||!Array.isArray(value.findings))throw new Error('Unexpected maintenance status.');
    const priorFileOperation=offline?.kind==='file_group_deletion'?offline.operation:null;offline=value;offlineReadOk=true;renderContentsRestore(value);if(value.kind==='file_group_deletion'&&(priorFileOperation!==value.operation||value.file_deletion.resume))$('file-delete-disk').checked=!!value.file_deletion.delete_disk_files;const key=offlineMarker();if(key)try{const marker=JSON.parse(sessionStorage.getItem(key));if(marker&&/^(0|[1-9][0-9]{0,19})$/.test(marker.before)&&['start','confirm','deletion_backup','deletion_confirm','file_deletion_confirm','contents_select'].includes(marker.phase))offlineUnknown=marker;}catch{}
    if(offlineUnknown&&(BigInt(value.operation)>BigInt(offlineUnknown.before)||(value.operation===offlineUnknown.before&&(((offlineUnknown.phase==='confirm'||offlineUnknown.phase==='file_deletion_confirm')&&!value.awaiting_confirmation)||(offlineUnknown.phase==='contents_select'&&!value.contents?.awaiting_selection)||(offlineUnknown.phase==='deletion_backup'&&!value.awaiting_backup_choice)||(offlineUnknown.phase==='deletion_confirm'&&(!value.awaiting_confirmation||value.deletion_reconcile!==offlineUnknown.reconcile))))))retainOffline(null);
    $('offline-section').hidden=!(offlineRoute(route)||value.operation!=='0'||offlineUnknown);
    $('offline-detail').textContent=offlineUnknown?'The last request has no confirmed new result. Check the saved report, backup/work and logs. Nothing was repeated. Refresh or cancel an active preview; only finish review after checking the actual files.':value.operation==='0'?'No maintenance has been requested in this management process.':value.stage+': '+value.detail;
    const parts=[];if(value.archive)parts.push('Selected backup: '+value.archive+'; verified preview files: '+value.archive_files+'.');if(value.report)parts.push('Saved report: '+value.report+'.');
    if(value.completed)parts.push('Board ready: '+(value.ready?'Yes':'No')+'; management reopened: '+(value.management_reopened?'Yes':'No')+'.');
    if(value.kind==='restore'&&value.completed){parts.push('Core files changed: '+(value.live_state_changed?'Yes':'No')+'; rollback: '+(!value.rollback_attempted?'Not attempted':value.rollback_succeeded?'Completed':'Incomplete')+'.');for(const [key,label] of [['pre_restore_archive','Pre-restore backup'],['previous_state','Previous state'],['failed_state','Failed state']])if(value[key])parts.push(label+': '+value[key]+'.');if(value.file_prepare_required)parts.push('Explicit Prepare Copied Files is required before starting the BBS.');}
    if(value.kind==='file_group_deletion'){const d=value.file_deletion;parts.push('File Group: '+d.name+' (#'+d.id+'). Areas: '+d.areas+'; listings: '+d.listings+'; disk files counted: '+d.disk_files+'. '+d.preview_detail+(d.resume?' Saved deletion choice: '+(d.delete_disk_files?'delete files on disk.':'keep files on disk.'):'')+(value.completed?' Completed successfully: '+(value.success?'Yes':'No')+'.':''));}
    if(value.kind==='message_deletion'){const d=value.deletion;parts.push('Selected '+d.kind+': '+d.name+' (#'+d.id+'). Echoes: '+d.echoes+'; messages: '+d.messages+'; Permanent messages INCLUDED: '+d.permanent+'.');if(value.deletion_backup)parts.push('Backup: '+value.deletion_backup+'; verified archive: '+(value.backup_verified?'Yes':'No')+'.');if(value.completed)parts.push('Deletion reported committed: '+(d.committed?'Yes':'No')+'.');}
    $('offline-evidence').textContent=parts.join(' ');$('health-findings').replaceChildren();for(const finding of value.findings){const section=element('section','','health-finding');section.append(element('h3',finding.state+' - '+finding.subject),element('p',finding.detail));if(finding.next)section.append(element('p','Next: '+finding.next));$('health-findings').append(section);}
  }catch(error){$('offline-detail').textContent='Maintenance status unavailable. Nothing was repeated. '+error.message;if(error.status===401)notice('Management access ended. After Restore, open the launcher again to review the result. The BBS remains stopped.');}
  finally{offlineControls();catalogControls();fileCatalogControls();fieldControls();}return offlineReadOk;
}
function offlineAnswer(accepted){const resolve=offlineDialog;offlineDialog=null;$('offline-dialog').close();if(resolve)resolve(accepted);}
function offlineAsk(title,detail,warning,accept){$('offline-dialog-title').textContent=title;$('offline-dialog-detail').textContent=detail;$('offline-dialog-warning').textContent=warning;$('offline-accept').textContent=accept;return new Promise(resolve=>{offlineDialog=resolve;$('offline-dialog').showModal();});}
async function startOffline(kind){
  offlineControls();const button=kind==='ftn_history'?'ftn-history-recover':kind==='health'?'health-run':kind==='prepare'?'prepare-files':kind==='file_contents'?'contents-inspect':'restore-inspect';if($(button).disabled)return;
  const hasArchive=kind==='restore'||kind==='file_contents',archive=hasArchive?$('restore-path').value:'';
  if(hasArchive&&(!archive.startsWith('/')||archive.length>4096||archive.includes('\0'))){$('offline-section').hidden=false;$('offline-detail').textContent='Enter the absolute path of a private backup on the Linux computer.';return;}
  offlineBusy=true;offlineControls();
  try{
    if(kind==='prepare'&&!await offlineAsk('Prepare copied file records?','This checks the copied/restored board and rebinds only matching file records. Missing or changed files remain unresolved.','It does not repair an interrupted compaction, restore data or start the BBS.','Prepare Files'))return;
    retainOffline({before:offline?.operation||'0',phase:'start'});const reply=await api('/api/maintenance/start',{kind,archive});$('offline-detail').textContent=reply.detail;
  }catch(error){if(error.response?.operation==='0')retainOffline(null);$('offline-detail').textContent=error.message+' Refresh the actual result; no request will be repeated.';}
  finally{offlineBusy=false;offlineControls();}
  await refreshOffline();
}
async function confirmOfflineRestore(){
  if(offlineBusy||offlineUnknown||!offline?.awaiting_confirmation||!offlineReadOk)return;
  const previous=offline;offlineBusy=true;offlineControls();
  try{
    const history=previous.kind==='ftn_history';const contents=previous.kind==='file_contents';const destinations=(previous.contents?.destinations||[]).map(d=>'Area '+d.area+' → '+d.path+'; missing '+d.missing+', identical '+d.identical+', conflicts '+d.conflicts).join('\n');
    if(!history&&!await offlineAsk(contents?'Restore missing file contents?':'Restore this verified core backup?',contents?previous.archive+'\n'+destinations:previous.archive+' ('+previous.archive_files+' verified files). Current core files will be replaced; previous state is retained. Live Files and durable intake are separate.',contents?'Only missing files in these selected areas will be restored. Existing files stay untouched; conflicts are skipped. Core settings and catalog identities do not change. Use File Manager Inspect afterward. The BBS remains stopped.':'All browser sessions will end before publication. Open the launcher again to review the result. The BBS stays stopped. Closing this browser after confirmation does not cancel Restore.',contents?'Restore Missing Files':'Restore Verified Core'))return;
    if(history&&!await offlineAsk('Recover FTN History?', 'Current UTC seconds: '+previous.ftn_history.utc+'. Verified pending outbound files: '+previous.ftn_history.pending_outbound+'; incoming files: '+previous.ftn_history.pending_ingress+'. Separate held outbound items: '+previous.ftn_history.held_outbound+'; incoming items: '+previous.ftn_history.held_ingress+'. Stop any independently launched BinkD. The BBS remains stopped.', 'This restored BBS will replace the previous copy. Any other copy using these FTN addresses is stopped.', 'Recover FTN History'))return;
    retainOffline({before:previous.operation,phase:'confirm'});const reply=await api('/api/maintenance/confirm',{operation:previous.operation,confirmation:previous.confirmation});$('offline-detail').textContent=reply.detail;
  }catch(error){$('offline-detail').textContent=error.message+' No confirmation will be repeated. Open the launcher again and read the actual result.';}
  finally{offlineBusy=false;offlineControls();}
  await refreshOffline();
}
$('ftn-history-recover').onclick=()=>startOffline('ftn_history');$('health-run').onclick=()=>startOffline('health');$('prepare-files').onclick=()=>startOffline('prepare');$('restore-inspect').onclick=()=>startOffline('restore');$('contents-inspect').onclick=()=>startOffline('file_contents');$('backup-settings').onclick=()=>navigate('config','tools.backup_schedule');
$('offline-refresh').onclick=refreshOffline;$('offline-confirm').onclick=()=>offline?.kind==='file_group_deletion'?confirmFileGroupDeletion():offline?.kind==='message_deletion'?confirmMessageDeletion():confirmOfflineRestore();$('offline-accept').onclick=()=>offlineAnswer(true);$('offline-decline').onclick=()=>offlineAnswer(false);$('offline-dialog').addEventListener('cancel',event=>{event.preventDefault();offlineAnswer(false);});
$('offline-cancel').onclick=async()=>{if(offlineBusy||!offline?.active)return;offlineBusy=true;offlineControls();try{const reply=await api('/api/maintenance/cancel',{operation:offline.operation});$('offline-detail').textContent=reply.detail;}catch(error){$('offline-detail').textContent=error.message+' Refresh the actual result.';}finally{offlineBusy=false;offlineControls();}await refreshOffline();};
$('offline-review').onclick=async()=>{if(offlineBusy||!offlineUnknown||!await refreshOffline()||offline?.active)return;retainOffline(null);$('offline-detail').textContent='Review finished. The earlier request remains unconfirmed. Nothing was repeated; a new operation requires a separate request.';};
$('catalog-groups').onclick=()=>catalogBrowse().catch(e=>notice(e.message));
$('catalog-next').onclick=()=>{if(catalogState.data?.next&&!busy&&!mainPending)return catalogBrowse(catalogState.data.next).catch(e=>notice(e.message));};
$('catalog-new-group').onclick=()=>catalogTransition(()=>{renderCatalogEditor({id:'0',revision:'0',name:'',order:'0',access:1,enabled:true},'group');setDirty(true);});
$('catalog-new-echo').onclick=()=>catalogTransition(()=>{if(!catalogState.query.group)return;renderCatalogEditor({id:'0',revision:'0',group:catalogState.query.group,number:0,name:'',order:'0',access:1,reading:1,posting:1,enabled:true,pipe_colors:false,maximum_count:1000,maximum_age:null},'echo');setDirty(true);});
$('catalog-clone').onclick=()=>navigate('config','messages.clone_permissions');
async function startMessageDeletion(){
  catalogControls();if($('catalog-delete').disabled||offlineBusy)return;const selected=catalogState.entry,kind=catalogState.action;
  offlineBusy=true;offlineControls();catalogControls();try{
    retainOffline({before:offline?.operation||'0',phase:'start'});const reply=await api('/api/config/messages/delete-preview',{kind,id:selected.id,revision:selected.revision});$('offline-detail').textContent=reply.detail;
  }catch(error){if(error.response?.operation==='0')retainOffline(null);$('offline-section').hidden=false;$('offline-detail').textContent=error.message+' No deletion was confirmed. Refresh the actual preview result.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
async function chooseMessageDeletionBackup(backup){
  if(offlineBusy||offlineUnknown||!offlineReadOk||!offline?.awaiting_backup_choice||offline.kind!=='message_deletion')return;const previous=offline;
  offlineBusy=true;offlineControls();try{
    if(!backup&&!await offlineAsk('Skip the deletion backup?','No new backup will be made for '+previous.deletion.name+'.','The next step still asks for separate final deletion confirmation. Cancel keeps the current preview.','Skip Backup'))return;
    retainOffline({before:previous.operation,phase:'deletion_backup'});const reply=await api('/api/maintenance/deletion-backup',{operation:previous.operation,confirmation:previous.confirmation,backup});$('offline-detail').textContent=reply.detail;
  }catch(error){if(error.response?.accepted===false)retainOffline(null);$('offline-detail').textContent=error.message+' Read the actual result. The backup choice will not be repeated.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
async function confirmMessageDeletion(){
  if(offlineBusy||offlineUnknown||!offlineReadOk||!offline?.awaiting_confirmation||offline.kind!=='message_deletion')return;const previous=offline,d=previous.deletion;
  offlineBusy=true;offlineControls();try{
    const detail=d.kind+' '+d.name+' (#'+d.id+'). Echoes: '+d.echoes+'; messages: '+d.messages+'; Permanent messages INCLUDED: '+d.permanent+'. '+(previous.backup_verified?'Verified backup: '+previous.deletion_backup:'No verified backup was requested for this attempt.');
    if(!await offlineAsk(previous.deletion_reconcile?'Reconcile this exact deletion?':'Permanently delete this area and its messages?',detail,'Other areas, private mail, accounts and existing backups remain. Recovery requires a supported backup restore. This is not Disable or secure erasure. Closing the browser after confirmation does not cancel deletion.',previous.deletion_reconcile?'Reconcile Exact Operation':'Delete Confirmed Area'))return;
    retainOffline({before:previous.operation,phase:'deletion_confirm',reconcile:!!previous.deletion_reconcile});const reply=await api('/api/maintenance/deletion-confirm',{operation:previous.operation,confirmation:previous.confirmation});$('offline-detail').textContent=reply.detail;
  }catch(error){if(error.response?.accepted===false)retainOffline(null);$('offline-detail').textContent=error.message+' No confirmation will be repeated. Read the actual result.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
$('catalog-delete').onclick=startMessageDeletion;$('deletion-backup-yes').onclick=()=>chooseMessageDeletionBackup(true);$('deletion-backup-no').onclick=()=>chooseMessageDeletionBackup(false);
$('catalog-clear-targets').onclick=()=>{if(busy||mainPending||board?.readback_failed)return;catalogState.targets=[];setDirty(true);renderCloneSelection();};
$('catalog-insert').onclick=()=>catalogTransition(()=>{const selected=catalogState.entry;if(!selected||catalogState.action!=='echo'||selected.id==='0')return;renderCatalogEditor({...selected,id:'0',revision:'0',number:0,name:'',before:selected.id,before_revision:selected.revision},'insert',board.revision,board.operation);setDirty(true);});
for(const direction of ['up','down'])$('catalog-'+direction).onclick=async()=>{if(busy||dirty||mainPending||board?.readback_failed||catalogState.action!=='echo'||!catalogState.entry)return;const selected=catalogState.entry;catalogState.action=direction;catalogState.entry={id:selected.id,revision:selected.revision};setDirty(true);const result=await saveBoard();if(!result&&!mainPending){catalogState.entry=selected;catalogState.action='echo';setDirty(false);}catalogControls();};

$('file-catalog-groups').onclick=()=>fileCatalogBrowse().catch(e=>notice(e.message));
$('file-catalog-next').onclick=()=>{if(fileCatalogState.data?.next&&!fileCatalogLocked())return fileCatalogBrowse(fileCatalogState.data.next).catch(e=>notice(e.message));};
$('file-catalog-new-group').onclick=()=>fileCatalogTransition(()=>{renderFileCatalogEditor({id:'0',revision:'1',name:'',order:'0',access:1,enabled:true},'group');setDirty(true);});
function newFileCatalogArea(){return {id:'0',revision:'1',group:fileCatalogState.query.group,number:0,kind:0,name:'',description:'',order:'0',access:1,upload:1,download:1,uploads_enabled:false,downloads_sysop_only:false,enabled:true,own_description_edit:false,backup_contents:false,ratio_uploads:true,ratio_downloads:true,sysop_review:false,maximum_upload_bytes:null,storage_path:''};}
$('file-catalog-new-area').onclick=()=>fileCatalogTransition(()=>{if(!fileCatalogState.query.group)return;renderFileCatalogEditor(newFileCatalogArea(),'area');setDirty(true);});
for(const direction of ['before','after'])$('file-catalog-'+direction).onclick=()=>fileCatalogTransition(()=>{const selected=fileCatalogState.entry;if(!selected||fileCatalogState.action!=='area'||selected.id==='0')return;renderFileCatalogEditor({...newFileCatalogArea(),group:selected.group,anchor:selected.id,anchor_revision:selected.revision},'insert_'+direction,board.revision,board.operation);setDirty(true);});
for(const direction of ['up','down'])$('file-catalog-'+direction).onclick=async()=>{if(fileCatalogLocked()||dirty||fileCatalogState.action!=='area'||!fileCatalogState.entry)return;const selected=fileCatalogState.entry;fileCatalogState.action=direction;setDirty(true);const result=await saveBoard();if(!result&&!mainPending){fileCatalogState.entry=selected;fileCatalogState.action='area';setDirty(false);}fileCatalogControls();};

async function startFileGroupDeletion(){
  fileCatalogControls();if($('file-catalog-delete').disabled||offlineBusy)return;const selected=fileCatalogState.entry;
  offlineBusy=true;offlineControls();fileCatalogControls();try{retainOffline({before:offline?.operation||'0',phase:'start'});const reply=await api('/api/config/files/delete-preview',{id:selected.id,revision:selected.revision,catalog_revision:board.revision});$('offline-detail').textContent=reply.detail;}
  catch(error){if(error.response?.operation==='0')retainOffline(null);$('offline-section').hidden=false;$('offline-detail').textContent=error.message+' No deletion was confirmed. Refresh the actual result.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
async function confirmFileGroupDeletion(){
  if(offlineBusy||offlineUnknown||!offlineReadOk||!offline?.awaiting_confirmation||offline.kind!=='file_group_deletion')return;const previous=offline,d=previous.file_deletion,disk=d.resume?d.delete_disk_files:$('file-delete-disk').checked;
  offlineBusy=true;offlineControls();try{
    const detail=d.name+' (#'+d.id+'). Areas: '+d.areas+'; listings: '+d.listings+'; disk files counted: '+d.disk_files+'. '+d.preview_detail+' '+(disk?'Reviewed files will be permanently deleted from disk.':'Files on disk will be kept.');
    if(!await offlineAsk(d.resume?'Continue the saved File Group deletion?':'Delete this File Group and its Areas?',detail,'Group, Area definitions and listings are removed. Permanent numbers and internal history are retained; empty folders remain. '+(d.resume?'Earlier removals cannot be undone. ':'')+'After confirmation, browser closure or Cancel cannot undo deletion. Review any incomplete result before continuing.',d.resume?'Continue Saved Deletion':'Delete Confirmed Group'))return;
    retainOffline({before:previous.operation,phase:'file_deletion_confirm'});const reply=await api('/api/maintenance/file-deletion-confirm',{operation:previous.operation,confirmation:previous.confirmation,delete_disk_files:disk});$('offline-detail').textContent=reply.detail;
  }catch(error){if(error.response?.accepted===false)retainOffline(null);$('offline-detail').textContent=error.message+' Nothing will be repeated automatically. Read the actual result.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
$('file-catalog-delete').onclick=startFileGroupDeletion;

let terminalState=null,terminalBusy=false,terminalPolling=false,terminalSending=false,terminalBlocked=false,terminalQueue=[];
function terminalControls(){for(const id of ['terminal-enter','terminal-escape','terminal-backspace','terminal-line','terminal-line-send'])$(id).disabled=!terminalState||terminalState.connecting||terminalBlocked||terminalBusy;$('terminal-close').disabled=!terminalState||terminalBusy;$('terminal-refresh').disabled=terminalBusy;}
function clearNodeTerminal(){terminalState=null;terminalQueue=[];terminalBlocked=false;$('terminal-line').value='';$('terminal-screen').replaceChildren();$('node-terminal').hidden=true;terminalControls();}
async function refreshNodeTerminal(review=false){
  if(!authenticated||maintenanceActive||terminalPolling)return false;terminalPolling=true;
  try{const result=await api('/api/node-terminal');
    if(!result.active){clearNodeTerminal();return true;}
    if(!/^[a-f0-9]{64}$/.test(result.handle)||!/^\d+$/.test(result.sequence)||result.columns!==80||result.rows!==25||!Number.isInteger(result.cursor_column)||result.cursor_column<0||result.cursor_column>=80||!Number.isInteger(result.cursor_row)||result.cursor_row<0||result.cursor_row>=25)throw new Error('The terminal response was incomplete.');
    const changed=terminalState?.handle!==result.handle,serverSequence=result.sequence;
    if(changed){terminalQueue=[];terminalState=result;terminalBlocked=false;}
    else{const sequence=terminalState.sequence;terminalState=result;if(terminalSending||terminalQueue.length)terminalState.sequence=sequence;}
    if(review&&!terminalSending){terminalQueue=[];terminalBlocked=false;terminalState.sequence=serverSequence;}
    $('node-terminal').hidden=false;$('terminal-title').textContent='Node '+result.node+' Logon';$('terminal-screen').replaceChildren(...previewCells(result.cells,result.cursor_row*80+result.cursor_column));
    if(!terminalBlocked)$('terminal-detail').textContent=result.connecting?'Connecting to the selected node…':'Connected. Click the screen to type.';
    terminalControls();return true;
  }catch(error){terminalBlocked=true;terminalQueue=[];$('terminal-detail').textContent=error.message+' Input is paused; use Refresh Terminal before typing again.';terminalControls();if(error.status===401)lockWebAccess();return false;}
  finally{terminalPolling=false;}
}
async function openNodeTerminal(node){
  if(terminalBusy||terminalState)return;terminalBusy=true;terminalControls();
  try{await api('/api/node-terminal/open',{node});await refreshNodeTerminal();$('terminal-screen').focus?.();}
  catch(error){notice(error.message+' Checking the current terminal; no second logon will be attempted.');await refreshNodeTerminal();}
  finally{terminalBusy=false;terminalControls();}
}
function terminalEncode(text){const result=[];for(const character of text){const code=character.codePointAt(0),byte=character==='\n'?13:code>=32&&code<=126?code:previewCp437.indexOf(character);if(byte<0)throw new Error('This BBS uses CP437 characters. The text was not sent.');result.push(byte);}if(result.length>256)throw new Error('Send at most 256 characters at once.');return result;}
function queueTerminalInput(bytes){
  if(!terminalState||terminalState.connecting||terminalBlocked||terminalBusy)return;
  if(terminalQueue.length+bytes.length>256){$('terminal-detail').textContent='Typing is arriving too quickly. Wait before entering more.';return;}
  terminalQueue.push(...bytes);void flushTerminalInput();
}
async function flushTerminalInput(){
  if(terminalSending||terminalBlocked||!terminalState||!terminalQueue.length)return;
  const handle=terminalState.handle,sequence=terminalState.sequence,bytes=terminalQueue.splice(0,256);terminalSending=true;
  try{await api('/api/node-terminal/input',{handle,sequence,bytes});if(terminalState?.handle===handle)terminalState.sequence=(BigInt(sequence)+1n).toString();}
  catch(error){terminalQueue=[];terminalBlocked=true;$('terminal-detail').textContent=error.message+' Input will not be resent. Use Refresh Terminal before continuing.';}
  finally{bytes.fill(0);terminalSending=false;terminalControls();}
  if(terminalQueue.length&&!terminalBlocked)void flushTerminalInput();
}
function terminalKeyBytes(event){
  if(event.metaKey||event.altKey||event.isComposing)return;
  const keys={Enter:[13],Backspace:[8],Tab:[9],Escape:[27],ArrowUp:[27,91,65],ArrowDown:[27,91,66],ArrowRight:[27,91,67],ArrowLeft:[27,91,68],Home:[27,91,72],End:[27,91,70],Delete:[27,91,51,126],Insert:[27,91,50,126],PageUp:[27,91,53,126],PageDown:[27,91,54,126]};
  let bytes=keys[event.key];
  if(event.ctrlKey&&/^[a-z]$/i.test(event.key))bytes=[event.key.toUpperCase().charCodeAt(0)-64];
  else if(!event.ctrlKey&&event.key.length===1){try{bytes=terminalEncode(event.key);}catch(error){$('terminal-detail').textContent=error.message;}}
  return bytes;
}
$('terminal-screen').addEventListener('keydown',event=>{
  const bytes=terminalKeyBytes(event);if(bytes){event.preventDefault();queueTerminalInput(bytes);}
});
$('terminal-screen').addEventListener('paste',event=>{event.preventDefault();try{queueTerminalInput(terminalEncode(event.clipboardData.getData('text').replace(/\r\n/g,'\n').replace(/\r/g,'\n')));}catch(error){$('terminal-detail').textContent=error.message;}});
$('terminal-line-form').onsubmit=event=>{event.preventDefault();const field=$('terminal-line');try{queueTerminalInput([...terminalEncode(field.value),13]);}catch(error){$('terminal-detail').textContent=error.message;}finally{field.value='';}};
$('terminal-enter').onclick=()=>queueTerminalInput([13]);$('terminal-escape').onclick=()=>queueTerminalInput([27]);$('terminal-backspace').onclick=()=>queueTerminalInput([8]);
$('terminal-refresh').onclick=()=>refreshNodeTerminal(true);
$('terminal-close').onclick=async()=>{if(!terminalState||terminalBusy)return;const handle=terminalState.handle;terminalBusy=true;terminalQueue=[];terminalControls();try{await api('/api/node-terminal/close',{handle});clearNodeTerminal();}catch(error){$('terminal-detail').textContent=error.message;}finally{terminalBusy=false;await refreshNodeTerminal();terminalControls();}};
setInterval(()=>{if(terminalState&&!terminalBusy)void refreshNodeTerminal();},250);
window.addEventListener('pagehide',()=>{terminalQueue=[];$('terminal-line').value='';});

function ftnRoute(value){return value==='ftn.networks';}
let ftnState={draft:null,view:null,section:'networks',network:'',record:'',offset:'0',search:'',unsent:false,unconfirmed:false,restore:null},ftnBusy=false,ftnAnswer=null;
let ftnRenewAt=0,ftnRenewing=false;
async function ftnRenew(now=Date.now()){
  if(page!=='config'||route!=='ftn.networks'||document.visibilityState==='hidden'||ftnBusy||ftnRenewing||!ftnState.draft||ftnState.unavailable||now<ftnRenewAt)return;
  const handle=ftnState.draft.draft;ftnRenewAt=now+60000;ftnRenewing=true;
  try{await api('/api/ftn/state?draft='+handle);}
  catch(error){if(ftnState.draft?.draft!==handle)return;if(error.status===400){ftnRemember();ftnState.unavailable=true;ftnState.unconfirmed=true;ftnClearPasswords();$('ftn-result').textContent='This FTN draft expired or the web host restarted. Your ordinary form text is still shown and retained in this tab. Copy anything needed before Discard FTN Draft reads saved settings. No edits or save were repeated.';ftnControls();}}
  finally{ftnRenewing=false;}
}
const ftnNames={mode:'Mailer',external_executable:'External BinkD program',listener_enabled:'Enable classic BinkP listener',bind_address:'Listen address',port:'Port',allow_direct_local_netmail:'Allow direct local NetMail',tls_listener_enabled:'Enable TLS listener',tls_port:'TLS port',tls_certificate:'TLS certificate file',tls_private_key:'TLS private key file',name:'Name',description:'Description',address:'Assigned FTN address',additional_addresses:'Additional addresses (one per line)',origin_line:'Origin line',echo_list_url:'EchoList URL',echo_list_source:'EchoList source',echo_list_file:'Local EchoList file',nodelist_file:'Optional nodelist file',enabled:'Enabled',keep_processed:'Keep processed originals',retention_days:'Days to keep processed originals (0 = Forever)',message_group:'Message Group',new_group_name:'Name for a new Message Group',msg_netmail_enabled:'Scan this network’s .MSG NetMail folder',msg_netmail_directory:'.MSG NetMail folder',default_link:'Default NetMail hub',host:'Host name or IP address',use:'Connection direction',poll_minutes:'Poll every (minutes; 0 = off)',tls:'Use TLS for outgoing sessions',allow_legacy_plain_auth:'Allow legacy plain authentication',forward_missing_echoes:'Ask an upstream provider for missing echoes',areafix:'AreaFix management',tls_server_name:'TLS server name',tls_ca_file:'TLS CA file',forward_via_link:'Upstream provider',contact_on_mail:'Contact when mail is ready',tag:'Echo tag',local_echo:'Local conference',store_locally:'Store messages in a local conference',require_real_name:'Require Real Name on outgoing messages',access_level:'Access level',read_level:'Read level',post_level:'Post level',local_name:'Caller-facing conference name',update_local_name:'Update the existing conference name',pipe_colors:'Enable pipe colors',update_local_permissions:'Update existing conference permissions and pipe colors',maximum_messages:'Maximum messages (blank = Unlimited)',maximum_age_days:'Maximum age in days (blank = Unlimited)',update_local_retention:'Update existing conference retention',ansi_colors:'ANSI in SFX-authored outgoing posts',file_area:'Local file area',destination:'Destination FTN address',via_link:'Send via connection',flavor:'Mail policy'};
const ftnChoices={mode:[[0,'Native SFX'],[1,'External BinkD'],[2,'Disabled — retain mail']],use:[[0,'Incoming only'],[1,'Outgoing only'],[2,'Incoming and outgoing']],echo_list_source:[[0,'HTTP(S) URL'],[1,'Local file']],areafix:[[0,'Keep legacy password-based choice'],[1,'Enabled'],[2,'Disabled']],ansi_colors:[['','Keep legacy choice'],['false','Off'],['true','On']],update_local_retention:[[0,'Leave existing limits unchanged'],[1,'Update message count only'],[2,'Update maximum age only'],[3,'Update both limits']],flavor:[[0,'Normal'],[1,'Hold'],[2,'Crash']]};
Object.assign(ftnNames,{copy_count:'Copy maximum message count',copy_age:'Copy maximum message age',copy_access:'Copy access level',copy_read:'Copy read level',copy_post:'Copy post level'});
function ftnPending(){return !!ftnState.draft&&(!!ftnState.setup||ftnState.unsent||ftnState.unconfirmed||(!ftnState.draft.result?.saved&&(ftnState.draft.edit_revision!=='1'||ftnState.draft.state!=='editing')));}
function ftnClearPasswords(){for(const input of $('ftn-passwords').querySelectorAll('input')){if(input.type==='password'){input.value='';input.dataset.changed='false';}} }
function ftnPermissionDraft(){if(ftnState.section!=='permissions')return null;return {selected:[...$('ftn-list').querySelectorAll('input')].filter(i=>i.checked).map(i=>({tag:i.dataset.tag,file_echo:i.dataset.file==='true'})),receive:!!$('ftn-permission-receive')?.checked,send:!!$('ftn-permission-send')?.checked,manage:!!$('ftn-permission-manage')?.checked};}
function ftnRemember(){
  if(!draftKey||!ftnState.draft||ftnState.unavailable)return;
  try{const values={};for(const key of Object.keys(ftnState.view?.values||{})){const field=$('ftn-field-'+key);if(field)values[key]=field.type==='checkbox'?field.checked:field.value;}
    sessionStorage.setItem(draftKey+'-ftn',JSON.stringify({version:1,draft:ftnState.draft.draft,section:ftnState.section,network:ftnState.network,record:ftnState.record,offset:ftnState.offset,search:ftnState.search,edit_revision:ftnState.draft.edit_revision,unsent:ftnState.unsent,unconfirmed:ftnState.unconfirmed,values,cloneTargets:ftnState.cloneTargets||[],importDraft:ftnImportDraft(),setup:ftnSetupRetained(),permissions:ftnPermissionDraft(),code:$('ftn-code')?.value||''}));
  }catch{notice('This tab could not retain its FTN draft reference. Keep the page open until you save or discard.');}
}
function ftnRetained(){try{const raw=draftKey&&sessionStorage.getItem(draftKey+'-ftn');if(!raw||raw.length>3*1024*1024)return null;const saved=JSON.parse(raw);if(saved.version!==1||!/^[a-f0-9]{32}$/.test(saved.draft)||!['networks','settings','links','echoes','files','routes','permissions','clone','import','setup','held'].includes(saved.section)||typeof saved.network!=='string'||saved.network.length>48||typeof saved.record!=='string'||saved.record.length>128||typeof saved.search!=='string'||saved.search.length>128||!/^\d{1,5}$/.test(saved.offset)||!/^\d{1,20}$/.test(saved.edit_revision))return null;if(saved.setup){saved.setup=ftnSetupSanitize(saved.setup);if(!saved.setup)return null;}if(saved.section==='setup'&&!saved.setup)return null;return saved;}catch{return null;}}
function ftnModeControl(control,native){control.dataset.ftnModeInactive=String(!native);control.disabled=!native;return control;}
function ftnControls(){
  configFtnPresentation();
  const locked=ftnBusy||busy||saving||offlineBusy||maintenanceActive,editing=ftnState.draft?.state==='editing'&&!ftnState.unconfirmed;
  for(const button of $('ftn-section').querySelectorAll('button'))button.disabled=locked;
  $('ftn-apply').hidden=['setup','held'].includes(ftnState.section);$('ftn-revert').hidden=['setup','held'].includes(ftnState.section);$('ftn-save').hidden=!!ftnState.setup||ftnState.section==='held';$('ftn-check').hidden=$('ftn-discard').hidden=ftnState.section==='held';
  $('ftn-apply').textContent=ftnState.section==='clone'?'Preview Clone...':ftnState.section==='import'?'Review Selected Import...':'Apply to Draft';
  for(const control of $('ftn-form').querySelectorAll('input,select,textarea'))control.disabled=locked||!editing||control.dataset.ftnModeInactive==='true';
  if(ftnState.section==='settings'&&$('ftn-field-mode')){const mode=Number($('ftn-field-mode').value);for(const key of ['listener_enabled','bind_address','port','tls_listener_enabled','tls_port','tls_certificate','tls_private_key'])if($('ftn-field-'+key))$('ftn-field-'+key).disabled=locked||!editing||mode!==0;if($('ftn-field-external_executable'))$('ftn-field-external_executable').disabled=locked||!editing||mode!==1;}
  if(ftnState.section==='setup')for(const control of [...$('ftn-navigation').querySelectorAll('button'),...$('ftn-fields').querySelectorAll('button')])control.disabled=locked||!editing;
  if(ftnState.section==='clone')for(const [key,mask] of [['maximum_messages','count'],['maximum_age_days','age'],['access_level','access'],['read_level','read'],['post_level','post']])if($('ftn-field-'+key))$('ftn-field-'+key).disabled=locked||!editing||!$('ftn-field-copy_'+mask)?.checked;
  for(const id of ['ftn-apply','ftn-remove','ftn-new'])$(id).disabled=locked||!editing;
  $('ftn-previous').disabled=locked||ftnState.offset==='0';$('ftn-next').disabled=locked||(ftnState.section==='import'?Number(ftnState.offset)+25>=ftnImportRows().length:!ftnState.view?.next);
  $('ftn-remove').hidden=!ftnState.record||['settings','permissions','setup','held'].includes(ftnState.section);
  $('ftn-new').hidden=['settings','permissions','clone','import','setup','held'].includes(ftnState.section);
  $('ftn-save').disabled=locked||!editing||(!ftnState.unsent&&ftnState.draft?.edit_revision==='1');
  $('ftn-check').disabled=locked||!ftnState.draft||!!ftnState.unavailable;$('ftn-discard').textContent=ftnState.draft?.result?.saved?'Open Saved Settings':'Discard FTN Draft...';$('ftn-revert').disabled=locked||!editing||!ftnState.unsent;
  if(ftnState.section==='import'){const d=ftnImportData();$('ftn-import-load').disabled=locked||!editing||d.loading;$('ftn-save').disabled=locked||!editing||d.loading;$('ftn-apply').disabled=locked||!editing||d.loading||!d.list;const cancel=$('ftn-import-cancel');if(cancel)cancel.disabled=locked||!d.loading;}
  if(ftnState.section==='held')ftnHeldControls(locked);
  $('save-state').textContent=ftnState.unconfirmed?'Check the last request':ftnState.draft?.result?.saved?'FTN settings saved':ftnState.draft?.state==='submitted'?'Save submitted — check result':ftnState.draft?.state==='review'?'Save review is open':ftnPending()?'FTN draft has changes':'Saved FTN settings';
}
async function ftnWork(action){if(ftnBusy||busy||saving||offlineBusy||maintenanceActive)return;ftnBusy=true;ftnControls();try{return await action();}catch(error){$('ftn-result').textContent=error.message;}finally{ftnBusy=false;ftnControls();}}
function ftnPrompt(title,paragraphs,accept){$('ftn-dialog-title').textContent=title;$('ftn-dialog-body').replaceChildren(...paragraphs.map(text=>element('p',text)));$('ftn-dialog-accept').textContent=accept;$('ftn-dialog').showModal();return new Promise(resolve=>{ftnAnswer=resolve;});}
function ftnClosePrompt(answer){$('ftn-dialog').close();const resolve=ftnAnswer;ftnAnswer=null;resolve?.(answer);}
$('ftn-dialog-accept').onclick=()=>ftnClosePrompt(true);$('ftn-dialog-cancel').onclick=()=>ftnClosePrompt(false);$('ftn-dialog').oncancel=event=>{event.preventDefault();ftnClosePrompt(false);};
function ftnQuery(extra={}){return new URLSearchParams({draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,section:ftnState.section,network:ftnState.network,record:ftnState.record,offset:ftnState.offset,search:ftnState.search,...extra}).toString();}
async function ftnLocalChoices(kind){
  const result=[],base='/api/config/'+(kind==='file_area'?'files':'messages')+'/catalog';let query={},revision=null,groups=[];
  do{const current=await api(base+'?'+new URLSearchParams(Object.entries(query).filter(([,v])=>v!==null)));if(revision!==null&&current.revision!==revision)throw new Error('The local catalog changed. Refresh the FTN page before choosing a mapping.');revision=current.revision;groups.push(...current.groups);query=current.next;}while(query);
  if(kind==='message_group')return [['0','Create or match the named Message Group'],...groups.map(g=>[g.id,g.name])];
  for(const group of groups){query={group:group.id,revision};do{const current=await api(base+'?'+new URLSearchParams(Object.entries(query).filter(([,v])=>v!==null)));if(current.revision!==revision)throw new Error('The local catalog changed. Refresh before choosing a mapping.');for(const row of current[kind==='file_area'?'areas':'echoes'])if(kind!=='file_area'||row.kind===0)result.push([row.id,group.name+' / '+row.number+' '+row.name]);query=current.next;}while(query);}
  return kind==='local_echo'?[['0','Create a new local conference / no mapping for pass-through'],...result]:kind==='file_area'?[['0','Choose a local file area'],...result]:result;
}
async function ftnLinkChoices(){let offset=0,rows=[],page;do{page=await api('/api/ftn/page?'+ftnQuery({section:'links',record:'',offset:String(offset),search:''}));rows.push(...page.rows);offset+=25;}while(page.next);return [['','None'],...rows.map(r=>[r.id,r.label])];}
function ftnInput(key,label,value,type,choices){
  const id='ftn-field-'+key,l=element('label',label),input=document.createElement(type==='select'?'select':Array.isArray(value)?'textarea':'input');l.htmlFor=id;input.id=id;input.dataset.kind=type;
  if(type==='select'){for(const [v,title] of choices){const option=element('option',title);option.value=String(v);input.append(option);}input.value=value===null?'':String(value);if(input.value!==String(value??'')){const option=element('option','Saved selection is unavailable');option.value=String(value??'');input.append(option);input.value=option.value;}}
  else if(type==='checkbox'){input.type='checkbox';input.checked=value;l.className='checkbox-label';l.prepend(input);}
  else{if(!Array.isArray(value))input.type=type==='optional_number'?'number':type;input.value=Array.isArray(value)?value.join('\n'):value??'';input.autocomplete='off';if(type==='number'||type==='optional_number'){input.min=0;input.max=key==='maximum_messages'?999999999:65535;input.step=1;input.required=type==='number';}else input.maxLength=key==='additional_addresses'?4096:2048;}
  if(['access_level','read_level','post_level'].includes(key)){input.min=1;input.max=999;}
  if(['maximum_messages','maximum_age_days','port','tls_port'].includes(key))input.min=1;
  input.addEventListener('input',()=>{ftnState.unsent=true;ftnRemember();ftnControls();});input.addEventListener('change',()=>{ftnState.unsent=true;ftnRemember();ftnControls();});const slot=configFtnFieldSlot(key);slot.append(l);if(type!=='checkbox')slot.append(input);return input;
}
async function ftnRender(){
  $('ftn-intro').textContent=ftnState.section==='held'?'Inspecting held mail changes nothing. Saving a recipient choice and queuing a retry are separate actions, each with its own review.':'Changes stay in your FTN draft until you choose Save FTN Settings. Canceling a review leaves saved settings and retained mail unchanged.';
  const current=ftnState.view;$('ftn-fields').replaceChildren();ftnClearPasswords();$('ftn-passwords').replaceChildren();$('ftn-permissions').replaceChildren();$('ftn-list').replaceChildren();$('ftn-navigation').replaceChildren();
  if(ftnState.section==='setup'){await ftnSetupRender();return;}if(ftnState.section==='held'){await ftnHeldOpen();return;}
  $('config-title').textContent='FTN Networks';$('config-category').textContent='FTN Networking';$('ftn-location').textContent=(current.network_name?current.network_name+' / ':'')+({networks:'Networks',settings:'Mailer Settings',links:'Connections',echoes:'Message Echoes',files:'File Echoes',routes:'Routing & Polling',permissions:'Connection Echo Permissions',clone:'Clone Options',import:'Import Message Echoes'}[ftnState.section]);
  const nav=(label,section,network='',record='')=>{const b=element('button',label);b.type='button';b.onclick=()=>ftnWork(()=>ftnBrowse(section,network,record));$('ftn-navigation').append(b);};
  nav('Networks','networks');nav('Mailer Settings','settings');nav('Held Mail...','held');
  const setupButton=(label,action)=>{const b=element('button',label);b.type='button';b.onclick=()=>ftnWork(action);$('ftn-navigation').append(b);};
  if(ftnState.setup)setupButton('Return to Network Setup',async()=>{if(ftnState.unsent&&!await ftnPrompt('Return to network setup?',['Leave this unapplied form and return to the setup steps? Applied draft changes remain.'],'Return to Setup'))return;ftnState.unsent=false;await ftnSetupGo(ftnState.setup.step);});
  else{setupButton('Set Up a Network...',()=>ftnSetupBegin(false));if(ftnState.section==='networks'&&ftnState.record)setupButton('Review Network Setup...',()=>ftnSetupBegin(true));}
  const net=ftnState.section==='networks'?ftnState.record:ftnState.network;
  if(net){for(const [label,section] of [['Connections','links'],['Message Echoes','echoes'],['Clone Options','clone'],['Import Echoes','import'],['File Echoes','files'],['Routing & Polling','routes']])nav(label,section,net);if(ftnState.section==='links'&&ftnState.record)nav('Echo Permissions','permissions',net,ftnState.record);}
  if(ftnState.section==='import'){ftnRenderImport();ftnControls();return;}
  for(const row of current.rows){if(ftnState.section==='clone'){const label=element('label',row.id+' — '+row.label,'checkbox-label'),box=document.createElement('input');box.type='checkbox';box.dataset.tag=row.id;box.checked=(ftnState.cloneTargets||[]).includes(row.id);label.prepend(box);$('ftn-list').append(label);box.onchange=()=>{const selected=new Set(ftnState.cloneTargets||[]);if(box.checked)selected.add(row.id);else selected.delete(row.id);ftnState.cloneTargets=[...selected];ftnState.unsent=true;ftnRemember();ftnControls();};}else if(ftnState.section==='permissions'){const label=element('label',(row.file_echo?'File: ':'Message: ')+row.tag+' — '+[row.receive?'Receive':'',row.send?'Send':'',row.manage?'AreaFix':''].filter(Boolean).join(', '),'checkbox-label'),box=document.createElement('input');box.type='checkbox';box.dataset.tag=row.tag;box.dataset.file=String(row.file_echo);label.prepend(box);$('ftn-list').append(label);box.onchange=()=>{ftnState.unsent=true;ftnRemember();ftnControls();};}else{const b=element('button',row.id+' — '+row.label);b.type='button';b.onclick=()=>ftnWork(()=>ftnBrowse(ftnState.section,ftnState.network,row.id));$('ftn-list').append(b);}}
  $('ftn-page-count').textContent=current.total?String(Number(ftnState.offset)+1)+'–'+String(Number(ftnState.offset)+current.rows.length)+' of '+current.total:'No matching entries';$('ftn-search').value=ftnState.search;$('ftn-search-form').hidden=ftnState.section==='settings';
  if(!ftnState.record&&['networks','links'].includes(ftnState.section)){const label=element('label',ftnState.section==='networks'?'New network code':'New connection code');label.htmlFor='ftn-code';const input=document.createElement('input');input.id='ftn-code';input.maxLength=48;input.pattern='[A-Za-z0-9][A-Za-z0-9_-]*';input.required=true;input.autocomplete='off';input.oninput=()=>{ftnState.unsent=true;ftnRemember();ftnControls();};$('ftn-fields').append(label,input);}
  const order={networks:['name','description','enabled','address','additional_addresses','origin_line','message_group','new_group_name','default_link','echo_list_source','echo_list_url','echo_list_file','nodelist_file','msg_netmail_enabled','msg_netmail_directory','keep_processed','retention_days'],links:['name','enabled','use','address','additional_addresses','host','port','contact_on_mail','poll_minutes','tls','tls_server_name','tls_ca_file','allow_legacy_plain_auth','areafix','forward_missing_echoes','forward_via_link'],settings:['mode','external_executable','bind_address','listener_enabled','port','tls_listener_enabled','tls_port','tls_certificate','tls_private_key','allow_direct_local_netmail'],echoes:['tag','description','store_locally','local_echo','local_name','update_local_name','access_level','read_level','post_level','pipe_colors','update_local_permissions','require_real_name','ansi_colors','maximum_messages','maximum_age_days','update_local_retention'],files:['tag','description','file_area'],routes:['destination','via_link','flavor'],clone:['copy_count','maximum_messages','copy_age','maximum_age_days','copy_access','access_level','copy_read','read_level','copy_post','post_level']}[ftnState.section]||[];
  if(current.values){for(const key of order.filter(key=>Object.hasOwn(current.values,key))){const value=current.values[key];
    let choices=ftnChoices[key],type=choices?'select':['maximum_messages','maximum_age_days'].includes(key)?'optional_number':typeof value==='boolean'?'checkbox':typeof value==='number'?'number':value===null?'optional_number':'text';
    if(['message_group','local_echo','file_area'].includes(key)){choices=await ftnLocalChoices(key);type='select';}
    if(['default_link','forward_via_link','via_link'].includes(key)){const priorNetwork=ftnState.network;if(ftnState.section==='networks')ftnState.network=ftnState.record;choices=ftnState.network?await ftnLinkChoices():[['','None']];ftnState.network=priorNetwork;type='select';}
    ftnInput(key,ftnNames[key]||key,value,type,choices);
  }}
  if(ftnState.section==='clone'){for(const [label,clear] of [['All Echoes in This Network',false],['Clear Targets',true]]){const button=element('button',label);button.type='button';button.onclick=()=>ftnWork(()=>ftnCloneSelect(clear));$('ftn-navigation').append(button);}nav('Back','echoes',net);for(const [key,value] of Object.entries(ftnState.cloneValues||{})){const field=$('ftn-field-'+key);if(field){if(field.type==='checkbox')field.checked=value;else field.value=value;}}$('ftn-fields').append(element('p','Select named target echoes across pages, then check only the fields to copy. Preview makes no changes. Apply to Draft still requires the normal final Save.'));}
  if(ftnState.section==='settings')$('ftn-fields').append(element('p','External BinkD: start the mailer separately with sfxbinkd and one profile for all networks. SFX scans and imports mail while the BBS runs. BinkD owns connections and retries. Stop BinkD before backup, restore or changing mailers. The saved program path does not launch it automatically.','field-help'));
  if(ftnState.section==='links'){
    let credentials={};if(ftnState.record)credentials=await api('/api/ftn/credentials?'+new URLSearchParams({draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,network:ftnState.network,record:ftnState.record}));
    for(const [key,label] of [['session','Session password'],['packet','Packet password'],['areafix','AreaFix password'],['tic','TIC password']]){const title=element('label',label);title.htmlFor='ftn-password-'+key;const input=document.createElement('input');input.id=title.htmlFor;input.type='password';input.maxLength=key==='packet'?8:128;input.autocomplete='off';input.value=credentials[key]?.value||'';input.dataset.changed='false';input.oninput=()=>{input.dataset.changed='true';$('ftn-clear-'+key).checked=false;ftnState.unsent=true;ftnRemember();ftnControls();};const clear=document.createElement('input');clear.id='ftn-clear-'+key;clear.type='checkbox';clear.onchange=()=>{ftnState.unsent=true;ftnRemember();ftnControls();};const clearLabel=element('label','Remove this saved password','checkbox-label');clearLabel.prepend(clear);$('ftn-passwords').append(title,input,clearLabel);if(credentials[key]?.present&&!credentials[key].readable)$('ftn-passwords').append(element('p','The saved password cannot be read here. Leave it untouched to preserve it, enter a replacement, or explicitly remove it.','field-help'));if(credentials[key])credentials[key].value='';}
  }
  if(ftnState.section==='permissions'){for(const key of ['receive','send','manage']){const box=document.createElement('input');box.type='checkbox';box.id='ftn-permission-'+key;const label=element('label',key==='manage'?'AreaFix management':key==='send'?'Send':'Receive','checkbox-label');label.prepend(box);$('ftn-permissions').append(label);box.onchange=()=>{ftnState.unsent=true;ftnRemember();ftnControls();};}$('ftn-permissions').append(element('p','Choose named echoes above, then Apply to Draft. AreaFix applies to message echoes only.','field-help'));}
  ftnControls();
}
async function ftnBrowse(section,network='',record='',offset='0',search=''){
  const clonePage=section==='clone'&&ftnState.section==='clone'&&network===ftnState.network&&(offset!==ftnState.offset||search!==ftnState.search);
  if(clonePage){ftnState.cloneValues={};for(const key of Object.keys(ftnState.view.values)){const field=$('ftn-field-'+key);ftnState.cloneValues[key]=field.type==='checkbox'?field.checked:field.value;}}
  const importPage=section==='import'&&ftnState.section==='import'&&network===ftnState.network;
  if(!clonePage&&!importPage&&ftnState.unsent&&!await ftnPrompt('Leave these form edits?',['These fields have not been applied to the FTN draft. Leave them and display another entry?'],'Leave Form'))return;
  if(!clonePage&&(ftnState.section==='clone'||section==='clone'&&network!==ftnState.network)){ftnState.cloneTargets=[];ftnState.cloneValues=null;}ftnClearPasswords();ftnState={...ftnState,section,network,record,offset,search,unsent:false};ftnState.view=await ftnReadView();await ftnRender();if(section!=='held'&&$('ftn-result').textContent==='No matching held originals or pending reviewed retries.')$('ftn-result').textContent='';if(clonePage||importPage)ftnState.unsent=true;ftnRemember();ftnControls();
}
async function ftnCloneSelect(clear){
  let selected=[];
  if(!clear){let offset=0,found;do{found=await api('/api/ftn/page?'+ftnQuery({section:'clone',record:'',offset:String(offset),search:''}));selected.push(...found.rows.map(row=>row.id));if(selected.length>4096)throw new Error('Select at most 4096 echoes for one clone operation.');offset+=25;}while(found.next);}
  ftnState.cloneTargets=selected;ftnState.unsent=true;for(const box of $('ftn-list').querySelectorAll('input'))box.checked=selected.includes(box.dataset.tag);ftnRemember();ftnControls();
}
async function loadFtn(){
  $('ftn-section').hidden=false;$('board-form').hidden=true;$('desktop-client-tools').hidden=true;board={title:'FTN Networks',fields:[],active_fields:[],can_save:false};
  const saved=!ftnState.draft?ftnRetained():null;
  if(saved){try{ftnState={...ftnState,...saved,restore:saved,draft:await api('/api/ftn/state?draft='+saved.draft),unsent:false};}
    catch(error){if(error.status!==400)throw error;ftnState={...ftnState,...saved,restore:null,draft:{draft:saved.draft,edit_revision:saved.edit_revision,state:'unavailable'},view:null,unavailable:true,unconfirmed:true};$('ftn-fields').replaceChildren(element('p','This FTN draft expired or the web host restarted. Retained ordinary form text follows. Copy anything needed before discarding this draft and reading saved settings. No edits or save were repeated.'));for(const [key,value] of Object.entries(saved.values||{}))$('ftn-fields').append(element('p',(ftnNames[key]||key)+': '+String(value)));if(saved.code)$('ftn-fields').append(element('p','New code: '+saved.code));$('ftn-list').replaceChildren();$('ftn-navigation').replaceChildren();ftnClearPasswords();$('ftn-passwords').replaceChildren();$('ftn-permissions').replaceChildren();ftnControls();return;}}
  if(!ftnState.draft)ftnState.draft=await api('/api/ftn/open',{});
  if(saved?.setup&&saved.edit_revision!==ftnState.draft.edit_revision){await ftnSetupRecover();ftnState.unconfirmed=true;}
  ftnState.view=await ftnReadView();await ftnRender();
  if(ftnState.restore){const retained=ftnState.restore;ftnState.restore=null;if(retained.unsent&&retained.edit_revision===ftnState.draft.edit_revision){for(const key of Object.keys(ftnState.view.values||{})){const field=$('ftn-field-'+key),value=retained.values?.[key];if(field&&typeof value===(field.type==='checkbox'?'boolean':'string')){if(field.type==='checkbox')field.checked=value;else field.value=value;}}if($('ftn-code')&&typeof retained.code==='string')$('ftn-code').value=retained.code;if(ftnState.section==='permissions'&&retained.permissions){const p=retained.permissions;if(Array.isArray(p.selected)&&p.selected.length<=25){for(const input of $('ftn-list').querySelectorAll('input'))input.checked=p.selected.some(item=>item&&item.tag===input.dataset.tag&&item.file_echo===(input.dataset.file==='true'));for(const key of ['receive','send','manage'])if(typeof p[key]==='boolean')$('ftn-permission-'+key).checked=p[key];}}ftnState.unsent=true;$('ftn-result').textContent='Your unsent form edits were restored. Password text was not retained; re-enter any intended password change.';}else if(retained.unsent){ftnState.unconfirmed=true;$('ftn-result').textContent='The draft changed after these form edits. Check the current draft before editing again.';}}
  ftnRemember();ftnControls();
}
function ftnValues(){const values={};for(const [key,original] of Object.entries(ftnState.view.values||{})){const input=$('ftn-field-'+key);values[key]=input.type==='checkbox'?input.checked:Array.isArray(original)?input.value.split(/\r?\n/).map(v=>v.trim()).filter(Boolean):key==='ansi_colors'?(input.value===''?null:input.value==='true'):['maximum_messages','maximum_age_days'].includes(key)?(input.value===''?null:Number(input.value)):typeof original==='number'?Number(input.value):input.value;}return values;}
async function ftnApply(){
  if(ftnState.unconfirmed||ftnState.draft.state!=='editing'||!$('ftn-form').reportValidity())return false;
  if(ftnState.section==='setup')return ftnSetupCollect();
  if(ftnState.section==='import')return ftnApplyImport();
  let action,network=ftnState.network,record=ftnState.record,values=ftnValues(),secrets={};
  if(ftnState.section==='settings')action='settings';else if(ftnState.section==='clone'){action='echo_clone';record='';values.selected=ftnState.cloneTargets||[];}else if(ftnState.section==='permissions'){action='permissions';values={selected:[...$('ftn-list').querySelectorAll('input')].filter(i=>i.checked).map(i=>({tag:i.dataset.tag,file_echo:i.dataset.file==='true'})),receive:$('ftn-permission-receive').checked,send:$('ftn-permission-send').checked,manage:$('ftn-permission-manage').checked};if(!values.selected.length)throw new Error('Select at least one named echo.');}
  else{const base={networks:'network',links:'link',echoes:'echo',files:'file',routes:'route'}[ftnState.section];action=base+(record?'_save':'_add');if(base==='network'){network=record||$('ftn-code').value;record='';}else if(base==='link'&&!record)record=$('ftn-code').value;}
  if(ftnState.section==='links')for(const key of ['session','packet','areafix','tic']){const input=$('ftn-password-'+key),clear=$('ftn-clear-'+key).checked,changed=input.dataset.changed==='true';secrets[key]={action:clear?'clear':changed?(input.value?'replace':'clear'):'keep',value:!clear&&changed?input.value:''};}
  const candidate={draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,change:{revision:ftnState.draft.revision,action,network,record,values,secrets}};
  if(action==='echo_clone'){const preview=await api('/api/ftn/clone-preview',candidate);const chosen=[['count','maximum_messages'],['age','maximum_age_days'],['access','access_level'],['read','read_level'],['post','post_level']].filter(([k])=>values['copy_'+k]).map(([k,v])=>k+': '+(values[v]??'Unlimited'));if(!await ftnPrompt('Preview Clone Options',[preview.rows.length+' selected echoes. Copy: '+chosen.join(', ')+'.','Targets: '+preview.rows.map(r=>r.tag).join(', '),'Unchecked fields and unselected echoes stay unchanged. This only updates your draft.'],'Apply to Draft'))return false;}
  try{ftnState.draft=await api('/api/ftn/edit',candidate);}
  catch(error){if(![400,401,403,409].includes(error.status)){ftnState.unconfirmed=true;ftnRemember();}if(Object.values(secrets).some(secret=>secret.action==='replace'))error.message+=' Password replacements were cleared; re-enter any intended password changes before trying again.';throw error;}
  finally{for(const secret of Object.values(secrets))secret.value='';ftnClearPasswords();}
  if(action==='echo_clone'){ftnState.cloneTargets=[];ftnState.cloneValues=null;ftnState.offset='0';ftnState.search='';}
  ftnState.unsent=false;if(ftnState.section==='networks')ftnState.record=network;else if(ftnState.section==='links')ftnState.record=record;else if(ftnState.section==='echoes'||ftnState.section==='files')ftnState.record=ftnState.draft.selected_record||values.tag.toUpperCase();else if(ftnState.section==='routes')ftnState.record=ftnState.draft.selected_record||values.destination;
  ftnRemember();await ftnBrowse(ftnState.section,ftnState.network,ftnState.record);$('ftn-result').textContent='Applied to your draft. Save FTN Settings publishes the reviewed changes.';return true;
}
async function ftnSave(){
  if(ftnState.setup&&!ftnState.setupSaving)throw new Error('Return to network setup and review its final Save, or choose Save Disabled and Finish Later.');
  if(ftnState.unsent&&!await ftnApply())return false;
  if(ftnState.unconfirmed||ftnState.draft.state!=='editing')return false;
  try{ftnState.draft=await api('/api/ftn/prepare',{draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision});}catch(error){ftnState.unconfirmed=true;ftnRemember();throw error;}
  ftnRemember();if(ftnState.draft.state!=='review')throw new Error('This draft could not be prepared. Another save may be active, or saved settings changed. Keep your edits and review the saved catalog before retrying.');
  const review=ftnState.draft.review,lines=['Save this FTN draft? New settings apply to this board. Existing local messages and files stay in place.'];
  if(review.changed){lines.push('Existing processed originals affected: '+review.changed+'. Protection shortened: '+review.shortened+'.');lines.push(review.time_zone?'New expiry dates use the BBS time zone: '+review.time_zone+'.':'BBS time zone unavailable; new expiry dates are shown in UTC.');for(const n of review.networks){const expiry=value=>value==='0'?'Forever':ftnHeldTime(value,review.time_zone);lines.push(n.name+' ('+n.network+'): '+n.due_now+' due now, '+n.expire_later+' due later; '+n.from_forever+' previously kept Forever. New expiry range: '+expiry(n.earliest)+' to '+expiry(n.latest)+'.');}lines.push('This review is tied to these originals and expires after one minute. Cancel leaves saved settings and protection dates unchanged.');}
  else lines.push('No existing retained-original protection dates are changed by this preview.');
  if(!await ftnPrompt('Review FTN Settings',lines,review.shortened?'Save and Shorten Protection':'Save FTN Settings')){ftnState.draft=await api('/api/ftn/cancel-review',{draft:ftnState.draft.draft});ftnRemember();return false;}
  ftnState.unconfirmed=true;ftnRemember();
  try{ftnState.draft=await api('/api/ftn/commit',{draft:ftnState.draft.draft,operation:ftnState.draft.operation});ftnState.unconfirmed=false;}
  catch(error){$('ftn-result').textContent='Save was not confirmed. Check Save Result reads the same operation without submitting again.';ftnRemember();throw error;}
  ftnRemember();if(!ftnState.draft.result?.saved){$('ftn-result').textContent='No successful save was confirmed. Use Check Save Result; this draft is locked against another submission.';return false;}
  $('ftn-result').textContent='FTN settings saved. Open Saved Settings reads the saved catalog without saving again.';ftnControls();return true;
}
async function ftnDiscard(ask=true){
  if(ask&&!await ftnPrompt('Discard FTN Draft?',['Discard unsaved FTN edits and read the saved settings? This does not undo a completed save or repeat an uncertain save.'],'Discard Draft'))return false;
  if(ftnState.draft&&!ftnState.unavailable)await api('/api/ftn/discard',{draft:ftnState.draft.draft});ftnClearPasswords();if(draftKey)sessionStorage.removeItem(draftKey+'-ftn');$('ftn-result').textContent='';ftnState={draft:null,view:null,section:'networks',network:'',record:'',offset:'0',search:'',unsent:false,unconfirmed:false,restore:null};return true;
}
$('ftn-form').onsubmit=event=>{event.preventDefault();void ftnWork(ftnApply);};$('ftn-save').onclick=()=>ftnWork(ftnSave);
$('ftn-discard').onclick=()=>ftnWork(async()=>{if(await ftnDiscard(!ftnState.draft?.result?.saved))await loadFtn();});
$('ftn-revert').onclick=()=>ftnWork(async()=>{ftnState.unsent=false;await ftnBrowse(ftnState.section,ftnState.network,ftnState.record,ftnState.offset,ftnState.search);});
$('ftn-new').onclick=()=>ftnWork(()=>ftnBrowse(ftnState.section,ftnState.network));
$('ftn-search-form').onsubmit=event=>{event.preventDefault();void ftnWork(()=>ftnBrowse(ftnState.section,ftnState.network,ftnState.record,'0',$('ftn-search').value));};
$('ftn-previous').onclick=()=>ftnWork(()=>ftnBrowse(ftnState.section,ftnState.network,ftnState.record,String(Math.max(0,Number(ftnState.offset)-25)),ftnState.search));$('ftn-next').onclick=()=>ftnWork(()=>ftnBrowse(ftnState.section,ftnState.network,ftnState.record,String(Number(ftnState.offset)+25),ftnState.search));
$('ftn-check').onclick=()=>ftnWork(async()=>{if(ftnState.unsent&&!await ftnPrompt('Read back the FTN draft?',['These form edits have not been applied. Leave them and read the current draft?'],'Leave Form Edits'))return;const previous=ftnState.draft;ftnState.draft=await api('/api/ftn/state?draft='+previous.draft);if(ftnState.draft.state==='submitted')ftnState.draft=await api('/api/ftn/result',{draft:previous.draft});else if(ftnState.draft.state==='review'){if(await ftnPrompt('Cancel the pending review?',['No submitted save is recorded in this draft. Cancel its prepared review before making another explicit save.'],'Cancel Review'))ftnState.draft=await api('/api/ftn/cancel-review',{draft:previous.draft});}ftnState.unconfirmed=false;ftnState.unsent=false;if(ftnState.setup)await ftnSetupRecover();await ftnBrowse(ftnState.section,ftnState.network,ftnState.record);$('ftn-result').textContent=ftnState.draft.result?.saved?'The exact save receipt confirms FTN settings were saved. No save was repeated.':ftnState.draft.state==='submitted'?'No successful receipt is available. This does not prove no change occurred. Review saved settings before a new draft.':'Current draft read back. No edit or save was repeated.';ftnRemember();});
$('ftn-remove').onclick=()=>ftnWork(async()=>{if(ftnState.unsent)throw new Error('Apply or undo the form edits before removing settings.');if(!await ftnPrompt('Remove FTN Settings?',['Remove this entry and dependent FTN permissions/routes? Local messages, conferences, file areas, files and preserved mail remain. This changes the draft; Save FTN Settings is still required.'],'Remove from Draft'))return;const action={networks:'network_remove',links:'link_remove',echoes:'echo_remove',files:'file_remove',routes:'route_remove'}[ftnState.section];const candidate={draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,change:{revision:ftnState.draft.revision,action,network:ftnState.section==='networks'?ftnState.record:ftnState.network,record:ftnState.section==='networks'?'':ftnState.record,values:{},secrets:{}}};try{ftnState.draft=await api('/api/ftn/edit',candidate);}catch(error){if(![400,401,403,409].includes(error.status))ftnState.unconfirmed=true;ftnRemember();throw error;}await ftnBrowse(ftnState.section,ftnState.network);});
window.addEventListener('pagehide',ftnClearPasswords);

let ftnImportPolling=false,ftnImportPollAt=0;
function ftnImportData(){
  if(!ftnState.importData||ftnState.importData.network!==ftnState.network){
    const saved=ftnState.importDraft;ftnState.importDraft=null;
    const d={network:ftnState.network,source:'',kind:'file',encoding:'utf8',access:10,read:10,post:10,update_text:false,list:'',job:'',loading:false,loaded_source:'',selected:[],rows:[],issues:[],detail:''};
    if(saved&&saved.network===d.network&&typeof saved.source==='string'&&saved.source.length<=4096&&['file','url'].includes(saved.kind)&&['utf8','cp437'].includes(saved.encoding)&&Array.isArray(saved.selected)&&saved.selected.length<=16384&&saved.selected.every(tag=>typeof tag==='string'&&tag.length<=128)){
      for(const key of ['source','kind','encoding','selected'])d[key]=saved[key];
      for(const key of ['access','read','post'])if(Number.isInteger(saved[key])&&saved[key]>=1&&saved[key]<=999)d[key]=saved[key];
      for(const key of ['list','job'])if(typeof saved[key]==='string'&&/^[a-f0-9]{32}$/.test(saved[key]))d[key]=saved[key];
      d.loading=saved.loading===true;d.update_text=saved.update_text===true;
    }
    ftnState.importData=d;
  }return ftnState.importData;
}
function ftnImportDraft(){const d=ftnState.importData;if(!d)return ftnState.importDraft||null;const out={};for(const key of ['network','source','kind','encoding','access','read','post','update_text','list','job','loading','selected'])out[key]=d[key];return out;}
function ftnImportRows(){const find=ftnState.search.toUpperCase();return ftnImportData().rows.filter(row=>(row.tag+' '+row.description).toUpperCase().includes(find));}
function ftnAcceptList(value){
  const d=ftnImportData();d.detail=value.detail;d.loading=value.active;d.job=value.list;
  if(value.ready){if(d.list!==value.list){d.selected=value.rows.slice(0,16384).map(row=>row.tag);ftnState.unsent=true;}d.list=value.list;d.rows=value.rows;d.issues=value.issues;d.loaded_source=value.source;}
  ftnRemember();
}
async function ftnReadView(){
  if(['setup','held'].includes(ftnState.section))return {values:null,rows:[],network_name:'',next:false};
  if(ftnState.section!=='import')return api('/api/ftn/page?'+ftnQuery());
  const net=await api('/api/ftn/page?'+ftnQuery({section:'networks',network:'',record:ftnState.network,offset:'0',search:''}));
  const d=ftnImportData();if(d.list&&!d.rows.length&&!d.loading)ftnAcceptList(await api('/api/ftn/list?'+new URLSearchParams({draft:ftnState.draft.draft,list:d.list})));
  return {...net,network_name:net.values.name,rows:[],values:null,section:'import'};
}

const ftnSetupTitles=['','Network and message group','Your hub connection','Choose message echoes','Caller access to selected echoes','Echo exchange with your hub','NetMail and contact policy','Names, colors and Origin','Optional features and message limits','Review and Save'];
function ftnSetupRetained(){const w=ftnState.setup;if(!w)return null;return ftnSetupSanitize(w);}
function ftnSetupSanitize(w){if(!w||!Number.isInteger(w.step)||w.step<1||w.step>9||typeof w.network!=='string'||w.network.length>48||typeof w.hub!=='string'||w.hub.length>48||typeof w.selected!=='string'||w.selected.length>128)return null;const out={forms:{}};for(const key of ['step','network','hub','created','hubCreated','withHub','prepared','receive','send','enabled','preset','selected','review','exchangeTouched','exchangeMixed'])out[key]=w[key];const fields={1:['preset','name','address','message_group','new_group_name'],2:['withHub','name','address','host','port'],3:['tag','local_name'],4:['name','access','read','post'],5:['receive','send','prepared'],6:['contact','timer','minutes'],7:['origin','real','ansi','pipe'],8:['msg','msg_path','nodelist','count','age'],9:['enabled']};for(const [step,keys] of Object.entries(fields))if(w.forms?.[step]){out.forms[step]={};for(const key of keys){const value=w.forms[step][key];if(typeof value==='boolean'||typeof value==='number'||typeof value==='string'&&value.length<=2048)out.forms[step][key]=value;}}return out;}
async function ftnSetupBegin(review=false){
  const selected=review?(ftnState.section==='networks'?ftnState.record:ftnState.network):'';
  if(ftnPending()&&!await ftnPrompt('Start network setup?',['Discard the current unpublished FTN draft and start from saved settings? Existing saved networks and mail remain.'],'Start Setup'))return;
  await ftnDiscard(false);ftnState.draft=await api('/api/ftn/open',{});
  const ids=await api('/api/ftn/setup-ids',{draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision});
  ftnState.setup={step:1,network:selected||ids.network,hub:ids.hub,created:!!selected,hubCreated:false,withHub:true,prepared:false,receive:true,send:true,enabled:true,preset:'FidoNet',forms:{},selected:'',review:!!selected,exchangeTouched:false,exchangeMixed:false};
  const w=ftnState.setup;if(selected){const page=await ftnSetupPage('networks',selected);w.enabled=page.values.enabled;const links=await ftnSetupRows('links');const hub=links.find(l=>l.id.toUpperCase()===page.values.default_link.toUpperCase());if(hub){w.hub=hub.id;w.hubCreated=true;const permissions=await ftnSetupPermissions();if(permissions.length){w.receive=permissions.every(p=>p.receive);w.send=permissions.every(p=>p.send);w.exchangeMixed=permissions.some(p=>p.receive!==w.receive||p.send!==w.send);}}else w.withHub=false;w.preset=['FidoNet','fsxNet'].includes(page.values.name)?page.values.name:'Other';}
  ftnState.unsent=false;await ftnSetupGo(1);
}
async function ftnSetupPage(section,record=''){const w=ftnState.setup;return api('/api/ftn/page?'+ftnQuery({section,network:section==='networks'||section==='settings'?'':w.network,record,offset:'0',search:''}));}
async function ftnSetupRows(section){const rows=[];let offset=0,result;do{result=await api('/api/ftn/page?'+ftnQuery({section,network:section==='networks'?'':ftnState.setup.network,record:'',offset:String(offset),search:''}));rows.push(...result.rows);offset+=25;if(rows.length>4096)throw new Error('This setup list exceeds the supported size. Use full settings for this network.');}while(result.next);return rows;}
async function ftnSetupPermissions(){const rows=[];let offset=0,result;do{result=await api('/api/ftn/page?'+ftnQuery({section:'permissions',network:ftnState.setup.network,record:ftnState.setup.hub,offset:String(offset),search:''}));rows.push(...result.rows.filter(p=>!p.file_echo));offset+=25;if(rows.length>4096)throw new Error('The connection has too many permissions for setup.');}while(result.next);return rows;}
async function ftnSetupRecover(){
  const w=ftnState.setup;if(!w)return;
  if(ftnState.draft.result?.saved){ftnState.setup=null;ftnState.section='networks';ftnState.network='';ftnState.record=w.network;return;}
  const networks=await ftnSetupRows('networks');w.created=networks.some(row=>row.id.toUpperCase()===w.network.toUpperCase());
  w.hubCreated=w.created&&(await ftnSetupRows('links')).some(row=>row.id.toUpperCase()===w.hub.toUpperCase());
  // Re-read the applied step after an uncertain response; never repeat it.
  delete w.forms[w.step];
}
async function ftnSetupGo(step){ftnState.setup.step=step;ftnState.section='setup';ftnState.network=ftnState.setup.network;ftnState.record='';ftnState.offset='0';ftnState.search='';ftnState.view={values:null,rows:[],next:false};await ftnRender();ftnRemember();ftnControls();}
function ftnSetupForm(defaults){const w=ftnState.setup;return w.forms[w.step]||(w.forms[w.step]=structuredClone(defaults));}
function ftnSetupField(key,label,type='text',choices){const w=ftnState.setup,form=w.forms[w.step],input=ftnInput('setup_'+key,label,form[key],type,choices);const changed=()=>{form[key]=type==='checkbox'?input.checked:type==='number'?Number(input.value):input.value;if(w.step===5&&['receive','send'].includes(key))w.exchangeTouched=true;ftnRemember();};input.addEventListener('input',changed);input.addEventListener('change',changed);return input;}
function ftnSetupButton(label,action){const b=element('button',label);b.type='button';b.onclick=()=>ftnWork(action);$('ftn-fields').append(b);return b;}
function ftnSetupChange(action,record,values,secrets={}){return {revision:ftnState.draft.revision,action,network:ftnState.setup.network,record,values,secrets};}
async function ftnSetupBatch(changes){
  if(ftnState.unconfirmed||ftnState.draft.state!=='editing')throw new Error('Check the last request before continuing setup.');
  try{ftnState.draft=await api('/api/ftn/setup-step',{draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,changes});}
  catch(error){if(![400,401,403,409].includes(error.status))ftnState.unconfirmed=true;ftnRemember();throw error;}
  finally{for(const change of changes)for(const secret of Object.values(change.secrets||{}))secret.value='';ftnClearPasswords();}
  ftnState.unsent=false;ftnRemember();
}
async function ftnSetupEchoes(){return ftnState.setup.created?ftnSetupRows('echoes'):[];}
async function ftnSetupSelected(rows){const w=ftnState.setup;if(!rows.length)return null;if(!rows.some(r=>r.id===w.selected))w.selected=rows[0].id;return (await ftnSetupPage('echoes',w.selected)).values;}
async function ftnSetupReviewPage(rows,offset=0){
  const list=$('ftn-list');list.replaceChildren();
  list.append(element('p',rows.length?'Echo details '+(offset+1)+'–'+Math.min(offset+10,rows.length)+' of '+rows.length:'No message echoes selected.'));
  for(const row of rows.slice(offset,offset+10)){
    const e=(await ftnSetupPage('echoes',row.id)).values;
    const item=element('section');item.append(element('h4',e.tag+' — '+(e.local_name||e.description||e.tag)),element('p','Access '+e.access_level+' / Read '+e.read_level+' / Post '+e.post_level+'; maximum messages '+(e.maximum_messages??'Unlimited')+'; maximum age '+(e.maximum_age_days===null?'Unlimited':e.maximum_age_days+' days')+'.'),element('p','Real Name '+(e.require_real_name?'On':'Off')+'; outgoing ANSI '+(e.ansi_colors===null?'Legacy choice':e.ansi_colors?'On':'Off')+'; pipe colors '+(e.pipe_colors?'On':'Off')+'; '+(e.store_locally?'Local conference':'Pass-through only')+'.'));list.append(item);
  }
  for(const [label,next] of [['Previous Echo Details',offset-10],['Next Echo Details',offset+10]])if(next>=0&&next<rows.length){const b=element('button',label);b.type='button';b.onclick=()=>ftnWork(()=>ftnSetupReviewPage(rows,next));list.append(b);}
}
async function ftnSetupRender(){
  const w=ftnState.setup;if(!w||!Number.isInteger(w.step)||w.step<1||w.step>9)throw new Error('Setup draft is unavailable. Discard it and read saved settings.');
  $('config-title').textContent='FTN Network Setup';$('config-category').textContent='FTN Networking';
  $('ftn-location').textContent='Set Up a Network — '+w.step+' of 9: '+ftnSetupTitles[w.step];$('ftn-search-form').hidden=true;$('ftn-page-count').textContent='';
  const nav=(label,fn)=>{const b=element('button',label);b.type='button';b.onclick=()=>ftnWork(fn);$('ftn-navigation').append(b);return b;};
  if(w.step>1)nav('Back',async()=>{if(ftnState.unsent&&!await ftnPrompt('Keep these entries for later?',['Your ordinary entries stay in this tab. Go back without applying this step? Password text is cleared.'],'Back'))return;ftnState.unsent=false;await ftnSetupGo(w.step-1);});
  if(w.step<9)nav('Next',async()=>{if(await ftnSetupCollect()){delete w.forms[w.step];await ftnSetupGo(w.step+1);}});
  nav('Cancel Setup',async()=>{if(await ftnPrompt('Cancel network setup?',['Discard this unpublished setup draft? No saved settings or mail will be changed.'],'Cancel Setup')){await ftnDiscard(false);await loadFtn();}});
  if(w.created)nav('Save Disabled and Finish Later...',()=>ftnSetupFinish(false,true));
  const network=w.created?(await ftnSetupPage('networks',w.network)).values:(await ftnSetupPage('networks')).values;
  if(w.step===1){const form=ftnSetupForm({preset:w.preset,name:w.created?network.name:'FidoNet',address:network.address,message_group:network.message_group,new_group_name:w.created?network.new_group_name:'FidoNet'});const preset=ftnSetupField('preset','Network','select',[['FidoNet','FidoNet'],['fsxNet','fsxNet'],['Other','Other']]);preset.addEventListener('change',()=>{w.preset=preset.value;if(preset.value!=='Other'){form.name=form.new_group_name=preset.value;$('ftn-field-setup_name').value=form.name;$('ftn-field-setup_new_group_name').value=form.new_group_name;}ftnRemember();});ftnSetupField('name','Network name').maxLength=64;ftnSetupField('address','Your assigned FTN address').maxLength=96;ftnSetupField('message_group','Message group','select',await ftnLocalChoices('message_group'));ftnSetupField('new_group_name','Group name to create or match').maxLength=64;$('ftn-fields').append(element('p','SFX creates or reuses this group at the final Save. An existing group keeps its settings.'));
  }else if(w.step===2){const native=(await ftnSetupPage('settings')).values.mode===0;const link=(await ftnSetupPage('links',w.hubCreated?w.hub:'')).values;ftnSetupForm({withHub:w.withHub,name:w.hubCreated?link.name:network.name+' Hub',address:link.address,host:link.host,port:link.port});ftnSetupField('withHub','Set up my hub connection now','checkbox');for(const [key,label] of [['name','Hub connection name'],['address','Hub FTN address'],['host','Hub host or IP address']])ftnSetupField(key,label);const port=ftnSetupField('port','Port','number');port.min=1;port.max=65535;ftnModeControl(port,native);ftnModeControl($('ftn-field-setup_host'),native);if(!native)$('ftn-fields').append(element('p','External BinkD uses the host, port and session password in its profile. SFX keeps the separate packet, AreaFix and TIC passwords.'));
    let credentials={};if(w.hubCreated)credentials=await api('/api/ftn/credentials?'+new URLSearchParams({draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,network:w.network,record:w.hub}));
    for(const key of ['session','packet','areafix','tic']){const l=element('label',key==='packet'?'Packet password (maximum 8 characters)':key==='areafix'?'AreaFix password':key==='tic'?'TIC password':'Session password'),input=document.createElement('input');input.id='ftn-password-'+key;l.htmlFor=input.id;input.type='password';input.maxLength=key==='packet'?8:128;input.autocomplete='off';if(key==='session')ftnModeControl(input,native);input.value=credentials[key]?.value||'';input.dataset.changed='false';input.oninput=()=>{input.dataset.changed='true';ftnState.unsent=true;ftnRemember();};$('ftn-passwords').append(l,input);if(credentials[key])credentials[key].value='';}$('ftn-fields').append(element('p','Passwords are separate. Leave unused passwords blank. Clear a displayed password explicitly to remove it. Uncheck hub setup to add a connection later.'));
  }else if(w.step===3){ftnSetupForm({tag:'',local_name:''});ftnSetupField('tag','Add your own echo tag').maxLength=128;ftnSetupField('local_name','Caller name (optional)').maxLength=64;ftnSetupButton('Add Echo to Draft',async()=>{await ftnSetupAddEcho();await ftnSetupGo(3);});ftnSetupButton('Load and Review EchoList...',async()=>{if(ftnState.unsent&&!await ftnSetupAddEcho())return;await ftnBrowse('import',w.network);});const rows=await ftnSetupEchoes();$('ftn-fields').append(element('p',rows.length+' message echoes in this network draft. Import only adds the reviewed selections. You can add more later.'));for(const row of rows.slice(0,100))$('ftn-list').append(element('p',row.id+' — '+row.label));
  }else if([4,7,8].includes(w.step)){
    const rows=await ftnSetupEchoes(),echo=await ftnSetupSelected(rows);const defaults=w.step===4?{name:echo?.local_name||'',access:echo?.access_level||10,read:echo?.read_level||10,post:echo?.post_level||10}:w.step===7?{origin:network.origin_line,real:echo?.require_real_name||false,ansi:echo?echo.ansi_colors??true:false,pipe:echo?.pipe_colors||false}:{msg:network.msg_netmail_enabled,msg_path:network.msg_netmail_directory,nodelist:network.nodelist_file,count:echo?.maximum_messages??'',age:echo?.maximum_age_days??''};ftnSetupForm(defaults);
    if(rows.length){const label=element('label','Selected message echo'),select=document.createElement('select');select.id='ftn-field-setup_echo';label.htmlFor=select.id;for(const row of rows){const option=element('option',row.id+' — '+row.label);option.value=row.id;select.append(option);}select.value=w.selected;$('ftn-fields').append(label,select);select.onchange=()=>ftnWork(async()=>{if(ftnState.unsent&&!await ftnPrompt('Change selected echo?',['Leave the unapplied entries on this page, including network fields, and read the selected echo?'],'Read Selected Echo')){select.value=w.selected;return;}w.selected=select.value;delete w.forms[w.step];ftnState.unsent=false;await ftnSetupGo(w.step);});}
    if(w.step===4){ftnSetupField('name','Caller-facing conference name').maxLength=64;for(const key of ['access','read','post']){const input=ftnSetupField(key,key==='access'?'Access level':key==='read'?'Read level':'Post level','number');input.min=1;input.max=999;}$('ftn-fields').append(element('p','Apply to All copies levels only. Caller names remain individual.'));}
    if(w.step===7){ftnSetupField('real','Require Real Name on outgoing posts','checkbox');ftnSetupField('ansi','Allow ANSI in outgoing posts','checkbox');ftnSetupField('pipe','Allow pipe colors','checkbox');ftnSetupField('origin','Network Origin line').maxLength=78;}
    if(w.step===8){ftnSetupField('msg','Scan external .MSG NetMail for this network','checkbox');ftnSetupField('msg_path','Directory containing numbered .MSG files');ftnSetupField('nodelist','Optional local nodelist file');for(const [key,label] of [['count','Maximum messages (blank = Unlimited)'],['age','Maximum age in days (blank = Unlimited)']]){const field=ftnSetupField(key,label,'optional_number');field.min=1;field.max=key==='count'?999999999:65535;}$('ftn-fields').append(element('p','Each .MSG supplies attachment paths. The nodelist can supply a configured connection’s missing endpoint. NetMail still follows the chosen hub route.'));}
    if(rows.length){ftnSetupButton('Apply to Selected Echo',()=>ftnSetupApplyEcho(false));ftnSetupButton('Apply to All Echoes...',()=>ftnSetupApplyEcho(true));}else $('ftn-fields').append(element('p','No message echoes selected. You may add them later.'));
  }else if(w.step===5){ftnSetupForm({receive:w.receive,send:w.send,prepared:w.prepared});if(w.withHub&&w.hubCreated){ftnSetupField('receive','Receive these echoes from my hub','checkbox');ftnSetupField('send','Send posts in these echoes to my hub','checkbox');ftnSetupField('prepared','Prepare an AreaFix subscription request','checkbox');if(w.review)$('ftn-fields').append(element('p',w.exchangeMixed?'Existing echo permissions differ. Next keeps those choices unless you explicitly change Receive or Send here; either change applies both visible choices to all message echoes.':'Next keeps existing echo permissions unless you change Receive or Send here.'));}else $('ftn-fields').append(element('p','No hub is configured. You can add one and choose echo exchange later.'));$('ftn-fields').append(element('p','Preparation sends nothing. The final page has separate Save Network and Save & Queue AreaFix Request actions.'));
  }else if(w.step===6){if(w.withHub&&w.hubCreated){const native=(await ftnSetupPage('settings')).values.mode===0;const link=(await ftnSetupPage('links',w.hub)).values;ftnSetupForm({contact:link.contact_on_mail,timer:link.poll_minutes>0,minutes:link.poll_minutes||30});$('ftn-fields').append(element('p','Outgoing NetMail uses '+link.name+' ('+link.address+'). Explicit routes and Hold mail retain their rules.'));ftnModeControl(ftnSetupField('contact','Contact my hub when outgoing mail is queued','checkbox'),native);ftnModeControl(ftnSetupField('timer','Also contact the hub on a timer','checkbox'),native);const minutes=ftnSetupField('minutes','Timer interval (minutes)','number');minutes.min=1;minutes.max=65535;ftnModeControl(minutes,native);if(!native)$('ftn-fields').append(element('p','Native contact controls are inactive. External BinkD manages calls and timers in its own profile; Disabled mode makes no calls.'));}else{ftnSetupForm({});$('ftn-fields').append(element('p','No NetMail hub yet. Add one later in Connections.'));}
  }else if(w.step===9){ftnSetupForm({enabled:w.enabled});const rows=await ftnSetupEchoes();$('ftn-fields').append(element('p','Network: '+network.name+' / '+network.address),element('p','Message group: '+(network.new_group_name||(await ftnLocalChoices('message_group')).find(choice=>String(choice[0])===String(network.message_group))?.[1]||'Saved group unavailable')),element('p','Echoes: '+rows.length+'; '+(w.exchangeMixed?'existing Receive/Send choices differ and are preserved.':'Receive '+(w.receive?'On':'Off')+' / Send '+(w.send?'On':'Off')+'.')),element('p','AreaFix: '+(w.prepared?'Prepared for the selected hub; nothing queued yet.':'No request prepared.')),element('p','Origin: '+(network.origin_line||'None')+'; .MSG scanning '+(network.msg_netmail_enabled?'On':'Off')+'; nodelist '+(network.nodelist_file||'None')));
    if(w.hubCreated){const link=(await ftnSetupPage('links',w.hub)).values;const native=(await ftnSetupPage('settings')).values.mode===0;$('ftn-fields').append(element('p','Hub: '+link.name+' / '+link.address+(native?' / '+link.host+':'+link.port+'; contact on mail '+(link.contact_on_mail?'On':'Off')+'; timer '+(link.poll_minutes?link.poll_minutes+' minutes':'Off'):'; calling and timers use the External BinkD profile, or remain inactive when Disabled.')));}else $('ftn-fields').append(element('p','Hub: Add later'));
    await ftnSetupReviewPage(rows);
    ftnSetupField('enabled','Enable this network after saving','checkbox');ftnSetupButton('Clone Options...',async()=>{w.enabled=w.forms[9].enabled;ftnState.unsent=false;await ftnBrowse('clone',w.network);});ftnSetupButton('Save Network...',()=>ftnSetupFinish(false,false));if(w.prepared)ftnSetupButton('Save & Queue AreaFix Request...',()=>ftnSetupFinish(true,false));
  }
  ftnControls();
}

async function ftnSetupCollect(){
  const w=ftnState.setup,f=w.forms[w.step]||{};if(ftnState.unconfirmed||ftnState.draft.state!=='editing')throw new Error('Check the last setup request before continuing.');if(w.step!==2||f.withHub)if(!$('ftn-form').reportValidity())return false;
  const network=w.created?(await ftnSetupPage('networks',w.network)).values:(await ftnSetupPage('networks')).values;
  if(w.step===1){if(!f.name.trim()||!f.address.trim()||(!Number(f.message_group)&&!f.new_group_name.trim()))throw new Error('Enter the network name, assigned FTN address and message group.');Object.assign(network,{name:f.name,address:f.address,message_group:f.message_group,new_group_name:Number(f.message_group)?'':f.new_group_name,enabled:false,msg_netmail_enabled:network.msg_netmail_enabled??false});await ftnSetupBatch([ftnSetupChange(w.created?'network_setup_save':'network_setup_add','',network)]);w.created=true;w.preset=f.preset;
  }else if(w.step===2){const changes=[];if(f.withHub){const link=(await ftnSetupPage('links',w.hubCreated?w.hub:'')).values;const native=(await ftnSetupPage('settings')).values.mode===0;Object.assign(link,{name:f.name,address:f.address});if(native)Object.assign(link,{host:f.host,port:f.port});if(!w.hubCreated)Object.assign(link,{enabled:true,use:2,contact_on_mail:true,poll_minutes:0});if(!link.address.trim()||!link.name.trim()||native&&(!link.host.trim()||!Number.isInteger(Number(link.port))||Number(link.port)<1||Number(link.port)>65535))throw new Error(native?'Enter the hub name, assigned address, host and port, or uncheck hub setup.':'Enter the hub name and assigned FTN address, or uncheck hub setup.');const secrets={};for(const key of ['session','packet','areafix','tic']){const input=$('ftn-password-'+key);if(key==='session'&&!native){secrets[key]={action:'keep',value:''};continue;}secrets[key]={action:input.dataset.changed==='true'?(input.value?'replace':'clear'):'keep',value:input.dataset.changed==='true'?input.value:''};}changes.push(ftnSetupChange(w.hubCreated?'link_save':'link_add',w.hub,link,secrets));network.default_link=w.hub;}else{if(w.hubCreated)changes.push(ftnSetupChange('link_remove',w.hub,{}));network.default_link='';}changes.push(ftnSetupChange('network_save','',network));try{await ftnSetupBatch(changes);}catch(error){error.message+=' Password replacements were cleared; re-enter any intended change.';throw error;}w.withHub=w.hubCreated=!!f.withHub;if(!f.withHub)w.prepared=false;
  }else if(w.step===3){if(!await ftnSetupAddEcho())return false;
  }else if([4,7,8].includes(w.step)){if(ftnState.unsent&&!await ftnSetupApplyEcho(false))return false;
  }else if(w.step===5){const rows=await ftnSetupEchoes();if(f.prepared&&(!w.hubCreated||!f.receive||!rows.length))throw new Error('An AreaFix request needs a hub, Receive enabled and at least one selected echo.');if(w.hubCreated&&rows.length&&(!w.review||w.exchangeTouched)){await ftnSetupBatch([ftnSetupChange('permissions',w.hub,{selected:rows.map(r=>({tag:r.id,file_echo:false})),receive:f.receive,send:f.send,manage:false})]);w.exchangeMixed=false;w.exchangeTouched=false;}w.receive=w.hubCreated&&f.receive;w.send=w.hubCreated&&f.send;w.prepared=w.hubCreated&&f.prepared;
  }else if(w.step===6&&w.hubCreated){if((await ftnSetupPage('settings')).values.mode!==0){ftnState.unsent=false;ftnRemember();return true;}const link=(await ftnSetupPage('links',w.hub)).values;if(f.timer&&(!Number.isInteger(f.minutes)||f.minutes<1||f.minutes>65535))throw new Error('Enter a timer interval from 1 to 65535 minutes.');Object.assign(link,{contact_on_mail:f.contact,poll_minutes:f.timer?f.minutes:0});const keep=Object.fromEntries(['session','packet','areafix','tic'].map(k=>[k,{action:'keep',value:''}]));await ftnSetupBatch([ftnSetupChange('link_save',w.hub,link,keep)]);
  }else if(w.step===9)w.enabled=f.enabled;
  ftnState.unsent=false;ftnRemember();return true;
}
async function ftnSetupAddEcho(){
  const w=ftnState.setup,f=w.forms[3]||{};if(!f.tag?.trim()){if(f.local_name?.trim())throw new Error('Enter the echo tag before adding its caller name.');ftnState.unsent=false;return true;}
  const echo=(await ftnSetupPage('echoes')).values;Object.assign(echo,{tag:f.tag,local_name:f.local_name||f.tag,require_real_name:w.preset==='FidoNet',ansi_colors:false});await ftnSetupBatch([ftnSetupChange('echo_add','',echo)]);w.forms[3]={tag:'',local_name:''};return true;
}
async function ftnSetupApplyEcho(all){
  const w=ftnState.setup,f=w.forms[w.step],rows=await ftnSetupEchoes(),targets=all?rows.map(r=>r.id):w.selected?[w.selected]:[];
  if(all&&!await ftnPrompt('Apply to all echoes in this setup?',[targets.length+' named echoes in this network will receive the visible '+(w.step===4?'Access, Read and Post levels; individual caller names stay unchanged':w.step===7?'Real Name, outgoing ANSI and pipe-color choices':'message count and age limits')+'.','This updates only the draft. Final Save is still required.'],'Apply to All'))return false;
  const changes=[];
  if(w.step===7||w.step===8){const network=(await ftnSetupPage('networks',w.network)).values;if(w.step===7)network.origin_line=f.origin;else{network.msg_netmail_enabled=f.msg;network.msg_netmail_directory=f.msg_path;network.nodelist_file=f.nodelist;if(f.msg&&!f.msg_path.trim())throw new Error('Enter the .MSG directory or turn its scanner off.');}changes.push(ftnSetupChange('network_save','',network));}
  if(targets.length){
    if(w.step===7){const echo=all?null:(await ftnSetupPage('echoes',w.selected)).values;const values={selected:targets,real_name:all||f.real!==echo.require_real_name?f.real:null,ansi:all||f.ansi!==(echo.ansi_colors??true)?f.ansi:null,pipe:all||f.pipe!==echo.pipe_colors?f.pipe:null};if(['real_name','ansi','pipe'].some(key=>values[key]!==null))changes.push(ftnSetupChange('echo_presentation','',values));}
    else if(w.step===4&&!all){const echo=(await ftnSetupPage('echoes',w.selected)).values;Object.assign(echo,{local_name:f.name,access_level:f.access,read_level:f.read,post_level:f.post,update_local_name:!!Number(echo.local_echo),update_local_permissions:!!Number(echo.local_echo)});changes.push(ftnSetupChange('echo_save',w.selected,echo));}
    else{const values={selected:targets,copy_count:w.step===8,maximum_messages:f.count===''?null:Number(f.count),copy_age:w.step===8,maximum_age_days:f.age===''?null:Number(f.age),copy_access:w.step===4,access_level:f.access||10,copy_read:w.step===4,read_level:f.read||10,copy_post:w.step===4,post_level:f.post||10};if(w.step===4){values.maximum_messages=null;values.maximum_age_days=null;}changes.push(ftnSetupChange('echo_clone','',values));}
  }
  if(changes.length)await ftnSetupBatch(changes);ftnState.unsent=false;ftnRemember();$('ftn-result').textContent='Selected setup fields applied to the draft. Final Save is still required.';return true;
}
async function ftnSetupFinish(queue,later){
  const w=ftnState.setup;if(ftnState.unconfirmed||!w.created)return false;
  if(later&&!await ftnPrompt('Save the applied steps and finish later?',['Save the valid steps already applied to this draft with this network disabled? Unapplied fields on this page are not included. No AreaFix request will be queued.'],'Review Disabled Save'))return false;
  if(!later){if(w.step!==9)return false;w.enabled=!!w.forms[9].enabled;}
  const rows=queue?await ftnSetupEchoes():[];if(queue&&(!w.prepared||!w.enabled))throw new Error('Prepare the AreaFix request and enable this network before queueing.');
  await ftnSetupBatch([ftnSetupChange('setup_finish','',{enabled:later?false:w.enabled,queue,hub:queue?w.hub:'',selected:rows.map(r=>r.id)})]);
  ftnState.setupSaving=true;let saved;try{saved=await ftnSave();}finally{ftnState.setupSaving=false;}
  if(saved){const network=w.network;ftnState.setup=null;await ftnBrowse('networks','',network);$('ftn-result').textContent=later?'Network saved disabled. Review Network Setup continues its saved settings.':queue?'Network saved. AreaFix request queued; hub acceptance is pending.':'Network saved. No AreaFix request was queued.';}
  ftnRemember();return saved;
}
function ftnRenderImport(){
  const d=ftnImportData();$('ftn-fields').replaceChildren();$('ftn-list').replaceChildren();$('ftn-search').value=ftnState.search;$('ftn-search-form').hidden=false;
  const add=(key,label,type,choices)=>{const input=ftnInput('import_'+key,label,d[key],type,choices);input.addEventListener('input',()=>{d[key]=type==='checkbox'?input.checked:['access','read','post'].includes(key)?Number(input.value):input.value;ftnRemember();});input.addEventListener('change',()=>{d[key]=type==='checkbox'?input.checked:['access','read','post'].includes(key)?Number(input.value):input.value;ftnRemember();});return input;};
  add('kind','EchoList source','select',[['file','Local file on the Linux computer'],['url','HTTP or HTTPS URL']]);add('source','Explicit EchoList file or URL','text').maxLength=4096;add('encoding','List encoding','select',[['utf8','UTF-8'],['cp437','CP437']]);
  const action=(id,label,fn)=>{const b=element('button',label);b.id=id;b.type='button';b.onclick=()=>ftnWork(fn);$('ftn-fields').append(b);};
  action('ftn-import-load','Load List',ftnStartImport);action('ftn-import-cancel','Cancel Load',ftnCancelImport);
  $('ftn-fields').append(element('p',d.loaded_source?'Reviewed list: '+d.loaded_source:'Choose Load List to read an ordinary file or explicitly selected HTTP(S) URL. Opening this page never downloads, imports or subscribes.'));
  for(const key of ['access','read','post']){const input=add(key,key==='access'?'Access level for selected imports':key==='read'?'Read level for selected imports':'Post level for selected imports','number');input.min=1;input.max=999;}
  add('update_text','Update selected existing names and descriptions','checkbox');
  for(const [label,clear] of [['Select All Entries',false],['Clear All Entries',true]]){const b=element('button',label);b.type='button';b.onclick=()=>{if(ftnState.unconfirmed||ftnState.draft?.state!=='editing')return;d.selected=clear?[]:d.rows.slice(0,16384).map(row=>row.tag);ftnState.unsent=true;ftnRemember();ftnRenderImport();ftnControls();};$('ftn-fields').append(b);}
  const matches=ftnImportRows(),offset=Number(ftnState.offset);for(const row of matches.slice(offset,offset+25)){const label=element('label',row.tag+' — '+row.description,'checkbox-label'),box=document.createElement('input');box.type='checkbox';box.checked=d.selected.includes(row.tag);box.onchange=()=>{const selected=new Set(d.selected);if(box.checked)selected.add(row.tag);else selected.delete(row.tag);d.selected=[...selected];ftnState.unsent=true;ftnRemember();ftnControls();};label.prepend(box);$('ftn-list').append(label);}
  $('ftn-page-count').textContent=d.selected.length+' selected of '+d.rows.length+' usable; '+d.issues.length+' rejected or duplicate lines. '+(matches.length?'Showing '+(offset+1)+'–'+Math.min(offset+25,matches.length)+' of '+matches.length:'No matching entries');
  if(d.issues.length)$('ftn-fields').append(element('p','Source issues: '+d.issues.slice(0,10).map(issue=>'line '+issue.line+': '+issue.reason).join('; ')));
  $('ftn-result').textContent=d.loading?'Loading the selected list. Cancel Load stops this request.':d.detail;
}
async function ftnStartImport(){
  const d=ftnImportData();if(d.loading||ftnState.unconfirmed||ftnState.draft?.state!=='editing')return;d.loading=true;d.job='';ftnRemember();
  try{const value=await api('/api/ftn/list-start',{draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,network:ftnState.network,source:d.source,kind:d.kind,encoding:d.encoding});ftnAcceptList(value);}
  catch(error){if([400,401,403,409].includes(error.status))d.loading=false;d.detail='List load was not confirmed. Status reads the owned request without starting another load. '+error.message;ftnRemember();}
  ftnRenderImport();
}
async function ftnCancelImport(){const d=ftnImportData();if(!d.loading)return;if(!d.job){const status=await api('/api/ftn/list?'+new URLSearchParams({draft:ftnState.draft.draft,list:''}));d.job=status.list;}await api('/api/ftn/list-cancel',{draft:ftnState.draft.draft,list:d.job});d.detail='Cancellation requested. Previous reviewed list and draft remain.';ftnRemember();}
async function ftnPollImport(now=Date.now()){
  if(page!=='config'||route!=='ftn.networks'||ftnState.section!=='import'||!ftnState.draft||ftnBusy||ftnImportPolling||now<ftnImportPollAt||!ftnImportData().loading)return;
  ftnImportPolling=true;ftnImportPollAt=now+500;const handle=ftnState.draft.draft;
  try{const value=await api('/api/ftn/list?'+new URLSearchParams({draft:handle,list:ftnImportData().job}));if(ftnState.draft?.draft===handle&&ftnState.section==='import'){ftnAcceptList(value);if(!value.active)ftnRenderImport();ftnControls();}}
  catch(error){if(ftnState.draft?.draft===handle){const d=ftnImportData();d.detail='The load status could not be read. Your previous list and selections remain. '+error.message;if([400,401,403].includes(error.status))d.loading=false;ftnRemember();$('ftn-result').textContent=d.detail;ftnControls();}}
  finally{ftnImportPolling=false;}
}
async function ftnApplyImport(){
  const d=ftnImportData();if(d.loading||!d.list)throw new Error('Load and review a list before importing.');
  const candidate={draft:ftnState.draft.draft,edit_revision:ftnState.draft.edit_revision,list:d.list,selected:d.selected,access:d.access,read:d.read,post:d.post,update_text:d.update_text};
  const preview=await api('/api/ftn/import-preview',candidate);if(preview.issues.length)throw new Error('Import needs review: '+preview.issues.map(issue=>issue.field+': '+issue.reason).join('; '));
  if(!await ftnPrompt('Review Selected Echo Import',['Source: '+d.loaded_source,preview.selected+' selected; '+preview.created+' new or staged conferences; '+preview.unchanged+' unchanged; '+preview.text_updates+' name/description updates.','Access '+d.access+', Read '+d.read+', Post '+d.post+'. Existing saved conference permissions stay unchanged.','Apply adds these selections to your FTN draft. Final Save is still required; no AreaFix request is sent.'],'Apply to Draft'))return false;
  try{ftnState.draft=await api('/api/ftn/import-apply',candidate);}catch(error){if(![400,401,403,409].includes(error.status))ftnState.unconfirmed=true;ftnRemember();throw error;}
  ftnState.unsent=false;d.detail='Selected echoes applied to the FTN draft. Save FTN Settings publishes them.';ftnRemember();ftnRenderImport();ftnControls();return true;
}

// Held Mail uses saved storage, separate from the unpublished FTN settings draft.
let ftnHeldState=null;
const ftnHeldReasons={'':'All reasons',ftn_recipient_unmatched:'No matching recipient',ftn_recipient_ambiguous:'More than one recipient',ftn_recipient_stale:'Recipient choice changed',ftn_echo_receive_not_authorized:'Echo mapping or permission',ftn_packet_password_wrong:'Packet password wrong',ftn_packet_invalid:'Invalid packet',ftn_message_import_held:'Message import held',ftn_message_identity_or_controls_ambiguous:'Conflicting message identifiers or controls',ftn_message_id_content_collision:'Message identifier reused for different content',ftn_message_charset_unsupported:'Unsupported message character set',ftn_message_body_unsupported:'Message exceeds supported display limits',ftn_tic_password_wrong:'TIC password wrong',ftn_tic_not_authorized:'File echo permission',ftn_tic_mapping_missing:'File echo mapping missing',ftn_tic_continuation_review_required:'Interrupted file needs review',ftn_tic_replaces_required:'Replacement not authorized',ftn_tic_forward_manifest_missing:'Original forwarding plan missing'};
function ftnHeldData(){
  if(ftnHeldState?.key===draftKey)return ftnHeldState;
  const h=ftnHeldState={key:draftKey,query:{network:'',reason:'',find:'',after:''},list:null,detail:null,message:'',recipients:null,recipient:'',recipientFind:'',pending:null,selected:'',notice:''};
  try{const raw=sessionStorage.getItem(draftKey+'-held');if(raw&&raw.length<8192){const saved=JSON.parse(raw);if(saved.pending&&['resolve','reprocess','tic'].includes(saved.pending.kind)&&/^[a-f0-9]{32}$/.test(saved.pending.operation)){h.pending=saved.pending;h.selected=typeof saved.selected==='string'?saved.selected:'';h.notice='An action is unconfirmed. Check Action Result reconciles that same recorded request; it never creates a new operation.';}for(const key of ['network','reason','find'])if(typeof saved.query?.[key]==='string'&&saved.query[key].length<=140)h.query[key]=saved.query[key];}}catch{h.notice='The saved Held Mail request could not be read. Check the original and its recorded decisions before taking another action.';}
  return h;
}
function ftnHeldRemember(){const h=ftnHeldData();try{sessionStorage.setItem(draftKey+'-held',JSON.stringify({pending:h.pending,selected:h.selected,query:h.query}));}catch{throw new Error('This tab could not retain the exact Held Mail request. Keep it open and use Check Action Result.');}}
function ftnHeldButton(parent,id,label,action){const b=element('button',label);b.type='button';b.id=id;b.onclick=()=>ftnWork(action);parent.append(b);return b;}
function ftnHeldField(key,label,value,choices){const l=element('label',label),input=document.createElement(choices?'select':'input');input.id='held-'+key;l.htmlFor=input.id;if(choices){for(const [v,name] of choices){const option=element('option',name);option.value=v;input.append(option);}}else{input.type='text';input.maxLength=140;}input.value=value;$('ftn-fields').append(l,input);return input;}
function ftnHeldMessage(){const h=ftnHeldData();return h.detail?.messages.find(m=>m.operation===h.message);}
function ftnHeldTime(seconds,zone){const date=new Date(Number(seconds)*1000);if(!(Number(seconds)>0)||!Number.isFinite(date.getTime()))return 'Unknown time';try{return date.toLocaleString(undefined,{timeZone:zone||'UTC'});}catch{return date.toISOString();}}
function ftnHeldControls(locked){
  const h=ftnHeldData(),blocked=locked||!!h.pending;
  for(const input of $('ftn-fields').querySelectorAll('input,select'))input.disabled=blocked;
  for(const b of [...$('ftn-fields').querySelectorAll('button'),...$('ftn-list').querySelectorAll('button')])b.disabled=blocked;
  const check=$('held-check');if(check)check.disabled=locked||!h.pending;
}
async function ftnHeldOpen(){const h=ftnHeldData();if(!h.list&&!h.pending)await ftnHeldLoad(false);else await ftnHeldRender();}
async function ftnHeldLoad(next=false){
  const h=ftnHeldData();if(h.pending)throw new Error('Check the exact pending action before reading another selection.');
  if(next){if(!h.list?.more||!h.list.rows.length)return;h.query.after=h.list.rows.at(-1).ingress;}
  else{for(const key of ['network','reason','find'])if($('held-'+key))h.query[key]=$('held-'+key).value;h.query.after='';}
  h.detail=null;h.message='';h.recipients=null;h.recipient='';h.selected='';
  const value=await api('/api/ftn/held?'+new URLSearchParams({...h.query,ingress:'',offset:'0'}));h.list=null;
  if(!value.ok||value.kind!=='review')h.notice='Held Mail could not be read. An empty queue is not claimed.';else{h.list=value;h.notice=value.rows.length?'Select an original to inspect it. Opening a record changes nothing.':'No matching held originals or pending reviewed retries.';}
  ftnHeldRemember();await ftnHeldRender();
}
async function ftnHeldDetail(ingress,offset=0){
  const h=ftnHeldData();if(h.pending)return;h.selected=ingress;h.message='';h.recipients=null;h.recipient='';h.detail=null;
  const value=await api('/api/ftn/held?'+new URLSearchParams({ingress,offset:String(offset),after:'',network:'',reason:'',find:''}));
  if(value.ok&&value.kind==='review'&&value.rows.length===1){h.detail=value;if(h.list)h.list.rows=h.list.rows.map(row=>row.ingress===ingress?value.rows[0]:row);h.notice='Exact original loaded. Recipient choice and reprocessing are separate reviewed actions.';}else h.notice='Original details could not be read. No action was taken.';
  await ftnHeldRender();
}
async function ftnHeldRecipients(next=false){
  const h=ftnHeldData(),m=ftnHeldMessage();if(h.pending||!m?.local_private||m.committed)return;
  if(!next)h.recipientFind=$('held-recipient-find')?.value||'';
  const after=next&&h.recipients?.more?h.recipients.rows.at(-1)?.id:'0';if(!after)return;
  const value=await api('/api/ftn/held/recipients?'+new URLSearchParams({after,find:h.recipientFind}));h.recipients=value.ok&&value.kind==='recipients'?value:null;h.recipient='';h.notice=h.recipients?'Choose an active local recipient. No recipient is selected automatically.':'Active recipients could not be read.';await ftnHeldRender();
}
async function ftnHeldRender(){
  const h=ftnHeldData();$('config-title').textContent='Held Mail';$('config-category').textContent='FTN Networking';$('ftn-location').textContent='Saved mail and settings — unpublished FTN draft edits do not apply here.';$('ftn-search-form').hidden=true;$('ftn-page-count').textContent='';$('ftn-fields').replaceChildren();$('ftn-list').replaceChildren();$('ftn-navigation').replaceChildren();
  ftnHeldButton($('ftn-navigation'),'held-back','Back to Networks',()=>ftnBrowse('networks'));
  const networks=new Map([['','All networks'],...(h.list?.networks||[]).map(n=>[n.code,n.name+' ('+n.code+')'])]);for(const r of h.list?.rows||[])if(r.network&&!networks.has(r.network))networks.set(r.network,r.network);if(h.query.network&&!networks.has(h.query.network))networks.set(h.query.network,h.query.network);
  ftnHeldField('network','Network',h.query.network,[...networks]);const reasons={...ftnHeldReasons};for(const r of h.list?.rows||[])if(r.reason&&!reasons[r.reason])reasons[r.reason]=r.reason;if(h.query.reason&&!reasons[h.query.reason])reasons[h.query.reason]=h.query.reason;
  ftnHeldField('reason','Reason',h.query.reason,Object.entries(reasons));ftnHeldField('find','Find',h.query.find);ftnHeldButton($('ftn-fields'),'held-reload','Find / Reload',()=>ftnHeldLoad(false));if(h.list?.more)ftnHeldButton($('ftn-fields'),'held-next','Next Originals',()=>ftnHeldLoad(true));
  $('ftn-page-count').textContent=h.list?.time_zone?'Times use the BBS time zone: '+h.list.time_zone:'Times shown in UTC; the BBS time zone is unavailable.';
  for(const r of h.list?.rows||[]){const date=ftnHeldTime(r.received,h.list.time_zone);ftnHeldButton($('ftn-list'),'held-row-'+r.ingress,date+' | '+(networks.get(r.network)||r.network||'Unconfigured')+' | '+(r.from||r.link||'Unknown sender')+' | '+(r.retry_pending?'Reviewed retry pending':ftnHeldReasons[r.reason]||r.reason||'Held original'),()=>ftnHeldDetail(r.ingress));}
  if(h.detail){const d=h.detail,r=d.rows[0];$('ftn-fields').append(element('h3','Original: '+r.file),element('p','Network '+(r.network||'Unconfigured')+'; connection '+(r.link||'Unconfigured')+'; reason '+(ftnHeldReasons[r.reason]||r.reason||'Not recorded')+'.'));
    if(d.recovery_required||d.mode===2)$('ftn-fields').append(element('p',d.recovery_required?'Message ID history requires recovery. New review actions are held.':'The mailer is disabled. New review actions are held.'));
    if(d.tic){const t=d.tic;$('ftn-fields').append(element('p','TIC From (claimed): '+(t.claimed_from||'Not recorded')+'; echo '+(t.echo||'Not recorded')+'; payload '+(t.payload||'Not recorded')+'.'),element('p','Publication: '+(t.publication==='0'.repeat(32)?'No publication started':['Reserved','Copy in progress','New file staged','Recovery in progress','Previous file retained in recovery','New file placed; catalog pending','Catalog committed'][t.phase]||'Unavailable')+'; original forwarding plan '+(t.forward_plan_present?'retained':'missing')+'. Current credentials, Receive permissions, original files and retained plan are checked again. Missing plans or changed files stay held.'));
    }else{const selected=ftnHeldField('message','Message in selected original',h.message,[['','Choose a message'],...d.messages.map(m=>[m.operation,'Message '+(m.index+1)+': '+m.to+(m.committed?' (committed)':'')])]);selected.onchange=()=>ftnWork(async()=>{h.message=selected.value;h.recipients=null;h.recipient='';await ftnHeldRender();});if(d.more&&d.messages.length)ftnHeldButton($('ftn-fields'),'held-more-messages','More Messages',()=>ftnHeldDetail(r.ingress,d.messages.at(-1).index+1));
      const m=ftnHeldMessage();if(m){$('ftn-fields').append(element('p','From: '+m.from+' ('+m.origin+'); original To: '+m.to+' ('+m.destination+').'),element('p','Intended echo: '+(m.echo||'Private NetMail')+'; first rejection: '+(ftnHeldReasons[m.reason]||m.reason)+'.'));if(m.resolution)$('ftn-fields').append(element('p','Reviewed local recipient: account '+m.resolution.recipient+', revision '+m.resolution.recipient_revision+'. Original To stays unchanged.'));if(m.committed)$('ftn-fields').append(element('p','This message is already committed and cannot be reassigned.'));
        if(m.local_private&&!m.committed&&r.state===3&&!r.retry_pending&&!d.recovery_required&&d.mode!==2){ftnHeldField('recipient-find','Find active local recipient',h.recipientFind);ftnHeldButton($('ftn-fields'),'held-find-recipients','Find Recipients',()=>ftnHeldRecipients(false));if(h.recipients){const input=ftnHeldField('recipient','Choose intended local recipient',h.recipient,[['','Choose a recipient'],...h.recipients.rows.map(a=>[a.id,a.name+' (account '+a.id+')'])]);input.onchange=()=>{h.recipient=input.value;};if(h.recipients.more)ftnHeldButton($('ftn-fields'),'held-more-recipients','More Recipients',()=>ftnHeldRecipients(true));ftnHeldButton($('ftn-fields'),'held-resolve','Review Recipient...',ftnHeldResolve);}}
      }
    }
    const retry=d.tic?.retry||d.retry;if(retry)$('ftn-fields').append(element('p','Last reviewed retry: '+(!retry.finished?'Queued / processing':retry.processed?'Processed':'Not processed')+(retry.reason?' ('+retry.reason+')':'')+'.'));
    if(!d.recovery_required&&d.mode!==2&&!r.retry_pending&&(r.tic?r.state!==4:r.state===3&&r.message_original))ftnHeldButton($('ftn-fields'),'held-reprocess','Reprocess Original...',ftnHeldReprocess);
    ftnHeldButton($('ftn-fields'),'held-detail-reload','Reload Details',()=>ftnHeldDetail(r.ingress));
  }
  ftnHeldButton($('ftn-fields'),'held-check','Check Action Result',ftnHeldSubmit);$('ftn-result').textContent=h.notice;ftnControls();
}
async function ftnHeldResolve(){
  const h=ftnHeldData(),m=ftnHeldMessage(),a=h.recipients?.rows.find(a=>a.id===h.recipient),d=h.detail;if(h.pending||!m?.local_private||m.committed||!a)throw new Error('Select the private message and an active local recipient first.');
  if(!await ftnPrompt('Review Local Recipient',['Original From: '+m.from+'; original To: '+m.to+'.','Choose '+a.name+' (account '+a.id+') as this private message’s local recipient?','The original To and packet bytes stay unchanged. Reprocess Original is a separate action.'],'Save Recipient Choice'))return;
  const token=await api('/api/ftn/held/token',{});h.pending={kind:'resolve',operation:token.operation,held_message:m.operation,previous:m.resolution?.operation||'0'.repeat(32),revision:d.revision,recipient:a.id,recipient_revision:a.revision};ftnHeldRemember();await ftnHeldSubmit();
}
async function ftnHeldReprocess(){
  const h=ftnHeldData(),d=h.detail,r=d?.rows[0];if(h.pending||!r||r.retry_pending||d.recovery_required||d.mode===2)return;
  if(!await ftnPrompt(d.tic?'Review Reprocess TIC':'Review Reprocess Original',['File: '+r.file+'; reason '+(ftnHeldReasons[r.reason]||r.reason)+'.',d.tic?'Retry the exact retained TIC and its original publication? Current credentials, permissions, original files and forwarding plan are checked again. Changed or missing objects stay held.':'Reprocess this exact retained original under current credentials, admission, mappings and recipient decisions? Already committed messages are not delivered twice.','This queues one reviewed attempt. It does not promise completion or send the original again.'],'Queue Reviewed Retry'))return;
  const token=await api('/api/ftn/held/token',{});h.pending={kind:d.tic?'tic':'reprocess',operation:token.operation,ingress:r.ingress,previous:(d.tic?.retry||d.retry)?.operation||'0'.repeat(32),revision:d.revision};if(d.tic)h.pending.review=d.tic.review;ftnHeldRemember();await ftnHeldSubmit();
}
async function ftnHeldSubmit(){
  const h=ftnHeldData();if(!h.pending)return;const request=structuredClone(h.pending);ftnHeldRemember();
  let result;try{result=await api('/api/ftn/held/action',request);}catch{h.notice='Action result is unconfirmed. Check Action Result reconciles the same retained operation. Do not create a new action.';await ftnHeldRender();return;}
  if(result.unconfirmed||result.ok&&(result.kind!==request.kind||result.result?.operation!==request.operation)){h.notice='The exact action result could not be verified. Use Check Action Result; the request remains retained.';await ftnHeldRender();return;}
  h.pending=null;ftnHeldRemember();const message=!result.ok?result.busy?'The writer is busy. Reload Details and review before a new attempt.':'The original, settings or recipient changed. Reload Details and review again.':request.kind==='resolve'?'Recipient choice saved. Original To is unchanged. Reprocessing remains a separate action.':result.result.finished?result.result.processed?'This exact reviewed retry completed and processed the original.':'This exact reviewed retry did not process the original. Review its current reason.':'Reviewed retry queued. It has not processed the original; the BBS will attempt it after the next enabled start.';
  if(h.selected)await ftnHeldDetail(h.selected);h.notice=message;await ftnHeldRender();
}

// Control Panel caller editing, reviewed notices and private Sysop chat.
let cpCallerAccess=0;
let cpCallerState=null,cpCallerReading=false,cpCallerLoaded=false;
let cpNoticeReview=null,cpNoticeTarget=null,cpNoticeBusy=false,cpNoticeUnconfirmed=false;
let cpChatState=null,cpChatBusy=false,cpChatReading=false,cpChatAck='0',cpChatHistory=[],cpChatPending=null;
async function cpCallerApi(path,data){
  const access=cpCallerAccess;
  if(!authenticated||maintenanceActive)throw new Error('Caller controls are unavailable.');
  const result=await api(path,data);
  if(access!==cpCallerAccess||!authenticated)throw new Error('The browser session changed.');
  return result;
}
function cpClearChat(){
  cpChatState=null;cpChatPending=null;cpChatAck='0';cpChatHistory=[];
  $('cp-chat-history').replaceChildren();$('cp-chat-text').value='';$('cp-chat-panel').hidden=true;
}
function cpClearCallerViews(){
  ++cpCallerAccess;cpClearObservations();cpClearBinkp();cpClearHelp();cpClearMonitor();fmClear();
  cpCallerState=null;cpCallerLoaded=false;cpNoticeReview=null;cpNoticeTarget=null;cpNoticeUnconfirmed=false;
  cpChatState=null;cpChatPending=null;cpChatAck='0';cpChatHistory=[];
  $('cp-chat-history').replaceChildren();$('cp-chat-text').value=$('cp-notice-text').value='';
  $('cp-chat-panel').hidden=$('cp-notice-panel').hidden=true;
}
function cpCallerButtons(){
  const caller=cpCurrentNode(),signed=caller?.connected&&caller.account&&caller.account!=='0';
  const blocked=!authenticated||busy||maintenanceActive||!cpCallerState?.ready;
  $('cp-edit-caller').disabled=blocked||!signed;$('cp-caller-notice').disabled=blocked||!signed||cpNoticeBusy;
  $('cp-chat-caller').disabled=blocked||!signed||cpChatBusy||!!cpChatState;
  $('cp-notice-open').disabled=blocked||cpNoticeBusy;$('cp-availability').disabled=blocked;
  cpNoticeRender();cpRenderChat(false);
  $('cp-availability').textContent=cpCallerState?.available?'Mark Unavailable for Chat':'Mark Available for Chat';
}
async function cpRefreshCallers(){
  if(!authenticated||cpCallerReading||maintenanceActive)return;cpCallerReading=true;
  try{
    cpCallerState=await cpCallerApi('/api/control/callers');
    $('cp-chat-status').textContent=!cpCallerState.ready?'Start the BBS to use caller chat and notices.':cpCallerState.available?'Sysop is available for pages.':'Sysop is unavailable for pages.';
    $('cp-pages').replaceChildren();
    for(const entry of cpCallerState.pages||[]){
      const row=element('div','','activity-row'),answer=element('button','Answer Page');
      answer.disabled=busy||cpChatBusy||!!cpChatState;answer.onclick=()=>cpStartChat(entry.target,entry.page,entry.name);
      row.append(element('span','Node '+entry.target.node),element('span',entry.name),answer);$('cp-pages').append(row);
    }
    cpCallerButtons();
    if(!cpCallerLoaded){
      cpCallerLoaded=true;const retained=await cpCallerApi('/api/control/notice');
      if(retained.found){cpNoticeReview=retained;cpNoticeTarget=retained.target;cpNoticeUnconfirmed=retained.submitted&&!retained.completed;
        if(cpNoticeUnconfirmed||!retained.submitted){cpNoticeShow();cpNoticeRender();}}
    }
    await cpPollChat();
  }catch(error){$('cp-chat-status').textContent='Caller controls unavailable. '+error.message;cpCallerState=null;cpCallerButtons();}
  finally{cpCallerReading=false;}
}
async function cpEditCaller(){
  const current=cpCurrentNode();if(!current?.account||current.account==='0'||busy)return;
  if(hasDraft()){notice('Save or discard your current settings draft before opening this caller.');return;}
  const target={...current,epoch:cpSelection.epoch};busy=true;
  try{const result=await cpCallerApi('/api/control/caller-account',{target:cpIdentity(target),account:target.account});await showPage('config','users.accounts',result.account,{prefix:'',offset:0});}
  catch(error){notice(error.message);}finally{busy=false;if(page==='config')setDirty(dirty);cpCallerButtons();}
}
async function cpAvailability(){if(busy||!cpCallerState?.ready)return;await action(()=>cpCallerApi('/api/control/available',{available:!cpCallerState.available}));}
function cpNoticeShow(){
  const select=$('cp-notice-scope');select.replaceChildren(element('option','All Callers'));select.children[0].value='all';
  if(cpNoticeTarget){const option=element('option','Selected caller — Node '+cpNoticeTarget.node);option.value='selected';select.append(option);}
  for(const room of cpCallerState?.rooms||[]){const option=element('option','Chat Room: '+room.name);option.value='room:'+room.id;select.append(option);}
  select.value=cpNoticeReview?.target?'selected':cpNoticeReview?.room?'room:'+cpNoticeReview.room:cpNoticeTarget?'selected':'all';
  if(cpNoticeReview)$('cp-notice-text').value=cpNoticeReview.text;
  $('cp-notice-panel').hidden=false;cpNoticeRender();
}
function cpNoticeOpen(selected=false){
  if(cpNoticeBusy||busy)return;if(cpNoticeUnconfirmed){cpNoticeShow();return;}
  const caller=selected?cpCurrentNode():null;if(selected&&(!caller?.connected||!caller.account||caller.account==='0'))return;
  cpNoticeReview=null;cpNoticeTarget=caller?cpIdentity({...caller,epoch:cpSelection.epoch}):null;
  $('cp-notice-text').value='';$('cp-notice-result').textContent='';cpNoticeShow();
}
function cpNoticeRender(){
  const blocked=!authenticated||maintenanceActive||cpNoticeBusy||!!cpNoticeReview||cpNoticeUnconfirmed;
  $('cp-notice-text').disabled=$('cp-notice-scope').disabled=$('cp-notice-review').disabled=blocked;
  $('cp-notice-send').disabled=!authenticated||maintenanceActive||cpNoticeBusy||!cpNoticeReview||cpNoticeReview.submitted||cpNoticeUnconfirmed;
  $('cp-notice-check').disabled=$('cp-notice-close').disabled=cpNoticeBusy;
  $('cp-notice-override').disabled=!authenticated||maintenanceActive||cpNoticeBusy;
  $('cp-notice-override').hidden=!(cpNoticeReview?.completed&&cpNoticeReview.code===3&&cpNoticeReview.target&&!cpNoticeReview.override_dnd);
  if(cpNoticeReview)$('cp-notice-result').textContent=cpNoticeReview.detail+(cpNoticeReview.override_dnd?' Do Not Disturb override applies to this notice only.':'');
}
async function cpPrepareNotice(override=false){
  if(cpNoticeBusy||cpNoticeUnconfirmed||(!override&&cpNoticeReview))return;cpNoticeBusy=true;cpNoticeRender();
  try{
    let request;
    if(override){
      if(!cpNoticeReview?.target||!await offlineAsk('Override Do Not Disturb?','Send this notice to the same selected caller.','This applies to this notice only. Review it again before Send.','Review Override'))return;
      request={target:cpNoticeReview.target,room:0,text:cpNoticeReview.text,override_dnd:true};
    }else{const scope=$('cp-notice-scope').value;request={target:scope==='selected'?cpNoticeTarget:null,room:scope.startsWith('room:')?Number(scope.slice(5)):0,text:$('cp-notice-text').value,override_dnd:false};}
    const result=await cpCallerApi('/api/control/notice/prepare',request);if(!result.found)throw new Error('The notice could not be prepared.');cpNoticeReview=result;
  }catch(error){$('cp-notice-result').textContent=error.message;}finally{cpNoticeBusy=false;cpNoticeRender();}
}
async function cpSendNotice(){
  if(cpNoticeBusy||!cpNoticeReview||cpNoticeReview.submitted||cpNoticeUnconfirmed)return;
  cpNoticeBusy=true;cpNoticeUnconfirmed=true;cpNoticeRender();
  try{const result=await cpCallerApi('/api/control/notice/send',{handle:cpNoticeReview.handle});if(!result.found)throw new Error('This notice receipt is unavailable.');cpNoticeReview=result;cpNoticeUnconfirmed=result.submitted&&!result.completed;}
  catch{cpNoticeUnconfirmed=true;}
  finally{cpNoticeBusy=false;cpNoticeRender();if(cpNoticeUnconfirmed)$('cp-notice-result').textContent='Send result is unconfirmed. Use Check Send Result; no notice is sent again automatically.';}
}
async function cpCheckNotice(){
  if(cpNoticeBusy)return;cpNoticeBusy=true;cpNoticeRender();let detail='';
  try{const result=await cpCallerApi('/api/control/notice');
    if(result.found&&(!cpNoticeReview||result.handle===cpNoticeReview.handle)){cpNoticeReview=result;cpNoticeUnconfirmed=result.submitted&&!result.completed;}
    else detail='This notice receipt is no longer available. Its outcome cannot be confirmed. No notice was resent.';
  }catch(error){detail='Send result is still unavailable. '+error.message;}
  finally{cpNoticeBusy=false;cpNoticeRender();if(detail)$('cp-notice-result').textContent=detail;}
}
async function cpCloseNotice(){
  if(cpNoticeBusy)return;if(cpNoticeUnconfirmed){$('cp-notice-panel').hidden=true;return;}cpNoticeBusy=true;
  try{if(cpNoticeReview&&!cpNoticeReview.submitted)await cpCallerApi('/api/control/notice/cancel',{handle:cpNoticeReview.handle});cpNoticeReview=null;cpNoticeTarget=null;$('cp-notice-text').value='';$('cp-notice-panel').hidden=true;}
  catch(error){$('cp-notice-result').textContent=error.message;}finally{cpNoticeBusy=false;cpNoticeRender();}
}
function cpRenderChat(updateButtons=true){
  $('cp-chat-panel').hidden=!authenticated||!cpChatState;if(!cpChatState)return;
  $('cp-chat-title').textContent='Chat with '+(cpChatState.name||'Caller');
  $('cp-chat-detail').textContent=cpChatPending?'Message result needs checking. It will not be sent again automatically.':cpChatState.code!==0?'Chat is unavailable or ended.':cpChatState.pending?"Connecting at the caller's safe screen…":cpChatState.active?(cpChatState.paused?"Private Sysop chat — the caller's calling time is paused.":"Answering the caller's page — normal calling time applies."):'Chat ended.';
  $('cp-chat-text').disabled=$('cp-chat-send').disabled=!authenticated||maintenanceActive||cpChatBusy||!!cpChatPending||!cpChatState.active||cpChatState.code!==0;
  $('cp-chat-check').disabled=$('cp-chat-close').disabled=cpChatBusy;$('cp-chat-retry').hidden=!cpChatPending;$('cp-chat-retry').disabled=!authenticated||maintenanceActive||cpChatBusy||!cpChatState.active;
  $('cp-chat-history').replaceChildren(...cpChatHistory.map(line=>element('p',line.text)));if(updateButtons)cpCallerButtons();
}
async function cpPollChat(){
  if(cpChatReading||cpChatBusy||!authenticated||maintenanceActive)return;cpChatReading=true;
  try{
    const result=await cpCallerApi('/api/control/chat?ack='+encodeURIComponent(cpChatAck));
    if(!result.present){if(cpChatState){cpChatState={...cpChatState,active:false,pending:false,code:1,absent:true};cpRenderChat();}return;}
    if(cpChatState?.handle!==result.handle){cpChatAck='0';cpChatHistory=[];cpChatPending=null;}
    cpChatState=result;
    for(const line of result.lines||[])if(BigInt(line.sequence)>BigInt(cpChatAck)){cpChatHistory.push(line);cpChatAck=line.sequence;}
    cpChatHistory=cpChatHistory.slice(-128);
    if(cpChatPending&&BigInt(result.next_send)>BigInt(cpChatPending.sequence)){cpChatPending=null;$('cp-chat-text').value='';}
    cpRenderChat();
  }catch(error){if(cpChatState){cpChatState.active=false;cpRenderChat();$('cp-chat-detail').textContent='Chat connection unavailable. '+error.message;}}
  finally{cpChatReading=false;}
}
async function cpStartChat(target=null,pageId='0',name=''){
  if(busy||cpChatBusy||cpChatState)return;
  const caller=cpCurrentNode();if(!target){if(!caller?.account||caller.account==='0')return;target=cpIdentity({...caller,epoch:cpSelection.epoch});name=caller.caller;}
  busy=cpChatBusy=true;cpCallerButtons();
  try{
    if(!await offlineAsk(pageId==='0'?'Chat with this caller?':'Answer this page?',name+' — Node '+target.node,pageId==='0'?"The caller's calling time pauses during this Sysop chat.":"Normal calling time applies while answering a caller's page.",'Start Chat'))return;
    const request={target,page:pageId,override_dnd:false};let result=await cpCallerApi('/api/control/chat/start',request);
    if(result.code===3&&await offlineAsk('Override Do Not Disturb?',name+' — Node '+target.node,'This applies to this conversation only.','Start This Chat'))result=await cpCallerApi('/api/control/chat/start',{...request,override_dnd:true});
    if(result.present&&result.code===0){cpChatState=result;cpChatAck='0';cpChatHistory=[];cpChatPending=null;cpRenderChat();}
    else notice(result.detail||'The caller or page is no longer available.');
  }catch(error){notice('Chat start was not confirmed. Check Chat before trying again. '+error.message);}
  finally{busy=cpChatBusy=false;await cpPollChat();cpCallerButtons();}
}
async function cpSendChat(retry=false){
  if(!authenticated||maintenanceActive||cpChatBusy||!cpChatState||(!retry&&cpChatPending))return;
  if(!retry){if(!cpChatState.active||!$('cp-chat-text').value)return;cpChatPending={handle:cpChatState.handle,sequence:cpChatState.next_send,text:$('cp-chat-text').value};}
  if(!cpChatPending)return;cpChatBusy=true;cpRenderChat();let detail='';
  try{await cpCallerApi('/api/control/chat/send',cpChatPending);cpChatPending=null;$('cp-chat-text').value='';}
  catch(error){if(error.status){cpChatPending=null;detail=error.message;}}
  finally{cpChatBusy=false;await cpPollChat();cpRenderChat();if(detail)$('cp-chat-detail').textContent=detail;}
}
async function cpEndChat(){
  if(cpChatBusy||!cpChatState)return;if(cpChatState.absent){cpClearChat();cpCallerButtons();return;}cpChatBusy=true;cpRenderChat();
  try{await cpCallerApi('/api/control/chat/end',{handle:cpChatState.handle});cpClearChat();}
  catch(error){$('cp-chat-detail').textContent='Chat close was not confirmed. Check Chat. '+error.message;}finally{cpChatBusy=false;cpCallerButtons();}
}
$('cp-edit-caller').onclick=cpEditCaller;$('cp-caller-notice').onclick=()=>cpNoticeOpen(true);$('cp-notice-open').onclick=()=>cpNoticeOpen(false);$('cp-chat-caller').onclick=()=>cpStartChat();$('cp-availability').onclick=cpAvailability;
$('cp-notice-form').onsubmit=event=>{event.preventDefault();cpPrepareNotice();};$('cp-notice-send').onclick=cpSendNotice;$('cp-notice-check').onclick=cpCheckNotice;$('cp-notice-close').onclick=cpCloseNotice;$('cp-notice-override').onclick=()=>cpPrepareNotice(true);
$('cp-chat-form').onsubmit=event=>{event.preventDefault();cpSendChat();};$('cp-chat-check').onclick=cpPollChat;$('cp-chat-retry').onclick=()=>cpSendChat(true);$('cp-chat-close').onclick=cpEndChat;

// Read-only Control Panel observations. Clear View never alters server logs.
let cpObservationView='',cpObservationReading=false,cpObservationData=null;
let cpLogEpoch='',cpLogCursor='0',cpLogNewest='0',cpLogRows=[],cpLogPaused=false,cpLogGap=false,cpLogTruncated=false,cpLogReading=false,cpLogGeneration=0;
function cpClearObservations(){
  ++cpLogGeneration;cpObservationData=null;cpLogEpoch='';cpLogCursor=cpLogNewest='0';cpLogRows=[];cpLogGap=cpLogTruncated=false;
  $('cp-observation-body').replaceChildren();$('cp-observation-detail').textContent='';
}
function cpObservationTime(seconds){
  if(seconds==='0')return 'Never';const date=new Date(Number(seconds)*1000);if(!Number.isFinite(date.getTime()))return 'Time unavailable';
  try{return new Intl.DateTimeFormat('en-CA',{timeZone:cpObservationData?.zone||'UTC',dateStyle:'short',timeStyle:'medium',hourCycle:'h23'}).format(date);}
  catch{return date.toISOString().replace('T',' ').replace('.000Z',' UTC');}
}
function cpObservationTable(headers,rows){
  const table=element('table','','cp-data-table'),head=element('tr');for(const label of headers)head.append(element('th',label));const thead=element('thead');thead.append(head);table.append(thead);const body=element('tbody');
  for(const row of rows){const tr=element('tr');for(const value of row)tr.append(element('td',String(value)));body.append(tr);}table.append(body);return table;
}
function cpLogText(row){return cpObservationTime(row.utc)+'  '+row.level+'  '+row.component+'  '+row.message;}
function cpRenderObservations(){
  const body=$('cp-observation-body');body.replaceChildren();const data=cpObservationData;
  $('cp-log-controls').hidden=cpObservationView!=='logs';$('cp-log-pause').textContent=cpLogPaused?'Resume':'Pause';
  if(cpObservationView==='logs'){
    const filter=Number($('cp-log-filter').value||0),rows=cpLogRows.filter(row=>!filter||row.group===filter);
    const log=element('pre',rows.length?rows.map(cpLogText).join('\n'):'No entries in this view.','cp-log-text');body.append(log);
    $('cp-observation-detail').textContent=(cpLogPaused?'Paused for reading. ':'Following new entries. ')+(cpLogGap?'Some older entries are no longer retained by the Runtime. ':'')+(cpLogTruncated?'The browser keeps only the most recent 1,000 entries. ':'')+(data?'Log recording: '+data.log_health+'. ':'')+'Clear View clears this display only.';
    if(!cpLogPaused)log.scrollTop=log.scrollHeight;return;
  }
  if(!data){body.append(element('p','Observations are unavailable.'));return;}
  $('cp-observation-detail').textContent='BBS time zone: '+(data.zone||'unavailable')+'. Log recording: '+data.log_health+'.';
  if(cpObservationView==='events'){
    body.append(element('p','Recent system events recorded by this Runtime. Live Logs contains the retained connection and caller history.'));
    body.append(element('pre',data.events.length?data.events.map(cpLogText).join('\n'):'No system events recorded yet.','cp-log-text'));
  }else if(cpObservationView==='statistics'){
    const stats=data.statistics;body.append(element('h3','Saved BBS Statistics'));
    body.append(cpObservationTable(['Statistic','Today','Total'],[['Successful logons',stats.logons],['Caller time (seconds)',stats.seconds],['New users',stats.users]].map(([name,value])=>[name,value.available?value.today:'Unavailable',value.available?value.total:'Unavailable'])));
    body.append(element('p','Today follows the BBS time zone. Caller time reflects the latest committed accounting update.'));
    body.append(element('h3','Recorded This Runtime'),element('p','These counters reset when the management Runtime restarts.'));
    body.append(cpObservationTable(['Observation','Count'],data.counters.map(row=>[row.label,row.value===null?'Unavailable':row.value])));
  }else if(cpObservationView==='botgate'){
    const gate=data.botgate,on=value=>value?'On':'Off';body.append(element('p',data.running?'Current BBS protection state.':'BBS stopped. Saved participation choices are shown; no active filtering or feed readiness is implied.'));
    body.append(cpObservationTable(['Protection','State'],[['Global',on(gate.enabled)],['Telnet participation',on(gate.telnet)],['New SSH registration challenge',on(gate.ssh)],['Recording health',data.log_health],['Rules',data.running?(gate.rules_ready?'Last good rules active: ':'Unavailable: ')+gate.rule_status:'Not running'],['Geo files',gate.geo||'None'],['Downloaded feed',data.running?(gate.feed_available?gate.feed_entries+' addresses':'Unavailable; no valid cached feed'):'Not running'],['Last successful feed update',cpObservationTime(gate.last_success)]]));
    body.append(element('h3','Recorded This Runtime'),element('p','Counter epoch began '+cpObservationTime(gate.started)+'.'));
    body.append(cpObservationTable(['Observation','Count'],['Connections checked','Challenge passed','Challenge failed','Challenge timed out','Blocked','Filter exemptions (challenge still required)','Challenges active now'].map((label,i)=>[label,gate.counts[i]])));
    body.append(cpObservationTable(['Rule list','Entries'],['Exempt','Manual IP','Silent IP','Verified hostname','Temporary (may include expired)','Feed exceptions'].map((label,i)=>[label,data.running?gate.rules[i]:'Unavailable'])));
    if(gate.updating||gate.progress)body.append(element('pre',gate.progress||gate.feed_result,'cp-log-text'));
    body.append(element('h3','Recent BotGate Activity'),element('pre',gate.recent.length?gate.recent.map(cpLogText).join('\n'):'No recent BotGate events.','cp-log-text'));
    const settings=element('button','Open BBS Security Settings');settings.onclick=()=>navigate('config','security.botgate');body.append(settings);
  }
}
async function cpReadLogs(){
  if(cpLogReading||cpLogPaused||cpObservationView!=='logs')return;
  cpLogReading=true;const generation=cpLogGeneration,after=cpLogCursor;
  try{
    const page=await cpCallerApi('/api/control/logs?epoch='+encodeURIComponent(cpLogEpoch)+'&after='+after);
    if(generation!==cpLogGeneration||cpLogPaused||cpObservationView!=='logs'||after!==cpLogCursor)return;
    if(page.reset){cpLogEpoch=page.epoch;cpLogCursor=cpLogNewest='0';cpLogRows=[];cpLogGap=true;}
    else{
      if(page.after!==after||BigInt(page.next)<BigInt(after)||BigInt(page.newest)<BigInt(page.next))throw new Error('Live log cursor changed unexpectedly.');
      cpLogEpoch=page.epoch;cpLogCursor=page.next;cpLogNewest=page.newest;cpLogGap=cpLogGap||page.gap;
      for(const row of page.entries)if(BigInt(row.sequence)>BigInt(after))cpLogRows.push(row);
      if(cpLogRows.length>1000){cpLogRows=cpLogRows.slice(-1000);cpLogTruncated=true;}
    }
    cpRenderObservations();
  }catch(error){$('cp-observation-detail').textContent='Live Logs unavailable. '+error.message;}
  finally{cpLogReading=false;}
}
async function cpRefreshObservations(){
  if(!cpObservationView||cpObservationReading||!authenticated||maintenanceActive||(cpObservationView==='logs'&&cpLogPaused))return;cpObservationReading=true;
  try{cpObservationData=await cpCallerApi('/api/control/observations');cpRenderObservations();await cpReadLogs();}
  catch(error){$('cp-observation-detail').textContent='Observations unavailable. '+error.message;}
  finally{cpObservationReading=false;}
}
async function cpOpenObservations(view){
  cpObservationView=view;$('cp-observations').hidden=false;$('cp-observation-title').textContent={events:'Events',logs:'Live Logs',statistics:'Statistics',botgate:'BotGate Status'}[view];cpRenderObservations();await cpRefreshObservations();
}
function cpPauseLogs(){cpLogPaused=!cpLogPaused;++cpLogGeneration;cpRenderObservations();if(!cpLogPaused)void cpReadLogs();}
function cpClearLogView(){++cpLogGeneration;cpLogCursor=cpLogNewest;cpLogRows=[];cpLogGap=cpLogTruncated=false;cpRenderObservations();}
for(const view of ['events','logs','statistics','botgate'])$('cp-open-'+view).onclick=()=>cpOpenObservations(view);
$('cp-log-pause').onclick=cpPauseLogs;$('cp-log-clear').onclick=cpClearLogView;$('cp-log-filter').onchange=cpRenderObservations;
$('cp-observation-refresh').onclick=cpRefreshObservations;$('cp-observation-close').onclick=()=>{cpObservationView='';++cpLogGeneration;$('cp-observations').hidden=true;};


// Native BinkP observations and reviewed, single-dispatch actions.
let cpBinkpOpen=false,cpBinkpData=null,cpBinkpReading=false,cpBinkpBusy=false,cpBinkpLoaded=false,cpBinkpReceipt=null,cpBinkpUnknown=false;
let cpBinkpSelected='',cpBinkpFollow=true,cpBinkpQueue=null,cpBinkpOffset=0,cpBinkpConnections=null,cpBinkpConnectionOffset=0,cpBinkpSample=null,cpBinkpRates=null,cpBinkpActivityPaused=false;
function cpClearBinkp(){cpBinkpOpen=false;cpBinkpData=cpBinkpReceipt=cpBinkpQueue=cpBinkpConnections=cpBinkpSample=cpBinkpRates=null;cpBinkpLoaded=cpBinkpUnknown=false;cpBinkpSelected='';cpBinkpFollow=true;cpBinkpOffset=cpBinkpConnectionOffset=0;$('cp-binkp').hidden=true;$('cp-binkp-body').replaceChildren();$('cp-binkp-queue-body').replaceChildren();$('cp-binkp-session').replaceChildren();$('cp-binkp-connection').replaceChildren();$('cp-binkp-connection-panel').hidden=true;$('cp-binkp-activity').textContent=$('cp-binkp-action-result').textContent='';}
function cpBinkpReady(action='poll'){const d=cpBinkpData;return !!(authenticated&&!maintenanceActive&&!busy&&!cpBinkpBusy&&!cpBinkpUnknown&&d?.running&&d.code===0&&(d.mode===0||(action==='scan'&&d.mode===1))&&d.capable&&!d.paused&&!d.serial_recovery_required&&d.active_revision===d.saved_revision&&d.saved_revision!=='0');}
function cpBinkpButtons(){const blocked=!authenticated||maintenanceActive||cpBinkpBusy||busy;$('cp-binkp-poll').disabled=!cpBinkpReady();$('cp-binkp-scan').disabled=!cpBinkpReady('scan');$('cp-binkp-queues').disabled=blocked;$('cp-binkp-check').disabled=blocked;$('cp-binkp-check').hidden=!cpBinkpReceipt&&!cpBinkpUnknown;$('cp-binkp-previous').disabled=blocked||cpBinkpOffset===0;$('cp-binkp-next').disabled=blocked||!cpBinkpQueue?.more;}
function cpBinkpReceiptText(value){
  if(!value?.found)return 'No retained action receipt. This does not prove that an earlier unconfirmed request never ran. Check the session history before requesting work again.';
  if(!value.submitted)return 'Prepared only. Nothing has been requested.';
  if(!value.completed)return 'Result unconfirmed. Check Action Result reads the same receipt; it does not repeat the request. Check the activity and queues before taking further action.';
  return value.result.code===0?(value.action==='poll'?'Poll requested. Watch session activity for the result; delivery is not yet confirmed.':'Outbound scan requested. Check the queues and activity; delivery is not yet confirmed.'):'The request was refused or is unavailable. Refresh the console and check FTN settings.';
}
function cpBinkpApplyReceipt(value){cpBinkpReceipt=value?.found?value:null;cpBinkpUnknown=!!(value?.found&&value.submitted&&!value.completed);$('cp-binkp-action-result').textContent=cpBinkpReceiptText(value);cpBinkpButtons();}
function cpBinkpTransfer(item,sending){
  const p=item?.[sending?'send':'receive'],title=sending?'Send':'Receive',box=element('section','','card');box.append(element('h3',title));
  if(!p?.name){box.append(element('p',item?.active?'Waiting for a file.':'Idle. No file transfer.'));return box;}
  const size=BigInt(p.size),position=BigInt(p.position),percent=size?Number(position*100n/size):p.active?0:100;
  const progress=element('progress');progress.max=100;progress.value=Math.min(100,percent);progress.setAttribute('aria-label',title+' progress');box.append(progress);
  const state=!item.active?(item.completed?'Session completed':'Session ended'):p.awaiting_ack?'Awaiting acknowledgement':!p.active?'File finished':cpBinkpRates?cpBinkpRates[sending?'send':'receive']+' bytes/second':'Measuring speed...';
  box.append(element('p',p.name),element('p',p.position+' / '+p.size+' bytes ('+Math.min(100,percent)+'%)'),element('p',state));return box;
}
function cpBinkpRender(){
  const body=$('cp-binkp-body');body.replaceChildren();const d=cpBinkpData;cpBinkpButtons();
  if(!d||d.code!==0){body.append(element('p','FTN Runtime status is unavailable. No empty queue or successful delivery is implied.'));return;}
  body.append(element('p',!d.running?'BBS stopped. Saved mail and settings remain.':d.serial_recovery_required?'FTN is stopped: message ID history needs recovery.':!d.capable?'FTN is unavailable for this installation.':d.mode===2?'FTN is disabled. Settings and mail remain.':d.mode===1?'External BinkD owns connections and retries. SFX processes mail; Scan Outbound is available. Handed to BinkD does not confirm delivery.':d.paused?'FTN is paused for backup.':'Native BinkP status.'));
  if(d.serial_recovery_required){const recovery=element('button','Open FTN History Recovery');recovery.type='button';recovery.onclick=()=>navigate('config','tools.backup_restore');body.append(recovery);}
  body.append(cpObservationTable(['Setting','Current state'],[['Saved / active revision',d.saved_revision+' / '+(d.running?d.active_revision:'Not running')],['Classic listener',d.classic_listening?d.classic_port:'Off'],['TLS listener',d.tls_listening?d.tls_port:'Off'],['Mail processing',!d.running?'Not running':d.processing?'Active':'Idle'],['Runtime status',d.diagnostic==='ftn_external_processing_active'?'Ready for External BinkD':d.diagnostic||'No additional detail']]));
  const errors=[['Classic listener',d.classic_error],['TLS listener',d.tls_error],['Processing',d.processing_error]].filter(row=>row[1]);if(errors.length)body.append(element('pre',errors.map(row=>row.join(': ')).join('\n'),'cp-log-text'));
  let item=d.sessions.find(s=>s.serial===cpBinkpSelected);
  if(cpBinkpFollow){item=d.sessions.reduce((best,s)=>!best||(s.active&&!best.active)||(s.active===best.active&&BigInt(s.serial)>BigInt(best.serial))?s:best,null);cpBinkpSelected=item?.serial||'';}
  const select=$('cp-binkp-session');if(document.activeElement!==select){select.replaceChildren();const follow=element('option','Follow active session');follow.value='';select.append(follow);for(const s of d.sessions){const option=element('option','#'+s.serial+' '+s.network+' / '+s.link+' - '+(s.outbound?'Outbound':'Inbound')+' - '+(s.active?'Active':s.completed?'Completed':'Ended'));option.value=s.serial;select.append(option);}if(!cpBinkpFollow&&!item){const missing=element('option','Selected session is no longer retained');missing.value=cpBinkpSelected;select.append(missing);}select.value=cpBinkpFollow?'':cpBinkpSelected;}
  const transfers=element('div','','panel-grid');transfers.append(cpBinkpTransfer(item,true),cpBinkpTransfer(item,false));body.append(transfers);
  if(item)body.append(cpObservationTable(['Session','Observation'],[['Configured peer address',item.peer_address||'Unconfigured peer'],['Configured endpoint',item.endpoint||'Unavailable'],['Peer verification',item.peer_verified?'Verified':'Not verified'],['Files sent / received',item.sent_files+' / '+item.received_files],['Bytes sent / received',item.sent_bytes+' / '+item.received_bytes],['Result',item.reason||'No result recorded yet']]));
  body.append(cpObservationTable(['Processing','Count'],[['Processed receipts',d.processed_receipts],['Held receipts',d.held_receipts],['Imported messages',d.imported_messages],['Replayed messages',d.replayed_messages],['Scanned messages',d.scanned_messages]].map(([label,value])=>[label,d.running?value:'Not running'])));
  if(!cpBinkpActivityPaused)$('cp-binkp-activity').textContent=d.activity.length?d.activity.map(a=>ftnHeldTime(a.utc,d.zone)+'  #'+a.session+'  '+a.network+' / '+a.link+'\n  '+a.message).join('\n'):'No current BinkP activity to show.';
  $('cp-binkp-activity-pause').textContent=cpBinkpActivityPaused?'Resume Activity':'Pause Activity';
  const queues=$('cp-binkp-queue-body');queues.replaceChildren();$('cp-binkp-queue-controls').hidden=!cpBinkpQueue;
  if(cpBinkpQueue){const q=cpBinkpQueue;queues.append(element('h3','Queues / Held Mail'),element('p','Saved custody states. Page '+(Math.floor(cpBinkpOffset/64)+1)+'. Unscanned local messages: '+q.unscanned));
    const incoming=['Partial','Received','Processing','Held','Processed'],outgoing=['Queued','Claimed','Awaiting ACK','Delivered','Held','Handed to BinkD'];
    queues.append(cpObservationTable(['Incoming','Count'],incoming.map((name,i)=>[name,q.ingress[i]])),cpObservationTable(['Outgoing','Count'],outgoing.map((name,i)=>[name,q.outbound[i]])));
    queues.append(q.rows.length?cpObservationTable(['Direction','Network / Connection','File','State','Bytes','Reason'],q.rows.map(row=>[row.outbound?'Out':'In',row.network+' / '+row.link,row.file,(row.outbound?outgoing:incoming)[row.state]||'Unknown',row.bytes,row.reason])):element('p','No pending or held records on this page.'));
  }
}
async function cpRefreshBinkp(){
  if(!cpBinkpOpen||page!=='panel'||!authenticated||maintenanceActive||cpBinkpReading||cpBinkpBusy)return;cpBinkpReading=true;
  try{const data=await cpCallerApi('/api/control/binkp');if(cpBinkpData?.epoch!==data.epoch){cpBinkpQueue=null;cpBinkpSelected='';cpBinkpFollow=true;cpBinkpSample=cpBinkpRates=null;}
    cpBinkpData=data;const item=data.sessions.find(s=>s.serial===cpBinkpSelected),now=Date.now();cpBinkpRates=null;
    if(item){const sample=cpBinkpSample;if(sample?.serial===item.serial&&now>sample.time&&BigInt(item.sent_bytes)>=BigInt(sample.sent)&&BigInt(item.received_bytes)>=BigInt(sample.received)){const millis=BigInt(now-sample.time);cpBinkpRates={send:String((BigInt(item.sent_bytes)-BigInt(sample.sent))*1000n/millis),receive:String((BigInt(item.received_bytes)-BigInt(sample.received))*1000n/millis)};}cpBinkpSample={serial:item.serial,time:now,sent:item.sent_bytes,received:item.received_bytes};}
    if(!cpBinkpLoaded){cpBinkpLoaded=true;const receipt=await cpCallerApi('/api/control/binkp/action');if(receipt.found)cpBinkpApplyReceipt(receipt);}cpBinkpRender();
  }catch(error){cpBinkpData=null;$('cp-binkp-action-result').textContent=error.message;cpBinkpRender();}finally{cpBinkpReading=false;}
}
async function cpBinkpQueuePage(offset=0){if(cpBinkpBusy)return;cpBinkpBusy=true;cpBinkpButtons();try{const result=await cpCallerApi('/api/control/binkp?view=queues&offset='+offset);if(result.code!==0||!result.queues)throw new Error('Queue snapshot unavailable. No empty queue is implied.');cpBinkpOffset=offset;cpBinkpQueue=result.queues;cpBinkpRender();}catch(error){$('cp-binkp-action-result').textContent=error.message;}finally{cpBinkpBusy=false;cpBinkpButtons();}}
async function cpBinkpConnectionPage(offset=0){
  if(!cpBinkpReady())return;cpBinkpBusy=true;cpBinkpButtons();
  try{const result=await cpCallerApi('/api/control/binkp?view=connections&offset='+offset);if(result.code!==0)throw new Error('Connections could not be read.');cpBinkpConnections=result;cpBinkpConnectionOffset=offset;const select=$('cp-binkp-connection');select.replaceChildren();for(let i=0;i<result.connections.length;i++){const row=result.connections[i],option=element('option',row.name+(row.enabled&&row.outbound?'':' (not available for outgoing calls)'));option.value=String(i);option.disabled=!row.enabled||!row.outbound;select.append(option);}const first=result.connections.findIndex(row=>row.enabled&&row.outbound);select.value=first<0?'':String(first);$('cp-binkp-connection-panel').hidden=false;$('cp-binkp-connection-detail').textContent=result.connections.length?'Choose an enabled outgoing connection.':'No connections on this page. Add one in FTN Settings.';$('cp-binkp-connection-previous').disabled=offset===0;$('cp-binkp-connection-next').disabled=!result.more_connections;$('cp-binkp-connection-send').disabled=first<0;
  }catch(error){$('cp-binkp-action-result').textContent=error.message;}finally{cpBinkpBusy=false;cpBinkpButtons();}
}
async function cpBinkpRun(action){
  if(!cpBinkpReady(action))return;const selected=action==='poll'?cpBinkpConnections?.connections[Number($('cp-binkp-connection').value)]:null;
  if(action==='poll'&&(!selected?.enabled||!selected.outbound||$('cp-binkp-connection').value===''))return;
  cpBinkpBusy=true;cpBinkpButtons();
  try{const review=await cpCallerApi('/api/control/binkp/prepare',{action,network:selected?.network||'',link:selected?.link||'',epoch:cpBinkpData.epoch,revision:cpBinkpData.saved_revision});if(!review.found)throw new Error('A new request could not be prepared. Check the previous action result.');cpBinkpReceipt=review;
    if(!await offlineAsk(action==='poll'?'Poll this connection?':'Scan outbound mail?',selected?.name||'Use the current saved FTN settings.','This requests work. It does not confirm delivery. Held outgoing mail is not released.',action==='poll'?'Poll Now':'Scan Outbound')){const cancelled=await cpCallerApi('/api/control/binkp/cancel',{handle:review.handle});if(!cancelled.cancelled)throw new Error('Cancellation could not be confirmed. Check Action Result.');cpBinkpReceipt=null;$('cp-binkp-action-result').textContent='Cancelled. No work requested.';return;}
    cpBinkpUnknown=true;const result=await cpCallerApi('/api/control/binkp/submit',{handle:review.handle});if(!result.found)throw new Error('Receipt unavailable. Check activity; do not repeat an uncertain request.');cpBinkpApplyReceipt(result);$('cp-binkp-connection-panel').hidden=true;
  }catch(error){$('cp-binkp-action-result').textContent=error.message+(cpBinkpUnknown?' Use Check Action Result; no request is repeated.':'');}finally{cpBinkpBusy=false;cpBinkpButtons();await cpRefreshBinkp();}
}
async function cpBinkpCheck(){if(cpBinkpBusy)return;cpBinkpBusy=true;try{const value=await cpCallerApi('/api/control/binkp/action');if(value.found)cpBinkpApplyReceipt(value);else $('cp-binkp-action-result').textContent=cpBinkpReceiptText(value);}catch(error){$('cp-binkp-action-result').textContent=error.message;}finally{cpBinkpBusy=false;cpBinkpButtons();}}
$('cp-open-binkp').onclick=async()=>{cpBinkpOpen=true;$('cp-binkp').hidden=false;await cpRefreshBinkp();};$('cp-binkp-close').onclick=()=>{cpBinkpOpen=false;$('cp-binkp').hidden=true;};$('cp-binkp-refresh').onclick=cpRefreshBinkp;
$('cp-binkp-session').onchange=()=>{cpBinkpSelected=$('cp-binkp-session').value;cpBinkpFollow=!cpBinkpSelected;cpBinkpSample=cpBinkpRates=null;cpBinkpRender();};$('cp-binkp-activity-pause').onclick=()=>{cpBinkpActivityPaused=!cpBinkpActivityPaused;cpBinkpRender();};
$('cp-binkp-poll').onclick=()=>cpBinkpConnectionPage(0);$('cp-binkp-scan').onclick=()=>cpBinkpRun('scan');$('cp-binkp-check').onclick=cpBinkpCheck;
$('cp-binkp-queues').onclick=()=>cpBinkpQueuePage(0);$('cp-binkp-previous').onclick=()=>cpBinkpQueuePage(Math.max(0,cpBinkpOffset-64));$('cp-binkp-next').onclick=()=>cpBinkpQueuePage(cpBinkpOffset+64);
$('cp-binkp-connection-previous').onclick=()=>cpBinkpConnectionPage(Math.max(0,cpBinkpConnectionOffset-64));$('cp-binkp-connection-next').onclick=()=>cpBinkpConnectionPage(cpBinkpConnectionOffset+64);$('cp-binkp-connection-send').onclick=()=>cpBinkpRun('poll');$('cp-binkp-connection-cancel').onclick=()=>{$('cp-binkp-connection-panel').hidden=true;};
$('cp-binkp-settings').onclick=async()=>{await showPage('config','ftn.networks');if(page==='config'&&route==='ftn.networks'&&ftnState)await ftnWork(()=>ftnBrowse('networks'));};$('cp-binkp-held').onclick=async()=>{await showPage('config','ftn.networks');if(page==='config'&&route==='ftn.networks'&&ftnState)await ftnWork(()=>ftnBrowse('held'));};

// Help uses this installation's bundled documentation and compiled version.
function cpClearHelp(){
  if($('help-about-dialog').open)$('help-about-dialog').close();
  $('help-about-image').replaceChildren();$('help-about-version').textContent='';$('help-about-platform').textContent='';$('help-about-detail').textContent='';
}
async function cpOpenAbout(){
  if(!authenticated||maintenanceActive)return;
  cpClearHelp();$('help-about-detail').textContent='Reading installed version...';$('help-about-dialog').showModal();
  try{
    const value=await cpCallerApi('/api/control/about');if(!$('help-about-dialog').open)return;
    $('help-about-version').textContent=value.product;$('help-about-platform').textContent=value.platform+' | '+value.release_date;$('help-about-detail').textContent='';
    const logo=element('img');logo.alt='Spitfire-X BBS';logo.src='/help/sysop/images/sfx-about-approved.png';
    logo.onerror=()=>{logo.hidden=true;$('help-about-detail').textContent='About image unavailable. Restore Docs/Sysop from the matching SFX package.';};
    $('help-about-image').append(logo);
  }catch(error){if(authenticated&&$('help-about-dialog').open)$('help-about-detail').textContent='The installed version could not be read. Close this window and try again.';}
}
$('help-about').onclick=cpOpenAbout;$('help-about-close').onclick=cpClearHelp;

// System Monitor reads the Linux server, not this browser's workstation.
let cpMonitorCoresOpen=false,cpMonitorOpen=false,cpMonitorPaused=false,cpMonitorReading=false,cpMonitorGeneration=0,cpMonitorData=null,cpMonitorLast='',cpMonitorHistory={};
function cpClearMonitor(){++cpMonitorGeneration;cpMonitorCoresOpen=false;cpMonitorOpen=false;cpMonitorPaused=false;cpMonitorData=null;cpMonitorLast='';cpMonitorHistory={};$('cp-monitor').hidden=true;$('cp-monitor-body').replaceChildren();$('cp-monitor-detail').textContent='';}
function cpMonitorBytes(value){if(value===null||value===undefined)return 'Unavailable';let n=Number(value);if(!Number.isFinite(n)||n<0)return 'Unavailable';const units=['B','KiB','MiB','GiB','TiB'];let i=0;while(n>=1024&&i<units.length-1){n/=1024;++i;}return n.toFixed(i?1:0)+' '+units[i];}
function cpMonitorDuration(value){if(value===null||value===undefined)return 'Unavailable';let seconds=BigInt(value)/1000n;const days=seconds/86400n;seconds%=86400n;const hours=seconds/3600n;seconds%=3600n;return (days?days+'d ':'')+hours+'h '+seconds/60n+'m '+seconds%60n+'s';}
function cpMonitorRate(value){return value===null?'Sampling...':cpMonitorBytes(value)+'/s';}
function cpMonitorPercent(value){return value===null?'Sampling...':Number(value).toFixed(1)+'%';}
function cpMonitorGraph(values,maximum,label){
  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('viewBox','0 0 240 64');svg.setAttribute('role','img');svg.setAttribute('aria-label',label+' recent history');svg.classList.toggle('monitor-graph',true);
  let points=[];const flush=()=>{if(points.length){const line=document.createElementNS('http://www.w3.org/2000/svg','polyline');line.setAttribute('points',points.join(' '));line.setAttribute('fill','none');line.setAttribute('stroke','currentColor');line.setAttribute('stroke-width','2');svg.append(line);points=[];}};
  for(let i=0;i<values.length;++i){const value=values[i];if(value===null||!Number.isFinite(value)){flush();continue;}points.push((4+i*232/59).toFixed(2)+','+(60-Math.min(maximum,Math.max(0,value))*56/maximum).toFixed(2));}flush();return svg;
}
function cpMonitorRecord(d){
  if(d.sample_ms===cpMonitorLast)return;
  const gap=cpMonitorLast&&BigInt(d.sample_ms)-BigInt(cpMonitorLast)>5000n;cpMonitorLast=d.sample_ms;
  const values={cpu:d.cpu.percent,memory:d.memory.total===null?null:100*Number(d.memory.used)/Number(d.memory.total),disks:d.disks.read_rate===null?null:d.disks.read_rate+d.disks.write_rate,network:d.network.receive_rate===null?null:d.network.receive_rate+d.network.send_rate,sqlite:d.sqlite.interval_available?Number(d.sqlite.reads)+Number(d.sqlite.writes):null};
  for(const [key,value] of Object.entries(values)){const history=cpMonitorHistory[key]||(cpMonitorHistory[key]=[]);if(gap)history.push(null);history.push(value);if(history.length>60)history.splice(0,history.length-60);}
}
function cpRenderMonitor(){
  $('cp-monitor-pause').textContent=cpMonitorPaused?'Resume Monitor':'Pause Monitor';$('cp-monitor-refresh').disabled=cpMonitorPaused||cpMonitorReading;
  $('cp-monitor').classList.toggle('monitor-gray',$('cp-monitor-gray').checked);$('cp-monitor').classList.toggle('monitor-compact',$('cp-monitor-compact').checked);
  const body=$('cp-monitor-body');body.replaceChildren();const d=cpMonitorData;if(!d)return;
  const card=(key,title,primary,secondary,floor=100)=>{const box=element('section',null,'monitor-card monitor-'+key);box.append(element('h3',title),element('p',primary,'monitor-primary'),element('p',secondary,'field-help'));const values=cpMonitorHistory[key];if(values){const maximum=(key==='cpu'||key==='memory')?100:Math.max(floor,...values.filter(v=>v!==null));box.append(cpMonitorGraph(values,maximum,title));if(key!=='cpu'&&key!=='memory')box.append(element('p','Graph: 0 to '+(key==='sqlite'?Math.ceil(maximum)+' page operations/s':cpMonitorBytes(maximum)+'/s'),'field-help'));}body.append(box);return box;};
  const cpu=card('cpu','CPU (host)',d.cpu.present?cpMonitorPercent(d.cpu.percent):'Unavailable',d.cpu.cores.length+' logical CPUs');
  if(d.cpu.cores.length){const details=element('details');details.id='cp-monitor-cores';details.open=cpMonitorCoresOpen;details.ontoggle=()=>{if(details===document.getElementById('cp-monitor-cores'))cpMonitorCoresOpen=details.open;};details.append(element('summary','Per-core activity'));details.append(cpObservationTable(['CPU','Use'],d.cpu.cores.map(c=>[c.name,cpMonitorPercent(c.percent)])));cpu.append(details);}
  card('memory','Memory (host)',d.memory.total===null?'Unavailable':cpMonitorBytes(d.memory.used)+' / '+cpMonitorBytes(d.memory.total),d.memory.available===null?'Memory information unavailable.':cpMonitorBytes(d.memory.available)+' available');
  card('disks','Disks (host)',d.disks.present?'Read '+cpMonitorRate(d.disks.read_rate)+' | Write '+cpMonitorRate(d.disks.write_rate):'Unavailable','Whole physical devices: '+(d.disks.devices.join(', ')||'Unavailable'),65536);
  card('sqlite','SQLite',!d.sqlite.available?'Unavailable':!d.sqlite.interval_available?'Sampling...':'Read '+d.sqlite.reads+'/s | Write '+d.sqlite.writes+'/s',d.sqlite.cache_basis_points===null?'Shared Runtime connection. Cache rate unavailable.':'Shared Runtime connection. Cache hit '+(d.sqlite.cache_basis_points/100).toFixed(1)+'%.',10);
  card('network','Network (host)',d.network.present?'Receive '+cpMonitorRate(d.network.receive_rate)+' | Send '+cpMonitorRate(d.network.send_rate):'Unavailable','Non-loopback interfaces: '+(d.network.interfaces.join(', ')||'Unavailable')+'. Total received '+cpMonitorBytes(d.network.received)+'; sent '+cpMonitorBytes(d.network.sent)+'.',1024);
  card('uptime','Uptime','BBS '+(d.uptime.bbs_ms===null?(d.bbs_running?'Unavailable':'Stopped'):cpMonitorDuration(d.uptime.bbs_ms)),'OS '+cpMonitorDuration(d.uptime.os_ms));
}
async function cpRefreshMonitor(){
  if(!cpMonitorOpen||cpMonitorPaused||cpMonitorReading||!authenticated||maintenanceActive||page!=='panel')return;
  const generation=cpMonitorGeneration;cpMonitorReading=true;cpRenderMonitor();
  try{const data=await cpCallerApi('/api/control/monitor');if(generation!==cpMonitorGeneration||!cpMonitorOpen||cpMonitorPaused)return;cpMonitorData=data;cpMonitorRecord(data);$('cp-monitor-detail').textContent='Linux server readings. Up to 60 recent samples while this view is open.';cpRenderMonitor();}
  catch(error){if(generation===cpMonitorGeneration&&authenticated)$('cp-monitor-detail').textContent='System Monitor unavailable. '+error.message;}
  finally{cpMonitorReading=false;if(generation===cpMonitorGeneration)cpRenderMonitor();}
}
$('cp-open-monitor').onclick=async()=>{cpMonitorOpen=true;$('cp-monitor').hidden=false;await cpRefreshMonitor();};
$('cp-monitor-close').onclick=cpClearMonitor;$('cp-monitor-refresh').onclick=cpRefreshMonitor;
$('cp-monitor-pause').onclick=()=>{cpMonitorPaused=!cpMonitorPaused;++cpMonitorGeneration;cpRenderMonitor();if(!cpMonitorPaused)void cpRefreshMonitor();};
$('cp-monitor-gray').onchange=cpRenderMonitor;$('cp-monitor-compact').onchange=cpRenderMonitor;

let fmOpen=false,fmBusy=false,fmGeneration=0,fmReading=false,fmJob=null,fmData=null,fmLocation={kind:'groups'},fmSelected=new Map(),fmAreas=new Map(),fmHatch=null,fmView=null,fmBatch=null;
function fmClear(){++fmGeneration;fmOpen=false;fmBusy=false;fmJob=null;fmData=null;fmSelected.clear();fmAreas.clear();fmHatch=null;fmView=null;fmBatch=null;$('fm-panel').hidden=true;$('fm-body').replaceChildren();$('fm-editor').replaceChildren();$('fm-job').replaceChildren();$('fm-detail').textContent='';}
function fmError(error){if(error.status===401)lockWebAccess();else if(authenticated)$('fm-detail').textContent=error.message;}
function fmButton(parent,label,action,disabled=false){const b=element('button',label);b.type='button';b.disabled=disabled;b.onclick=()=>Promise.resolve(action()).catch(fmError);parent.append(b);return b;}
function fmBlocked(){return fmBusy||busy||maintenanceActive||!!(fmJob?.submitted&&(!fmJob.known||fmJob.status?.running));}
function fmControls(){const blocked=fmBlocked()||!!fmBatch;for(const id of ['fm-browse','fm-search','fm-pending','fm-offline','fm-unfinished','fm-next','fm-select-page','fm-select-all','fm-rename','fm-move','fm-remove','fm-details','fm-contents','fm-hatch'])$(id).disabled=blocked;
  $('fm-next').disabled=blocked||!fmData?.next;$('fm-select-page').disabled=$('fm-select-all').disabled=blocked||fmLocation.kind!=='records';$('fm-select-all').disabled=blocked||fmLocation.kind!=='records'||!fmLocation.area;
  for(const id of ['fm-rename','fm-move','fm-details','fm-contents','fm-hatch'])$(id).disabled=blocked||fmSelected.size!==1;$('fm-remove').disabled=blocked||!fmSelected.size;
  for(const box of $('fm-body').querySelectorAll('input'))if(box.dataset.fileId){box.checked=fmSelected.has(box.dataset.fileId);box.disabled=blocked;}
  $('fm-selection').textContent=fmSelected.size+' selected';
}
function fmRenderJob(){const body=$('fm-job');body.replaceChildren();const job=fmJob;if(!job?.found){fmControls();return;}
  body.append(element('h3',job.submitted?'Current File Job':'Reviewed File Request'));
  body.append(element('p',job.submitted?(job.known?job.status.state+': '+job.status.detail:'Outcome unknown. Check status or retry the same request. Do not start replacement work.'):'Ready for confirmation. No job has started.'));
  if(job.known){const s=job.status;body.append(element('p',s.bytes_done+' / '+s.bytes_total+' bytes in the current pass. '+(s.cancel_requested?'Cancellation requested; completed changes remain.':'')));
    if(s.report){body.append(element('pre',s.detail,'cp-log-text'));fmButton(body,'Previous Report Page',()=>fmRun({action:'report',target:job.choices.action==='report'?job.choices.target:s.job,page:String(s.report.page-1)},false),!s.report.page);fmButton(body,'Next Report Page',()=>fmRun({action:'report',target:job.choices.action==='report'?job.choices.target:s.job,page:String(s.report.page+1)},false),s.report.page+1>=s.report.pages);}
    if(s.view)fmRenderView(s.view,body);
  }
  const bar=element('div','','form-actions');body.append(bar);fmButton(bar,'Check Job Status',()=>fmCheck(true),fmBusy);fmButton(bar,'Retry Same Request',()=>fmAct('retry'),fmBusy||!job.submitted||(job.known&&!job.status.running));fmButton(bar,'Cancel Remaining Work',()=>fmAct('cancel'),fmBusy||!job.submitted||(job.known&&!job.status.running));fmControls();
}
async function fmCheck(force=false){if(!fmOpen||fmReading||fmBusy||!authenticated||maintenanceActive||page!=='panel'||(!force&&(!fmJob?.submitted||(fmJob.known&&!fmJob.status.running))))return;const generation=fmGeneration;fmReading=true;
  try{const result=await api('/api/files/job');if(generation!==fmGeneration||!authenticated)return;fmJob=result;fmRenderJob();if(fmBatch)await fmAdvanceBatch();}catch(e){fmError(e);}finally{fmReading=false;}}
async function fmAct(action){if(action==='cancel')fmBatch=null;if(!fmJob?.found||fmBusy||!authenticated)return;const generation=fmGeneration,handle=fmJob.handle;fmBusy=true;fmControls();
  try{if(action==='retry'&&!await offlineAsk('Retry the same file job?','The exact previously reviewed request will be sent again.','Its original job and operation identities are retained. This does not create a replacement operation.','Retry Same Request'))return;
    const result=await api('/api/files/job',{handle,action});if(generation!==fmGeneration||!authenticated)return;fmJob=result;fmRenderJob();
  }catch(e){if(generation===fmGeneration&&fmJob){fmJob.known=false;fmError(e);}}finally{fmBusy=false;if(generation===fmGeneration)fmRenderJob();}}
async function fmRun(choices,confirm=true,summary=''){
  if(fmBlocked()||!authenticated)return null;const generation=fmGeneration;fmBusy=true;fmControls();
  try{const review=await api('/api/files/prepare',choices);if(generation!==fmGeneration||!authenticated)return null;fmJob=review;
    if(confirm&&!await offlineAsk('Review '+choices.action.replaceAll('_',' '),summary||JSON.stringify(choices,null,2),choices.action==='remove'?'This permanently removes the selected managed file from disk.':choices.action==='hatch'?'Queue only these destinations. Queued does not mean delivered or accepted. The original file remains.':'Only this reviewed request will run. Completed work is not undone by Cancel.','Confirm '+choices.action.replaceAll('_',' '))){await api('/api/files/job',{handle:review.handle,action:'discard'});if(generation===fmGeneration){fmJob=null;if(choices.action==='hatch'){fmHatch=null;$('fm-editor').replaceChildren();$('fm-detail').textContent='Hatch cancelled. No file was queued. Choose Hatch File to verify and review a new request.';}}return null;}
    if(generation!==fmGeneration||!authenticated)return null;fmJob={...review,submitted:true,known:false};const result=await api('/api/files/job',{handle:review.handle,action:'submit'});if(generation!==fmGeneration||!authenticated)return null;fmJob=result;return result;
  }catch(e){if(generation===fmGeneration)fmError(e);return null;}finally{fmBusy=false;if(generation===fmGeneration)fmRenderJob();}
}
async function fmBrowse(location={kind:'groups'},continuing=false){if(fmBlocked()||!authenticated)return;const generation=++fmGeneration;fmBusy=true;fmControls();
  try{let path;if(location.kind==='groups'||location.kind==='areas'){const params=new URLSearchParams(location.query||{});if(location.kind==='areas')params.set('group',location.group);path='/api/config/files/catalog?'+params;}
    else{const params=new URLSearchParams({kind:location.kind==='records'?'records':location.kind,area:location.area||'0',search:location.search||'',after:location.after||'0',selection:location.selection||'0'});if(location.pending_after)params.set('pending_after',location.pending_after);path='/api/files/read?'+params;}
    const data=await api(path);if(generation!==fmGeneration||!authenticated)return;if(data.ok===false)throw new Error('The file list could not be read. Refresh before taking action.');
    if(!continuing)fmSelected.clear();fmLocation=location;fmData=data;fmHatch=null;$('fm-editor').replaceChildren();const body=$('fm-body');body.replaceChildren();
    if(location.kind==='groups'){body.append(element('h3','Choose a File Group'));for(const row of data.groups)fmButton(body,row.name+(row.enabled?'':' (disabled)'),()=>fmBrowse({kind:'areas',group:row.id,group_name:row.name}));fmData.next=data.next?{...location,query:data.next}:null;}
    else if(location.kind==='areas'){body.append(element('h3',location.group_name||'File Areas'));for(const row of data.areas){fmAreas.set(row.id,row);fmButton(body,row.number+' — '+row.name+(row.enabled?'':' (disabled)'),()=>fmBrowse({kind:'records',area:row.id,area_name:row.name}));}fmData.next=data.next?{...location,query:data.next}:null;}
    else if(location.kind==='records'){body.append(element('h3',location.area_name||'Filename search'));const table=element('table','','cp-data-table'),head=element('tr');for(const title of ['Select','Filename','Description','Bytes','Area / state'])head.append(element('th',title));table.append(head);
      for(const row of data.records){const tr=element('tr'),cell=element('td'),box=document.createElement('input');box.type='checkbox';box.dataset.fileId=row.id;box.setAttribute('aria-label','Select '+row.name);box.checked=fmSelected.has(row.id);box.disabled=row.purged;box.onchange=()=>{if(box.checked)fmSelected.set(row.id,row);else fmSelected.delete(row.id);fmControls();};cell.append(box);tr.append(cell,element('td',row.name),element('td',row.short),element('td',row.bytes),element('td',(fmAreas.get(row.area)?.name||row.area)+' / '+(['Available','Removed','Missing'][row.state]||'Unknown')+(row.pending?' / unfinished':'')));table.append(tr);}body.append(table);if(!data.records.length)body.append(element('p','No matching files.'));fmData.next=data.next!=='0'?{...location,after:data.next}:null;
    }else{body.append(element('h3','Unfinished '+location.kind));for(const row of data.pending){const item=element('section','','card');item.append(element('h4',row.name||row.operation),element('pre',row.detail+(row.hatch?'\n'+JSON.stringify(row.hatch,null,2):''),'cp-log-text'));
        const action=location.kind==='operations'?'retry_operation':location.kind==='imports'?'retry_import':location.kind==='intake'?'retry_intake':'retry_hatch';fmButton(item,'Review Original Retry...',()=>fmRun({action,target:row.operation},true,row.name+'\n'+row.detail+(row.hatch?'\n'+JSON.stringify(row.hatch,null,2):'')));
        if(location.kind==='operations'||location.kind==='imports')fmButton(item,'Review Cancel Intent...',()=>fmRun({action:location.kind==='operations'?'cancel_operation':'cancel_import',target:row.operation},true,row.name+'\n'+row.detail));body.append(item);}if(!data.pending.length)body.append(element('p','No unfinished work found.'));fmData.next=data.pending_next?{...location,pending_after:data.pending_next}:null;}
    $('fm-detail').textContent='Loaded current file records. Changes require a separate review.';
  }catch(e){if(generation===fmGeneration)fmError(e);}finally{fmBusy=false;if(generation===fmGeneration)fmControls();}
}
async function fmAllAreas(){const rows=[];let revision='0',after='0';for(;;){const data=await api('/api/files/areas?revision='+revision+'&after_id='+after);for(const a of data.areas){rows.push(a);fmAreas.set(a.id,a);}if(rows.length>9999)throw new Error('Too many file areas to display.');if(!data.next)break;revision=data.next.revision;after=data.next.after_id;}return rows;}
async function fmDetails(){const row=[...fmSelected.values()][0];if(!row||fmBlocked())return;const generation=fmGeneration;try{const data=await api('/api/files/read?kind=detail&selection='+row.id);if(generation!==fmGeneration||!authenticated)return;if(!data.ok||data.records.length!==1)throw new Error('The selected file is unavailable.');const f=data.records[0];$('fm-editor').replaceChildren(element('h3',f.name),element('pre','Record '+f.id+' · revision '+f.revision+'\n'+f.bytes+' bytes · '+f.downloads+' downloads\nUploader: '+f.uploader+'\n'+f.short+'\n'+f.long,'cp-log-text'));}catch(e){fmError(e);}}
function fmInput(parent,label,value,multiline=false){const id='fm-field-'+label.replaceAll(' ','-').toLowerCase(),lab=element('label',label);lab.htmlFor=id;const input=document.createElement(multiline?'textarea':'input');input.id=id;input.value=value;parent.append(lab,input);return input;}
async function fmRename(move){const row=[...fmSelected.values()][0];if(!row||fmBlocked())return;const generation=fmGeneration;fmBusy=true;fmControls();try{const areas=await fmAllAreas();if(generation!==fmGeneration||!authenticated)return;const source=areas.find(a=>a.id===row.area);if(!source)throw new Error('The source area changed. Refresh the file list.');const editor=$('fm-editor');editor.replaceChildren(element('h3',move?'Move '+row.name:'Rename '+row.name));const name=fmInput(editor,'Filename',row.name);name.maxLength=255;let destination=null;
    if(move){const label=element('label','Destination File Area');label.htmlFor='fm-destination';destination=document.createElement('select');destination.id='fm-destination';for(const area of areas.filter(a=>a.enabled)){const o=element('option',area.number+' — '+area.name);o.value=area.id;destination.append(o);}editor.append(label,destination);}
    fmButton(editor,'Review '+(move?'Move':'Rename')+'...',()=>{const target=move?areas.find(a=>a.id===destination.value):source;if(!target)throw new Error('Choose a destination.');return fmRun({action:move?'move':'rename',file:row.id,revision:row.revision,destination:target.id,destination_revision:target.revision,filename:name.value},true,row.name+' → '+name.value+'\n'+source.name+' → '+target.name);});fmButton(editor,'Cancel Edit',()=>editor.replaceChildren());
  }catch(e){fmError(e);}finally{fmBusy=false;fmControls();}}
async function fmSelectAll(){if(fmBlocked()||!fmLocation.area)return;const generation=fmGeneration,selected=new Map();fmBusy=true;fmControls();try{let after='0';do{const data=await api('/api/files/read?area='+fmLocation.area+'&after='+after);if(generation!==fmGeneration||!authenticated)return;if(!data.ok)throw new Error('Selection could not be completed. Nothing newly selected.');for(const row of data.records)if(!row.purged)selected.set(row.id,row);if(selected.size>100000)throw new Error('This area exceeds the 100,000-file display limit. Narrow the search.');after=data.next;}while(after!=='0');fmSelected=selected;$('fm-detail').textContent='Selected all '+selected.size+' current files in this File Area, including other pages.';}catch(e){fmError(e);}finally{fmBusy=false;fmControls();}}
async function fmRemove(){const rows=[...fmSelected.values()];if(!rows.length||fmBlocked()||fmBatch)return;const generation=fmGeneration;fmBusy=true;fmControls();try{if(!await offlineAsk('Remove selected files?',rows.map(r=>r.name+' [record '+r.id+']').join('\n'),'Permanently delete these '+rows.length+' managed files from disk. Stops on an uncertain result. Completed deletions remain.','Remove Files'))return;if(generation!==fmGeneration||!authenticated)return;fmBatch={rows,index:0,removed:0,handle:null};}finally{fmBusy=false;fmControls();}await fmAdvanceBatch();}
async function fmAdvanceBatch(){const batch=fmBatch;if(!batch||fmBusy)return;
  if(batch.handle){if(fmJob?.handle!==batch.handle||!fmJob.known){fmBatch=null;$('fm-detail').textContent='Batch stopped because the current outcome is uncertain. Check the same job; no remaining files were attempted.';fmControls();return;}if(fmJob.status.running)return;if(fmJob.status.state!=='Completed'||!fmJob.status.ok){fmBatch=null;$('fm-detail').textContent='Batch stopped: '+fmJob.status.detail;fmControls();return;}fmSelected.delete(batch.rows[batch.index].id);++batch.index;++batch.removed;batch.handle=null;}
  if(batch.index>=batch.rows.length){fmBatch=null;$('fm-detail').textContent='Confirmed removed: '+batch.removed+'. Refresh the file list.';fmControls();return;}
  const row=batch.rows[batch.index],result=await fmRun({action:'remove',file:row.id,revision:row.revision},false);if(fmBatch!==batch)return;
  if(!result?.found){fmBatch=null;$('fm-detail').textContent='Batch stopped. Confirmed removed: '+batch.removed+'. Check the current job before continuing.';fmControls();return;}batch.handle=result.handle;if(!result.known||!result.status.running)await fmAdvanceBatch();
}
function fmRenderView(v,parent){fmView=v;parent.append(element('h3',v.filename+(v.member?' — '+v.member:'')));if(v.archive){const table=element('table','','cp-data-table');for(const entry of v.entries){const row=element('tr'),name=element('td');fmButton(name,entry.name,()=>fmRun({action:'member',target:v.cursor,page:String(entry.index)},false),!entry.readable||entry.directory||entry.encrypted);row.append(name,element('td',entry.bytes+' bytes'+(entry.encrypted?' (encrypted)':'')));table.append(row);}parent.append(table);}else{const screen=element('div','','bbs-preview');const cells=v.cells.slice();while(cells.length<2000)cells.push([32,7]);screen.append(...previewCells(cells));parent.append(screen);fmButton(parent,'ZIP Directory',()=>fmRun({action:'index',target:v.cursor,page:'0'},false));}
  const count=v.archive?20:25;fmButton(parent,'Previous Contents Page',()=>fmRun({action:'page',target:v.cursor,page:String(v.page-1)},false),v.page===0);fmButton(parent,'Next Contents Page',()=>fmRun({action:'page',target:v.cursor,page:String(v.page+1)},false),(v.page+1)*count>=v.total);fmButton(parent,'Close Contents',()=>fmRun({action:'close_view',target:v.cursor,page:'0'},false));}
async function fmHatchBegin(){const row=[...fmSelected.values()][0];if(!row||fmBlocked())return;await fmBrowse({kind:'hatches',selection:row.id});fmSelected.set(row.id,row);fmControls();const editor=$('fm-editor');editor.replaceChildren(element('h3','Hatch '+row.name),element('p','Review any original hatch above before preparing a new one. A retry retains its original destinations and description.'));fmButton(editor,'Verify File for New Hatch',async()=>{const result=await fmRun({action:'inspect',area:row.area,filename:row.name,file:row.id},false);fmHatch={row};if(result?.known&&!result.status.running)await fmHatchForm();else $('fm-detail').textContent='Verification is running. Check Job Status, then Continue Hatch Review.';});fmButton(editor,'Continue Hatch Review',fmHatchForm);}
async function fmHatchForm(){if(!fmHatch||fmBlocked())return;const row=fmHatch.row,inspection=fmJob?.status?.inspection;const index=inspection?.records.findIndex(f=>f.id===row.id&&f.revision===row.revision);if(index===undefined||index<0||inspection.presence[index]!==0)throw new Error('The selected file identity/hash is not verified. Check the verification job.');const generation=fmGeneration;const data=await api('/api/files/hatch-catalog');if(generation!==fmGeneration||!authenticated)return;$('fm-detail').textContent='Selected file verified. Review the outgoing Hatch choices below.';const editor=$('fm-editor');editor.replaceChildren(element('h3','Hatch '+row.name),element('p','The original file and catalog record stay unchanged. Queued does not mean delivered.'));
  const select=(label,id)=>{const lab=element('label',label);lab.htmlFor=id;const s=document.createElement('select');s.id=id;editor.append(lab,s);return s;};const network=select('Network','fm-hatch-network'),echo=select('File echo','fm-hatch-echo'),origin=select('Your originating address','fm-hatch-origin');const description=fmInput(editor,'Hatch description',inspection.records[index].long||inspection.records[index].short,true);description.maxLength=4096;const choices=element('div');editor.append(element('h4','Permitted destinations'),choices);let destinationBoxes=[];
  const option=(s,value,label)=>{const o=element('option',label);o.value=value;s.append(o);};for(const n of data.networks.filter(n=>n.enabled&&n.addresses.length))option(network,n.code,n.name);
  const fillDestinations=()=>{choices.replaceChildren();destinationBoxes=[];for(const d of data.destinations.filter(d=>d.network===network.value&&d.tag===echo.value)){const label=element('label',d.name+' ('+d.link+')','checkbox-label'),box=document.createElement('input');box.type='checkbox';label.append(box);choices.append(label);destinationBoxes.push({box,link:d.link});}};
  const fillNetwork=()=>{echo.replaceChildren();origin.replaceChildren();for(const e of data.echoes.filter(e=>e.network===network.value))option(echo,e.tag,e.tag);for(const a of data.networks.find(n=>n.code===network.value)?.addresses||[])option(origin,a,a);fillDestinations();};network.onchange=fillNetwork;echo.onchange=fillDestinations;fillNetwork();
  fmButton(editor,'Select All Permitted',()=>destinationBoxes.forEach(d=>d.box.checked=true));fmButton(editor,'Clear Destinations',()=>destinationBoxes.forEach(d=>d.box.checked=false));fmButton(editor,'Review Hatch Queue...',()=>{const destinations=destinationBoxes.filter(d=>d.box.checked).map(d=>d.link);if(!destinations.length)throw new Error('Choose at least one permitted destination.');return fmRun({action:'hatch',file:row.id,revision:row.revision,catalog_revision:data.revision,network:network.value,tag:echo.value,origin:origin.value,description:description.value,destinations},true,row.name+'\nNetwork: '+network.value+'\nFile echo: '+echo.value+'\nOrigin: '+origin.value+'\nDestinations: '+destinations.join(', ')+'\n'+description.value);});
}
$('cp-open-files').onclick=async()=>{fmOpen=true;$('fm-panel').hidden=false;await fmCheck(true);if(!fmData)await fmBrowse();};
$('fm-close').onclick=()=>{fmOpen=false;$('fm-panel').hidden=true;};$('fm-browse').onclick=()=>fmBrowse();$('fm-next').onclick=()=>fmData?.next&&fmBrowse(fmData.next,true);
$('fm-search').onclick=()=>fmBrowse({kind:'records',search:$('fm-pattern').value});$('fm-pending').onclick=()=>fmRun({action:'pending'},true,'Register unlisted files in every configured File Area, using FILE_ID.DIZ or No Description.');$('fm-offline').onclick=()=>fmRun({action:'offline'},true,'Remove confirmed-missing listings in every File Area. Inaccessible locations are skipped.');
$('fm-unfinished').onclick=()=>fmBrowse({kind:$('fm-unfinished-kind').value});$('fm-select-page').onclick=()=>{for(const row of fmData?.records||[])if(!row.purged)fmSelected.set(row.id,row);fmControls();};$('fm-select-all').onclick=fmSelectAll;
$('fm-rename').onclick=()=>fmRename(false);$('fm-move').onclick=()=>fmRename(true);$('fm-remove').onclick=fmRemove;$('fm-details').onclick=fmDetails;$('fm-contents').onclick=()=>fmRun({action:'view',file:[...fmSelected.values()][0].id},false);$('fm-hatch').onclick=()=>fmHatchBegin().catch(fmError);


// Event audio is optional and belongs to this explicitly enabled Control Panel.
let soundContext=null,soundSource=null,soundEnabled=false,soundTab='',soundEpoch='0'.repeat(32),soundAfter='0',soundGeneration=0,soundPolling=false,soundQueue=[],soundPlaying=false,soundOpen=false,soundModel=null,soundDirty=false,soundSaving=false,soundUncertain=false;
function soundStop(){++soundGeneration;soundQueue=[];soundPlaying=false;if(soundSource){soundSource.onended=null;try{soundSource.stop();}catch{}soundSource=null;}}
function soundDisable(){soundEnabled=false;soundStop();soundEpoch='0'.repeat(32);soundAfter='0';$('sound-enable').textContent='Enable Sounds';$('sound-live').textContent='Sounds are muted.';}
function soundClear(){soundDisable();soundOpen=false;soundModel=null;soundDirty=false;soundSaving=false;soundUncertain=false;$('sound-editor').hidden=true;$('sound-rows').replaceChildren();$('sound-result').textContent='';if(soundContext){void soundContext.close().catch(()=>{});soundContext=null;}}
async function soundUnlock(){const Constructor=window.AudioContext||window.webkitAudioContext;if(!Constructor)throw Error('This browser does not offer audio playback.');if(!soundContext||soundContext.state==='closed')soundContext=new Constructor();await soundContext.resume();if(soundContext.state!=='running')throw Error('Browser playback is blocked. Check this site\'s sound permission and your audio device.');}
async function soundEnable(){if(soundEnabled){soundDisable();return;}const generation=soundGeneration;try{await soundUnlock();if(generation!==soundGeneration||!authenticated||page!=='panel')return;if(!soundTab){const bytes=new Uint8Array(16);window.crypto.getRandomValues(bytes);soundTab=Array.from(bytes,x=>x.toString(16).padStart(2,'0')).join('');}soundEnabled=true;soundEpoch='0'.repeat(32);soundAfter='0';$('sound-enable').textContent='Mute Sounds';$('sound-live').textContent='Sounds enabled. Waiting for new BBS events.';await soundPoll();}catch(error){soundDisable();$('sound-live').textContent=error.message;}}
async function soundPlay(path,preview=false){
  const generation=soundGeneration;if(!path)return;
  const allowed=()=>preview?page==='config'&&route==='system.event_sounds'&&soundOpen:page==='panel'&&soundEnabled;const file=await api('/api/control/sounds/preview',{path});if(generation!==soundGeneration||!authenticated||!allowed())return;
  const bytes=Uint8Array.from(atob(file.wave),c=>c.charCodeAt(0));const buffer=await soundContext.decodeAudioData(bytes.buffer);
  if(generation!==soundGeneration||!authenticated||!allowed())return;
  if(soundContext.state!=='running')throw Error('Browser audio is paused. Enable sounds again to resume playback.');
  const source=soundContext.createBufferSource();source.buffer=buffer;source.connect(soundContext.destination);soundSource=source;soundPlaying=true;
  source.onended=()=>{if(soundSource!==source)return;soundSource=null;soundPlaying=false;if(!preview)void soundNext();};source.start();
}
async function soundNext(){if(soundPlaying||!soundEnabled||soundOpen||!authenticated||page!=='panel')return;const item=soundQueue.shift();if(!item)return;if(Date.now()-item.time>3000){void soundNext();return;}soundPlaying=true;
  try{await soundPlay(item.path);}catch(error){soundDisable();$('sound-live').textContent='Playback stopped. '+error.message;}
}
async function soundPoll(){if(maintenanceActive){soundDisable();return;}if(!soundEnabled||soundPolling||!authenticated||page!=='panel'||maintenanceActive)return;soundPolling=true;const generation=soundGeneration;
  try{const result=await api('/api/control/sounds/poll',{tab:soundTab,epoch:soundEpoch,after:soundAfter});if(generation!==soundGeneration||!authenticated||!soundEnabled||page!=='panel')return;
    soundEpoch=result.epoch;soundAfter=result.through;$('sound-live').textContent=result.owner?(soundOpen?'Live sounds paused while editing or previewing.':'Sounds enabled for new BBS events.'):'Sounds are playing in another open Control Panel. This Control Panel is waiting.';
    if(result.owner&&!soundOpen){for(const event of result.events){const path=result.paths[event.event];if(path&&soundQueue.length<16)soundQueue.push({path,time:Date.now()});}void soundNext();}
  }catch(error){soundDisable();$('sound-live').textContent='Sound connection stopped. Enable again after checking the BBS. '+error.message;if(error.status===401)lockWebAccess();}finally{soundPolling=false;}
}
function soundControls(){if(!soundModel)return;const blocked=soundSaving||soundUncertain||busy||maintenanceActive;for(let i=0;i<10;++i){$('sound-path-'+i).disabled=blocked;$('sound-test-'+i).disabled=blocked;$('sound-clear-'+i).disabled=blocked;}$('sound-save').disabled=blocked||!soundDirty;$('sound-reload').disabled=soundSaving;$('sound-close').disabled=soundSaving;}
function soundChanged(){if(!soundModel)return;soundDirty=soundModel.paths.some((path,i)=>$('sound-path-'+i).value!==path);$('sound-result').textContent=soundDirty?'Unsaved sound assignments.':'';soundControls();}
async function soundPreview(index){soundStop();const generation=soundGeneration;try{const path=$('sound-path-'+index).value;if(!path){$('sound-result').textContent='This event is silent.';return;}await soundUnlock();if(generation!==soundGeneration||!authenticated||page!=='config'||route!=='system.event_sounds'||!soundOpen)return;await soundPlay(path,true);if(generation!==soundGeneration||!authenticated)return;$('sound-result').textContent='Preview started in this browser. Stop ends it. No BBS event was created.';}catch(error){$('sound-result').textContent='Preview unavailable. '+error.message;}}
function soundRender(){const body=$('sound-rows');body.replaceChildren();for(let i=0;i<soundModel.names.length;++i){const row=element('div','','sound-row'),label=element('label',soundModel.names[i]);label.setAttribute('for','sound-path-'+i);const input=element('input','');input.id='sound-path-'+i;input.value=soundModel.paths[i];input.maxLength=768;input.placeholder='/home/you/sounds/event.wav';input.oninput=soundChanged;const actions=element('div','','form-actions'),test=element('button','Test'),clear=element('button','Clear');test.id='sound-test-'+i;clear.id='sound-clear-'+i;test.type=clear.type='button';test.onclick=()=>soundPreview(i);clear.onclick=()=>{soundStop();input.value='';soundChanged();};actions.append(test,clear);row.append(label,input,actions);body.append(row);}soundControls();}
async function soundLoad(){if(soundSaving)return;if(soundDirty&&!await offlineAsk('Discard sound edits?','Reload the current saved assignments.','Your unsaved path edits will be discarded. WAV files stay unchanged.','Reload Assignments'))return;soundStop();const generation=soundGeneration;
  try{const result=await api('/api/control/sounds');if(generation!==soundGeneration||!authenticated)return;soundModel=result;soundDirty=false;soundUncertain=false;soundOpen=true;$('sound-editor').hidden=false;soundRender();$('sound-result').textContent='Saved assignments loaded. Blank means silent. '+result.activation;}
  catch(error){$('sound-result').textContent=error.message;if(error.status===401)lockWebAccess();}
}
async function soundSave(){if(!soundModel||!soundDirty||soundSaving||soundUncertain||busy||maintenanceActive)return;soundStop();const generation=soundGeneration,paths=soundModel.paths.map((_,i)=>$('sound-path-'+i).value),request={revision:soundModel.revision,paths};soundSaving=true;soundControls();
  try{await api('/api/control/sounds',request);const saved=await api('/api/control/sounds');if(generation!==soundGeneration||!authenticated)return;if(JSON.stringify(saved.paths)!==JSON.stringify(paths))throw Error('The saved assignments differ. Reload and review them.');soundModel=saved;soundDirty=false;soundUncertain=false;$('sound-result').textContent='Event sound assignments saved. '+saved.activation;}
  catch(error){if(generation!==soundGeneration)return;soundUncertain=true;$('sound-result').textContent='Save needs review. Reload Assignments reads the saved settings without repeating the save. '+error.message;if(error.status===401)lockWebAccess();}finally{soundSaving=false;soundControls();}
}
async function soundClose(){if(soundSaving)return false;if(soundDirty&&!await offlineAsk('Close Event Sounds?','Discard unsaved assignments.','WAV files stay unchanged.','Discard and Close'))return false;soundStop();soundOpen=false;soundModel=null;soundDirty=false;soundUncertain=false;$('sound-editor').hidden=true;$('sound-rows').replaceChildren();return true;}
$('sound-enable').onclick=soundEnable;$('sound-save').onclick=soundSave;$('sound-reload').onclick=soundLoad;$('sound-close').onclick=async()=>{if(await soundClose())await navigate('panel');};$('sound-stop').onclick=()=>{soundStop();$('sound-result').textContent='Preview stopped.';};
setInterval(()=>{void soundPoll();},1000);
window.addEventListener('pagehide',soundClear);


let setupLoaded=false,setupReading=false,setupSending=false,setupUncertain=false,setupAccess=0;
const setupFields=['bbs_name','time_zone','nodes','bind_address','telnet_port','ssh_enabled','ssh_port','strong_passwords','sysop_name','sysop_password','system_password'];
function setupClearSecrets(){++setupAccess;$('setup-sysop_password').value='';$('setup-system_password').value='';$('setup-sysop-confirm').value='';$('setup-system-confirm').value='';}
function setupControls(){for(const key of setupFields)$('setup-'+key).disabled=setupSending||setupUncertain;$('setup-sysop-confirm').disabled=$('setup-system-confirm').disabled=setupSending||setupUncertain;$('setup-submit').disabled=setupSending||setupUncertain;$('setup-check').disabled=setupSending;}
async function setupRefresh(state){
  const required=state.setup_required&&!state.recovery_required,wasOpen=!$('first-run').hidden;$('first-run').hidden=!required;
  if(!required){if(wasOpen){setupLoaded=false;$('control-tab').disabled=false;$('panel').hidden=page!=='panel';$('config').hidden=page!=='config';}return false;}$('panel').hidden=$('config').hidden=true;$('config-tab').disabled=$('control-tab').disabled=true;
  $('connection').textContent='Local controls';$('bbs-state').textContent='Setup required';$('start').disabled=true;if(setupLoaded||setupReading)return true;setupReading=true;const access=setupAccess;
  try{await loadTimeZones();const data=await api('/api/setup');if(access!==setupAccess||!authenticated)return true;if(!data.required){setupLoaded=false;await refresh();return true;}for(const [key,value] of Object.entries(data.defaults)){const input=$('setup-'+key);if(input.type==='checkbox')input.checked=value;else input.value=String(value);}setupLoaded=true;setupControls();}
  catch(error){$('setup-result').textContent=error.message;if(error.status===401)lockWebAccess();}finally{setupReading=false;}return true;
}
async function setupSubmit(event){event?.preventDefault();if(setupSending||setupUncertain||!authenticated||!$('setup-form').reportValidity())return;
  if($('setup-sysop_password').value!==$('setup-sysop-confirm').value||$('setup-system_password').value!==$('setup-system-confirm').value){$('setup-result').textContent='The repeated passwords must match. Nothing was sent.';return;}
  const request={};for(const key of setupFields){const input=$('setup-'+key);request[key]=input.type==='checkbox'?input.checked:input.type==='number'?Number(input.value):input.value;}
  setupClearSecrets();const access=setupAccess;setupSending=true;setupControls();$('setup-result').textContent='Completing first-run setup...';
  try{const result=await api('/api/setup',request);if(access!==setupAccess||!authenticated)return;$('setup-result').textContent=result.detail;await setupCheck();}
  catch(error){if(access!==setupAccess)return;setupUncertain=true;$('setup-result').textContent=error.message+' Check Setup Result before trying again. Password fields have been cleared.';if(error.status===401)lockWebAccess();}
  finally{request.sysop_password='';request.system_password='';setupSending=false;setupControls();}
}
async function setupCheck(){const access=setupAccess;try{const state=await api('/api/status');if(access!==setupAccess||!authenticated)return;
  if(state.setup_required&&!state.recovery_required){setupUncertain=false;setupControls();$('setup-result').textContent='Setup is still incomplete. Review the form and enter the same passwords to resume. No setup request was repeated.';return;}
  if(state.recovery_required){setupUncertain=true;$('setup-result').textContent='Setup needs recovery: '+state.diagnostic+'. Existing work is retained. Open BBS Health Check or Backup & Restore.';}
  else{$('setup-result').textContent='Setup complete. The BBS is stopped until you choose Start BBS.';setupUncertain=false;}
  setupLoaded=false;$('first-run').hidden=true;$('control-tab').disabled=$('config-tab').disabled=false;page='panel';$('panel').hidden=false;await refresh();
 }catch(error){setupUncertain=true;$('setup-result').textContent='Setup result is unavailable. Check again before trying to create the board. '+error.message;if(error.status===401)lockWebAccess();}finally{setupControls();}}
$('setup-form').onsubmit=setupSubmit;$('setup-check').onclick=setupCheck;

let contentsSelectionKey='',contentsSelected=new Set();
function contentsControls(){const selecting=offline?.kind==='file_contents'&&offline.contents?.awaiting_selection;$('contents-selection').hidden=!selecting;$('contents-select').disabled=!selecting||offlineBusy||!offlineReadOk||!!offlineUnknown||!contentsSelected.size;for(const input of $('contents-area-list').querySelectorAll('input'))input.disabled=offlineBusy||!!offlineUnknown;}
function renderContentsRestore(value){
  const contents=value.kind==='file_contents'?value.contents:null,key=contents?.awaiting_selection?value.operation+':'+value.confirmation:'';
  if(key!==contentsSelectionKey){contentsSelectionKey=key;contentsSelected=new Set();$('contents-area-list').replaceChildren();if(key)for(const area of contents.areas){const label=element('label','','checkbox-label'),input=element('input','');input.type='checkbox';input.onchange=()=>{if(input.checked)contentsSelected.add(area.id);else contentsSelected.delete(area.id);contentsControls();};label.append(input,element('span',area.name+' (#'+area.id+')'));$('contents-area-list').append(label);}}
  $('contents-destinations').replaceChildren();for(const dest of contents?.destinations||[]){const section=element('section','','health-finding');section.append(element('h3','Area '+dest.area),element('p','Current destination: '+dest.path),element('p','Missing files to restore: '+dest.missing+'; identical files left untouched: '+dest.identical+'; conflicting files skipped: '+dest.conflicts+'.'));$('contents-destinations').append(section);}
  if(contents&&value.completed)$('contents-destinations').append(element('p','Actual result: '+contents.restored+' restored; '+contents.identical+' identical; '+contents.conflicts+' conflicts skipped. Core settings and catalog identities are unchanged. Inspect restored files before serving them.'));
  contentsControls();
}
async function selectContentsRestore(){
  contentsControls();if($('contents-select').disabled)return;const previous=offline,areas=[...contentsSelected];offlineBusy=true;offlineControls();
  try{retainOffline({before:previous.operation,phase:'contents_select'});const result=await api('/api/maintenance/contents-select',{operation:previous.operation,confirmation:previous.confirmation,areas});$('offline-detail').textContent=result.detail;}
  catch(error){$('offline-detail').textContent=error.message+' Refresh the actual result. No selection was repeated.';}
  finally{offlineBusy=false;offlineControls();}await refreshOffline();
}
$('contents-select').onclick=selectContentsRestore;

let timeZonesLoaded=false;
async function loadTimeZones(){
  if(timeZonesLoaded)return;
  const zones=await api('/api/config/time-zones');
  const list=$('bbs-time-zones');list.replaceChildren();
  for(const zone of zones){const option=element('option',zone.replaceAll('_',' '));option.value=zone;list.append(option);}
  timeZonesLoaded=true;
}

// Door Bulletins owns a nonsecret, tab-local draft. Preview cells are never saved.
let dbul={data:null,selected:0,creating:true,dirty:false,uncertain:false,working:false,pending:null,confirm:null};
function dbulMessage(text){$('dbul-result').textContent=text;}
function dbulClearPreview(){$('dbul-preview-section').hidden=true;$('dbul-preview-screen').replaceChildren();}
function dbulEntry(){return dbul.data?.entries.find(e=>e.id===dbul.selected);}
function dbulFields(){const generated=$('dbul-source').value==='1';return {id:Number($('dbul-number').value),title:$('dbul-title').value,enabled:$('dbul-enabled').checked,source:generated?1:0,door:generated?Number($('dbul-door').value):0,path:generated?$('dbul-path').value:'',format:generated?Number($('dbul-format').value):0};}
function dbulRemember(){if(!draftKey)return;try{const key=draftKey+'-door-bulletins';if(!dbul.dirty&&!dbul.uncertain&&!dbul.pending){sessionStorage.removeItem(key);return;}sessionStorage.setItem(key,JSON.stringify({version:1,revision:dbul.data?.revision,definition:dbulFields(),creating:dbul.creating,pending:dbul.pending,uncertain:dbul.uncertain}));}catch{}}
function dbulRetained(){try{const raw=sessionStorage.getItem(draftKey+'-door-bulletins');if(!draftKey||!raw||raw.length>4096)return null;const v=JSON.parse(raw),d=v?.definition;if(v.version!==1||!d||!Number.isInteger(d.id)||d.id<1||d.id>999||typeof d.title!=='string'||d.title.length>64||typeof d.path!=='string'||d.path.length>240||typeof d.enabled!=='boolean'||![0,1].includes(d.source)||![0,1].includes(d.format)||!Number.isInteger(d.door)||d.door<0||d.door>999||!/^\d+$/.test(v.revision)||typeof v.creating!=='boolean')return null;return v;}catch{return null;}}
function dbulControls(){
  if(!$('door-bulletins-dialog').open)return;
  const blocked=dbul.working||!!doorCatalogLocked(),editable=!!dbul.data&&!dbul.uncertain&&!blocked,generated=$('dbul-source').value==='1',saved=!!dbulEntry()&&!dbul.creating;
  for(const id of ['dbul-title','dbul-enabled','dbul-source'])$(id).disabled=!editable;
  $('dbul-number').disabled=!editable||!dbul.creating;
  for(const id of ['dbul-door','dbul-path','dbul-format'])$(id).disabled=!editable||!generated;
  $('dbul-new').disabled=!editable;$('dbul-reload').disabled=blocked;$('dbul-save').disabled=!editable;
  $('dbul-remove').disabled=!editable||!saved;
  for(const id of ['dbul-validate','dbul-preview','dbul-plain'])$(id).disabled=!editable||!saved||dbul.dirty;
  for(const b of $('dbul-list').querySelectorAll('button'))b.disabled=!editable;
  $('dbul-close').disabled=dbul.working;
  $('dbul-source-help').textContent=generated?"Relative to the Door's working folder. ASCII path: 240 characters total, 128 per name. Source files stay in place.":'Display files: DB'+(String(Number($('dbul-number').value)||1).padStart(3,'0'))+'.ANS / .ASC, with exact security overrides. Enabled bulletins are visible to all callers.';
}
function dbulRows(){const list=$('dbul-list');list.replaceChildren();for(const item of dbul.data?.entries||[]){const row=element('tr');if(!dbul.creating&&item.id===dbul.selected)row.className='selected';const cell=element('td'),pick=element('button',String(item.id));pick.type='button';pick.onclick=()=>dbulDiscard(()=>dbulForm(item,false));cell.append(pick);row.append(cell);for(const value of [item.title,item.source===1?'Door file':'Display',item.enabled?'Yes':'No',['Not checked','Available','Unavailable'][item.health]||'Not checked'])row.append(element('td',value));list.append(row);}dbulControls();}
function dbulForm(d,creating){dbulClearPreview();dbul.selected=d.id;dbul.creating=creating;dbul.dirty=false;dbul.uncertain=false;dbul.pending=null;
  $('dbul-number').value=String(d.id);$('dbul-title').value=d.title;$('dbul-enabled').checked=d.enabled;$('dbul-source').value=String(d.source);$('dbul-path').value=d.path;$('dbul-format').value=String(d.format);
  const choices=$('dbul-door');choices.replaceChildren();const empty=element('option','Choose a local Door');empty.value='0';choices.append(empty);for(const door of dbul.data?.doors||[]){const o=element('option',door.id+' - '+door.name);o.value=String(door.id);choices.append(o);}if(d.door&&!(dbul.data?.doors||[]).some(x=>x.id===d.door)){const missing=element('option',d.door+' - Door unavailable');missing.value=String(d.door);choices.append(missing);}choices.value=String(d.door);dbulRows();dbulRemember();}
function dbulAsk(title,detail,label,action){dbul.confirm=action;$('dbul-confirm-title').textContent=title;$('dbul-confirm-detail').textContent=detail;$('dbul-confirm-yes').textContent=label;$('dbul-confirm').hidden=false;$('dbul-confirm-no').focus();}
function dbulDiscard(action){if(dbul.working)return;if(dbul.dirty||dbul.uncertain||dbul.pending){dbulAsk('Discard the retained draft?','Your unsaved edits will be replaced by saved settings. An uncertain save will not be repeated.','Discard and Continue',action);}else action();}
function dbulDefault(){let id=1;while(dbul.data.entries.some(e=>e.id===id))++id;if(id>999){dbulMessage('All 999 numbers are configured. Edit or remove a definition first.');return;}dbulForm({id,title:'',enabled:true,source:0,door:0,path:'',format:0},true);}
async function dbulLoad(restore=false){
  dbul.working=true;dbulClearPreview();dbulControls();dbulMessage('Reading Door Bulletins...');
  const retained=restore?dbulRetained():null;
  try{const data=await api('/api/config/doors/bulletins');if(!/^\d+$/.test(data.revision)||!Array.isArray(data.entries)||!Array.isArray(data.doors)||!/^[a-f0-9]{32}$/.test(data.operation))throw new Error('Invalid Door Bulletins response.');dbul.data=data;
    const current=data.entries.find(e=>e.id===dbul.selected)||data.entries[0];if(current)dbulForm(current,false);else dbulDefault();
    if(retained){dbulForm(retained.definition,retained.creating);dbul.dirty=true;dbul.pending=retained.pending||null;dbul.uncertain=!!retained.uncertain||!!retained.pending||retained.revision!==data.revision;if(dbul.uncertain)dbulMessage('Your draft is kept. Reload Saved Settings and review the actual result before another change. No save was repeated.');else dbulMessage('Your nonsecret Door Bulletin draft was restored in this tab.');}
    else dbulMessage('Choose a bulletin, or Add New. Numbers stay fixed; gaps are allowed.');dbulRemember();
  }catch(error){dbul.uncertain=true;dbulMessage(error.message+' Your edits are kept. Reload before making changes.');}
  finally{dbul.working=false;dbulControls();}
}
async function dbulOpen(){if(doorCatalogLocked())return;$('door-bulletins-dialog').showModal();await dbulLoad(true);}
function dbulClose(keep=false){
  if(dbul.working)return;
  if(!keep&&(dbul.dirty||dbul.uncertain||dbul.pending)){
    doorLeaveAsk({detail:dbul.uncertain||dbul.pending?'A Bulletin save needs checking. Close / Keep Draft preserves it; review the saved result before another change.':'Save or discard your Door Bulletin edits before closing.',
      save:!dbul.uncertain&&!dbul.pending?async()=>{await dbulSave();if(dbul.dirty||dbul.uncertain||dbul.pending)return false;dbulClose(true);}:null,
      discard:!dbul.uncertain&&!dbul.pending?()=>{dbul.dirty=false;dbulClose(true);}:null,keep:()=>dbulClose(true)});return;
  }
  dbulRemember();dbulClearPreview();$('dbul-confirm').hidden=true;dbul.confirm=null;$('door-bulletins-dialog').close();
}
function dbulLock(){dbulRemember();dbulClearPreview();if($('door-bulletins-dialog').open)$('door-bulletins-dialog').close();}
async function dbulSave(remove=false){
  if(dbul.working||dbul.uncertain||!dbul.data||doorCatalogLocked())return;
  if(!remove&&!$('dbul-form').reportValidity())return;
  const definition=remove?{id:dbul.selected}:dbulFields();if(!Number.isInteger(definition.id)||definition.id<1||definition.id>999){dbulMessage('Choose a permanent number from 1 to 999.');return;}
  if(!remove&&dbul.creating&&dbul.data.entries.some(e=>e.id===definition.id)){dbulMessage('That number is already used. Choose an unused number, or edit its existing definition.');return;}
  const request={revision:dbul.data.revision,operation:dbul.data.operation,action:remove?'remove':dbul.creating?'create':'save',definition};
  dbul.pending=request;dbulRemember();dbul.working=true;dbulClearPreview();dbulControls();dbulMessage(remove?'Removing the definition...':'Saving Door Bulletin settings...');
  try{const result=await api('/api/config/doors/bulletins/change',request);if(result.ok!==true)throw new Error(result.detail||'Save was not confirmed.');dbul.pending=null;dbul.dirty=false;dbul.uncertain=false;dbul.selected=remove?0:definition.id;dbulRemember();await dbulLoad();if(!dbul.uncertain)dbulMessage(result.detail);}
  catch(error){if(error.status===400&&error.response?.unresolved===false){dbul.pending=null;dbul.dirty=true;dbul.uncertain=false;dbulMessage((error.response.detail||error.message)+' Your edits are kept. Correct the fields, then Save again.');}else{dbul.uncertain=true;dbulMessage((error.response?.detail||error.message)+' Your draft is kept; reload and review saved settings before another change.');}dbulRemember();}
  finally{dbul.working=false;dbulControls();}
}
async function dbulCheck(show){
  if(dbul.working||dbul.dirty||dbul.uncertain||dbul.creating||!dbulEntry()||doorCatalogLocked())return;
  dbul.working=true;dbulClearPreview();dbulControls();dbulMessage('Checking the saved source...');
  try{const result=await api('/api/config/doors/bulletins/preview',{id:dbul.selected,ansi:!$('dbul-plain').checked,revision:dbul.data.revision});
    const refreshed=await api('/api/config/doors/bulletins');if(refreshed.revision!==dbul.data.revision){dbul.uncertain=true;throw new Error('Settings changed elsewhere. Reload before checking again.');}dbul.data=refreshed;dbulRows();dbulMessage(result.detail);
    if(result.available&&show){if(!authenticated)throw new Error('Sign in again.');if(result.id!==dbul.selected||result.columns!==80||result.rows!==25||!Array.isArray(result.cells)||result.cells.length!==2000)throw new Error('Invalid preview response.');$('dbul-preview-screen').replaceChildren(...previewCells(result.cells));$('dbul-preview-section').hidden=false;}
  }catch(error){dbulClearPreview();dbulMessage(error.message+' No old preview is shown.');}
  finally{dbul.working=false;dbulControls();}
}
$('door-bulletins-open').onclick=dbulOpen;
$('dbul-close').onclick=()=>dbulClose();
$('door-bulletins-dialog').addEventListener('cancel',event=>{event.preventDefault();if(!$('dbul-confirm').hidden){$('dbul-confirm').hidden=true;dbul.confirm=null;}else dbulClose();});
$('dbul-new').onclick=()=>dbulDiscard(dbulDefault);
$('dbul-reload').onclick=()=>dbulDiscard(()=>dbulLoad());
$('dbul-form').onsubmit=event=>{event.preventDefault();dbulSave();};
$('dbul-remove').onclick=()=>{const item=dbulEntry();if(item)dbulAsk('Remove definition '+item.id+' ('+item.title+')?','The source file will remain. This removes only this Door Bulletin definition.','Remove Definition',()=>dbulSave(true));};
$('dbul-confirm-no').onclick=()=>{$('dbul-confirm').hidden=true;dbul.confirm=null;};
$('dbul-confirm-yes').onclick=()=>{const action=dbul.confirm;dbul.confirm=null;$('dbul-confirm').hidden=true;action?.();};
$('dbul-validate').onclick=()=>dbulCheck(false);$('dbul-preview').onclick=()=>dbulCheck(true);
for(const id of ['dbul-number','dbul-title','dbul-enabled','dbul-source','dbul-door','dbul-path','dbul-format'])$(id).addEventListener('input',()=>{dbul.dirty=true;dbulClearPreview();dbulControls();dbulRemember();});
$('dbul-plain').onchange=dbulClearPreview;
window.addEventListener('pagehide',dbulRemember);

// DOS Door Setup owns its nonsecret draft and never repeats an uncertain write.
const dwFields=['binary','root','name','directory','command','io','input-interval','drop','drop-location','security','sysop','multiple','menu','number','menu-title','description'];
const dw={step:0,working:false,active:false,status:null,runtimeSaved:false,runtimeRevision:'0',game:null,catalog:null,savedDoor:0,uncertain:false,pending:null,poll:0,inputDraft:false,runtimeOnly:false,contextMenu:null};
function dwValues(){const v={};for(const key of dwFields){const e=$('dw-'+key);v[key]=e.type==='checkbox'?e.checked:e.value;}return v;}
function dwMessage(s){$('dw-result').textContent=s;}
function dwRemember(){if(!draftKey)return;try{if(dw.runtimeOnly){sessionStorage.setItem(draftKey+'-dos-runtime',JSON.stringify({binary:$('dw-binary').value,root:$('dw-root').value,pending:dw.pending,uncertain:dw.uncertain}));return;}sessionStorage.setItem(draftKey+'-dos-door',JSON.stringify({values:dwValues(),step:dw.step,savedDoor:dw.savedDoor,uncertain:dw.uncertain,pending:dw.pending}));}catch{}}
function dwControls(){
  const blocked=dw.working||dw.active;
  for(let n=0;n<6;++n)$('dw-page-'+n).hidden=n!==dw.step;
  $('dw-title').textContent=dw.runtimeOnly?'Doors > DOS Runtime Setup':'Doors > Add DOS Door';
  $('dw-step').textContent=dw.runtimeOnly?'Check and save DOS support — no Door will be created':'Step '+(dw.step+1)+' of 6 — '+['DOS support','Game','Caller connection','Door menu','Review','Caller test'][dw.step];
  for(const e of $('dos-wizard-dialog').querySelectorAll('input,select,button'))e.disabled=blocked||dw.uncertain;
  $('dw-close').disabled=dw.working;$('dw-reload').disabled=dw.working;$('dw-stop').hidden=!dw.active;$('dw-stop').disabled=dw.working;
  $('dw-working').hidden=!blocked;$('dw-back').disabled=blocked||dw.uncertain||dw.step===0||dw.step===5||!!dw.savedDoor;
  $('dw-prepare').disabled=blocked||dw.uncertain||!dw.status?.can_prepare;
  if(dw.status?.setup_available===false)$('dw-check').disabled=$('dw-prepare').disabled=true;
  $('dw-next').disabled=blocked||dw.uncertain||dw.step===5||(dw.step===0&&!dw.status?.checked);
  if(dw.step===0&&dw.status?.setup_available===false)$('dw-next').disabled=true;
  if(dw.runtimeOnly&&dw.runtimeSaved)$('dw-next').disabled=true;
  $('dw-next').textContent=dw.step===0?(dw.runtimeOnly?'Save Runtime':'Use This Runtime'):dw.step===4?(dw.savedDoor?'Place Saved Door':'Save and add to '+($('dw-menu').selectedOptions[0]?.textContent||'category')):'Next';
}
function dwShow(step){dw.step=step;if(step===2)dwMessage('Choose the I/O method and drop file required by this game.');if(step===3)dwMessage(dw.savedDoor?'The Door is saved. Choose its menu placement.':'Choose a Door menu and an unused number.');if(step===4){dwSummary();dwMessage(dw.savedDoor?'Review the placement for your saved Door, then choose Place Saved Door.':'Check the settings above, then choose Save and add to '+($('dw-menu').selectedOptions[0]?.textContent||'the category')+'.');}dwControls();dwRemember();}
function dwNeed(ok,message){if(!ok)throw new Error(message);}
async function dwAction(action){if(dw.working)return;dw.working=true;dwControls();try{await action();}catch(error){dwMessage((error.response?.detail||error.message)+(dw.savedDoor&&dw.step!==5?' Door #'+dw.savedDoor+' is already saved; its menu placement can be reviewed separately.':''));}finally{dw.working=false;dwControls();dwRemember();}}
function dwRuntimeFields(value){$('dw-binary').value=value?.binary||'';$('dw-root').value=value?.root||'';}
async function dwStatus(fill=false){
  const data=await api('/api/config/doors/dos/status');dw.status=data;dw.active=data.active;$('dw-version').textContent=data.version||'';
  $('dw-profile-help').textContent=data.can_prepare?'You can reuse installed DOSEMU2 or prepare the qualified separate SFX copy. The copy does not change system packages.':'For a new installation, install DOSEMU2 with your distribution’s package instructions, then check it here. Automatic SFX-copy preparation currently supports Pop!_OS 22.04 AMD64.';
  if(fill){const select=$('dw-candidate');select.replaceChildren();const blank=element('option','Choose a detected runtime, or enter its actual binary');blank.value='';select.append(blank);for(const [i,c] of data.candidates.entries()){const o=element('option',c.binary);o.value=String(i);select.append(o);}dwRuntimeFields(data.saved.binary?data.saved:data.candidates[0]);}
  dwMessage(data.detail);
  if(data.active&&$('dos-wizard-dialog').open){clearTimeout(dw.poll);dw.poll=setTimeout(()=>dwStatus().catch(e=>dwMessage(e.message)),500);}
  dwControls();return data;
}
async function dwOpen(options={}){
  if(!authenticated||doorCatalogLocked())return;$('dos-wizard-dialog').showModal();
  dw.runtimeOnly=!!options.runtimeOnly;dw.contextMenu=Number.isInteger(options.menu)?options.menu:null;
  await dwAction(async()=>{
    dw.game=null;dw.runtimeSaved=false;dw.savedDoor=0;dw.uncertain=false;dw.pending=null;dw.step=0;dw.inputDraft=false;
    await dwStatus(true);if(dw.runtimeOnly){dw.runtimeBaseline={binary:$('dw-binary').value,root:$('dw-root').value};let retained;try{retained=JSON.parse(sessionStorage.getItem(draftKey+'-dos-runtime')||'null');}catch{}if(retained&&typeof retained.binary==='string'&&retained.binary.length<=4096&&typeof retained.root==='string'&&retained.root.length<=4096){dwRuntimeFields(retained);if(retained.pending?.kind==='runtime'){dw.pending=retained.pending;dw.uncertain=true;}}if(dw.status?.runtime&&($('dw-binary').value!==dw.status.runtime.binary||$('dw-root').value!==dw.status.runtime.root))dw.status.checked=false;dwControls();return;}const data=await api('/api/config/doors/catalog?menu='+(dw.contextMenu??0));dw.catalog=data;
    const menus=$('dw-menu');menus.replaceChildren();for(const m of data.menus){const o=element('option',m.name);o.value=String(m.id);menus.append(o);}
    let saved;try{saved=JSON.parse(sessionStorage.getItem(draftKey+'-dos-door')||'null');}catch{}
    if(saved?.values&&Object.keys(saved.values).every(k=>dwFields.includes(k))){for(const key of dwFields)if(Object.hasOwn(saved.values,key)){const e=$('dw-'+key);if(e.type==='checkbox')e.checked=!!saved.values[key];else e.value=String(saved.values[key]);}dw.savedDoor=Number.isInteger(saved.savedDoor)&&saved.savedDoor>0&&saved.savedDoor<=999?saved.savedDoor:0;dw.pending=saved.pending||null;dw.uncertain=!!saved.uncertain||!!saved.pending;dwMessage(dw.uncertain?'Your unconfirmed request is kept. Use Refresh / Review Saved Result before another save. No request was repeated.':'Your draft is kept. Check DOS support and the game folder again before saving.');}
    if(dw.contextMenu!==null&&!saved?.pending&&!saved?.savedDoor){$('dw-menu').value=String(dw.contextMenu);$('dw-number').value='';}
    if(saved?.values&&Object.hasOwn(saved.values,'input-interval'))dw.inputDraft=true;
    if(dw.status?.runtime&&($('dw-binary').value!==dw.status.runtime.binary||$('dw-root').value!==dw.status.runtime.root))dw.status.checked=false;
    if(dw.savedDoor&&!dw.uncertain)dwShow(saved?.step===5?5:3);
  });
}
function dwClose(keep=false){
  if(dw.working)return;
  const runtimeEdited=dw.runtimeOnly&&JSON.stringify({binary:$('dw-binary').value,root:$('dw-root').value})!==JSON.stringify(dw.runtimeBaseline);
  const draft=dw.pending||dw.uncertain||runtimeEdited||(!dw.runtimeOnly&&dw.step!==5&&($('dw-name').value||$('dw-directory').value||dw.runtimeSaved));
  if(!keep&&draft){
    const unresolved=!!dw.pending||dw.uncertain;
    const checkedRuntime=dw.status?.checked&&dw.status.runtime?.binary===$('dw-binary').value.trim()&&dw.status.runtime?.root===$('dw-root').value.trim()&&dw.status.setup_available!==false;
    doorLeaveAsk({detail:unresolved?'A setup save needs checking. Keep the draft and review its result; no request will be repeated.':dw.runtimeOnly?'Save the checked runtime choice, discard these unsaved paths, or stay here. Check matching DOS support before saving. Already saved runtime settings and game drafts stay unchanged.':dw.savedDoor?'The Door is already saved. Discard leaves it unplaced in All Configured Doors; it does not undo saved runtime or game-driver settings.':dw.step===4?'Save the reviewed Door and placement, or discard this draft. Any runtime or driver settings already saved are kept.':'Finish the setup review before saving. Discard removes this draft; already saved runtime or driver settings stay. Background work is not cancelled by closing.',
      save:!unresolved&&!dw.active&&(dw.runtimeOnly?checkedRuntime:dw.step===4)?async()=>{await dwNext();if(dw.uncertain||dw.pending||(!dw.runtimeOnly&&dw.step!==5))return false;dwClose(true);}:null,
      discard:!unresolved?()=>{if(!dw.runtimeOnly){dw.savedDoor=0;dw.game=null;for(const key of ['name','directory','command','menu-title','description'])$('dw-'+key).value='';sessionStorage.removeItem(draftKey+'-dos-door');}else sessionStorage.removeItem(draftKey+'-dos-runtime');clearTimeout(dw.poll);$('dos-wizard-dialog').close();}:null,
      keep:()=>dwClose(true)});return;
  }
  dwRemember();clearTimeout(dw.poll);$('dos-wizard-dialog').close();
}
function dwLock(){dwRemember();clearTimeout(dw.poll);if($('dos-wizard-dialog').open)$('dos-wizard-dialog').close();dw.game=null;dw.status=null;dw.runtimeSaved=false;}
async function dwStart(action){
  await dwAction(async()=>{
    dw.runtimeSaved=false;dwNeed(!dw.savedDoor,'Finish placing the saved Door before changing the runtime.');
    await api('/api/config/doors/dos/start',{action,runtime:{binary:$('dw-binary').value.trim(),root:$('dw-root').value.trim()}});await dwStatus();
  });
}
async function dwScan(){
  await dwAction(async()=>{
    const directory=$('dw-directory').value.trim();dwNeed(directory,'Enter the game folder first.');dw.game=await api('/api/config/doors/dos/game',{directory});$('dw-directory').value=dw.game.directory;
    if(!dw.inputDraft)$('dw-input-interval').value=String(dw.game.input_interval_ms??0);
    const select=$('dw-program');select.replaceChildren();const placeholder=element('option','Choose a program, or enter a batch command');placeholder.value='';select.append(placeholder);
    for(const program of dw.game.programs){const o=element('option',program);o.value=program;select.append(o);}
    if(!$('dw-name').value.trim())$('dw-name').value=directory.split('/').filter(Boolean).at(-1)||'';
    $('dw-game-detail').textContent=dw.game.programs.length+' DOS program(s) found. '+dw.game.driver_preview;dwMessage('Folder checked. Enter the game command and its required arguments.');
  });
}
function dwGameValid(){
  const v=dwValues();dwNeed(v.name.trim()&&v.name.length<=64,'Enter a Door name.');dwNeed(dw.game&&dw.game.directory===v.directory.trim(),'Check the selected game folder first.');
  dwNeed(v.command.trim()&&v.command.length<=4096,'Enter the game command and the arguments its manual requires.');return v;
}
function dwCallerValid(){const v=dwGameValid();dwNeed(['0','1','2','3'].includes(v.io),'Choose the I/O method required by the game.');dwNeed(v['input-interval'].trim()!==''&&Number.isInteger(Number(v['input-interval']))&&Number(v['input-interval'])>=0&&Number(v['input-interval'])<=100,'Choose whole-number input pacing from 0 to 100 ms.');dwNeed(['0','1','2','3','4','7','8','9','10','13','14'].includes(v.drop),'Choose the drop file required by the game.');dwNeed(Number.isInteger(Number(v.security))&&Number(v.security)>=1&&Number(v.security)<=999,'Choose a security level from 1 to 999.');return v;}
async function dwMenu(refreshNumber=false){
  const menu=Number($('dw-menu').value||0);const data=await api('/api/config/doors/catalog?menu='+menu);dw.catalog=data;
  const used=new Set(data.entries.map(e=>e.number));let n=1;while(used.has(n)&&n<=99)++n;
  if(refreshNumber||!$('dw-number').value)$('dw-number').value=n<=99?String(n):'';
  if(!$('dw-menu-title').value.trim())$('dw-menu-title').value=$('dw-name').value.trim();
  $('dw-menu-detail').textContent='Used menu numbers: '+(data.entries.map(e=>e.number).join(', ')||'none')+'. Existing entries will be kept.';
}
function dwMenuValid(){const v=dwValues(),n=Number(v.number);dwNeed(Number.isInteger(n)&&n>=1&&n<=99,'Choose an unused menu number from 1 to 99.');dwNeed(v['menu-title'].trim(),'Enter the title to show on the menu.');dwNeed(dw.catalog?.menu===Number(v.menu),'Refresh the selected menu first.');dwNeed(!dw.catalog.entries.some(e=>e.number===n),'That menu number is already used. Choose another.');return v;}
function dwSummary(){
  const v=dwValues();$('dw-summary').textContent=[
    'Door: '+v.name,'Game folder: '+v.directory,'Command: '+v.command,
    'Runtime: '+(dw.status?.runtime?.binary||dw.status?.saved?.binary||'Saved DOS support'),
    'I/O: '+$('dw-io').selectedOptions[0]?.textContent,'Drop file: '+$('dw-drop').selectedOptions[0]?.textContent,
    'Input pacing: '+v['input-interval']+' ms'+(Number(v['input-interval'])===0?' (no added delay)':''),
    'Drop location: '+$('dw-drop-location').selectedOptions[0]?.textContent,
    'Minimum security: '+v.security+(v.sysop?' (Sysop-only during testing)':''),
    'Simultaneous callers: '+(v.multiple?'Allowed — game must support it':'One caller at a time'),
    'Menu: '+$('dw-menu').selectedOptions[0]?.textContent+' / '+v.number+' / '+v['menu-title'],
    dw.game&&Number(v['input-interval'])!==dw.game.input_interval_ms?'Save this game’s input pacing at '+v['input-interval']+' ms. Other driver settings are kept.':dw.game?.driver_preview||'Prepared game driver settings are kept.',
    dw.savedDoor?'Door #'+dw.savedDoor+' is already saved. This action only places it on the menu.':''
  ].filter(Boolean).join('\n');
}
async function dwNext(){
  await dwAction(async()=>{
    if(dw.step===0){
      dwNeed(dw.status?.checked&&!dw.active,'Check DOS support first.');
      const wanted=dw.status.runtime;dw.pending={kind:'runtime',runtime:wanted,identity:dw.status.checked_identity,revision:dw.status.check_revision||dw.status.revision};dwRemember();
      try{const result=await api('/api/config/doors/dos/save',{job:dw.status.job,revision:dw.status.check_revision||dw.status.revision});dw.status=result;dw.runtimeSaved=true;dw.runtimeRevision=result.revision;dw.pending=null;if(dw.runtimeOnly){dw.runtimeBaseline={binary:$('dw-binary').value,root:$('dw-root').value};dwMessage('DOS runtime saved. No Door or menu entry was created.');dwControls();}else{dwMessage('DOS support saved. Choose an already installed game folder.');dwShow(1);}}
      catch(error){dw.uncertain=true;throw error;}
    }else if(dw.step===1){dwGameValid();dwShow(2);}
    else if(dw.step===2){dwCallerValid();await dwMenu();dwShow(3);}
    else if(dw.step===3){await dwMenu();dwMenuValid();if(!dw.savedDoor)dwCallerValid();dwShow(4);}
    else if(dw.step===4)await dwSave();
  });
}
async function dwSave(){
  dwNeed(!dw.uncertain,'Review the unconfirmed request before saving again.');
  await dwMenu();const v=dwMenuValid();
  if(!dw.savedDoor){
    dwCallerValid();dwNeed(dw.runtimeSaved,'Check and save DOS support first.');
    dw.pending={kind:'driver',directory:dw.game.directory,input_interval_ms:Number(v['input-interval'])};dwRemember();
    try{const result=await api('/api/config/doors/dos/prepare-game',{directory:dw.game.directory,driver_identity:dw.game.driver_identity,runtime_revision:dw.runtimeRevision,input_interval_ms:Number(v['input-interval'])});dwNeed(result.ok===true&&result.input_interval_ms===Number(v['input-interval'])&&/^[a-f0-9]{64}$/.test(result.driver_identity||''),'Game driver settings were not confirmed. Review the saved result before continuing.');dw.pending=null;}
    catch(error){dw.uncertain=true;throw error;}
    const fresh=await api('/api/config/doors/door/0');const entry=fresh.entry;
    Object.assign(entry,{name:v.name.trim(),code:'DOS'+fresh.operation.slice(0,12).toUpperCase(),enabled:true,command:v.command.trim(),startup_directory:dw.game.directory,cleanup_command:'',program_type:0,io:Number(v.io),command_shell:false,drop_file:Number(v.drop),drop_location:Number(v['drop-location']),doordrop_utf8:false,allow_multiple:!!v.multiple,automatic_event:0,event_only:false,access:{mode:0,level:Number(v.security),sysop_only:!!v.sysop},execution:{mode:0,level:Number(v.security),sysop_only:!!v.sysop}});
    if(/^("[^"]+\.bat"|[^\s]+\.bat)(\s|$)/i.test(entry.command))entry.command='CALL '+entry.command;
    const request={revision:fresh.revision,operation:fresh.operation,action:'door',entry};dw.pending={kind:'door',request:structuredClone(request)};dwRemember();
    try{const result=await api('/api/config/doors/change',request);dwNeed(result.ok&&Number.isInteger(result.door_id)&&result.door_id>0,'The Door save was not confirmed.');dw.savedDoor=result.door_id;dw.pending=null;dwRemember();}
    catch(error){if(error.response?.unresolved===false){dw.pending=null;dw.game=null;dw.step=1;}else dw.uncertain=true;throw error;}
  }
  await dwMenu();dwMenuValid();
  const placement={menu:Number(v.menu),number:Number(v.number),kind:0,target:dw.savedDoor,title:v['menu-title'].trim(),description:v.description.trim()};
  const request={revision:dw.catalog.revision,operation:dw.catalog.operation,action:'entry',entry:{placement,previous_number:0,source_menu:0,create:true,move:false,order:[]}};
  dw.pending={kind:'entry',request:structuredClone(request)};dwRemember();
  try{const result=await api('/api/config/doors/change',request);dwNeed(result.ok,'Menu placement was not confirmed.');dw.pending=null;dw.uncertain=false;dwShow(5);dwMessage('Door #'+dw.savedDoor+' and its menu entry are saved. Start the BBS and try a real caller session.');}
  catch(error){if(error.response?.unresolved===false){dw.pending=null;dw.step=3;dwMessage('Door #'+dw.savedDoor+' is saved. Choose another menu number to finish placement.');}else dw.uncertain=true;throw error;}
}
async function dwReview(){
  await dwAction(async()=>{
    const pending=dw.pending;await dwStatus(!pending);
    if(!pending){dw.uncertain=false;dwMessage(dw.savedDoor?'Door #'+dw.savedDoor+' is saved. Choose its menu placement.':'Current DOS support status refreshed. No save was repeated.');return;}
    if(pending.kind==='runtime'){
      const s=dw.status;dwNeed(/^[a-f0-9]{64}$/.test(pending.identity||'')&&s.saved.binary===pending.runtime.binary&&s.saved.root===pending.runtime.root&&s.saved_identity===pending.identity&&BigInt(s.revision)>BigInt(pending.revision),'Runtime save is unconfirmed. Review the current runtime settings before choosing again.');
      dw.runtimeSaved=true;dw.runtimeRevision=s.revision;dw.step=dw.runtimeOnly?0:1;if(dw.runtimeOnly)dw.runtimeBaseline={binary:$('dw-binary').value,root:$('dw-root').value};
    }else if(pending.kind==='driver'){
      dw.game=await api('/api/config/doors/dos/game',{directory:pending.directory});dwNeed(!dw.game.driver_change,'The driver setting was not confirmed. Review the game folder and settings before continuing.');if(Object.hasOwn(pending,'input_interval_ms'))dwNeed(Number.isInteger(pending.input_interval_ms)&&pending.input_interval_ms>=0&&pending.input_interval_ms<=100&&dw.game.input_interval_ms===pending.input_interval_ms,'The saved input pacing differs from your request. Review it before continuing.');$('dw-input-interval').value=String(dw.game.input_interval_ms??0);dw.inputDraft=true;dw.step=1;
    }else if(pending.kind==='door'){
      const data=await api('/api/config/doors/catalog?menu=0');const wanted=pending.request.entry;const match=data.doors.find(d=>d.code===wanted.code&&d.name===wanted.name);dwNeed(match,'The Door save is unconfirmed. Review All Configured Doors before another save.');
      const saved=await api('/api/config/doors/door/'+match.id);
      for(const key of ['name','code','enabled','command','startup_directory','cleanup_command','program_type','io','command_shell','drop_file','drop_location','doordrop_utf8','allow_multiple','automatic_event','event_only'])dwNeed(saved.entry[key]===wanted[key],'The saved Door differs from your request. Review it in Edit Door.');
      for(const role of ['access','execution'])for(const key of ['mode','level','sysop_only'])dwNeed(saved.entry[role]?.[key]===wanted[role]?.[key],'The saved Door access differs from your request. Review it in Edit Door.');
      dw.savedDoor=match.id;dw.step=3;await dwMenu();
    }else if(pending.kind==='entry'){
      const wanted=pending.request.entry.placement;const data=await api('/api/config/doors/catalog?menu='+wanted.menu);dwNeed(data.entries.some(e=>['menu','number','kind','target','title','description'].every(k=>e[k]===wanted[k])),'Menu placement is unconfirmed. The saved Door is kept. Review its menu before another change.');dw.step=5;
    }else throw new Error('Review All Configured Doors before another change.');
    dw.pending=null;dw.uncertain=false;dwMessage('The current saved result has been reviewed. No request was repeated.');
  });
}
$('dw-check').onclick=()=>dwStart('check');$('dw-prepare').onclick=()=>dwStart('prepare');$('dw-scan').onclick=dwScan;
$('dw-candidate').onchange=()=>{const selected=$('dw-candidate').value;if(!selected)return;const value=dw.status?.candidates[Number(selected)];if(value)dwRuntimeFields(value);if(dw.status)dw.status.checked=false;dw.runtimeSaved=false;dwControls();};
for(const id of ['dw-binary','dw-root'])$(id).addEventListener('input',()=>{if(dw.status)dw.status.checked=false;dw.runtimeSaved=false;dwControls();});
$('dw-directory').addEventListener('input',()=>{dw.game=null;dw.inputDraft=false;});
$('dw-input-interval').addEventListener('input',()=>{dw.inputDraft=true;});
$('dw-program').onchange=()=>{const value=$('dw-program').value;if(value)$('dw-command').value=/\.bat$/i.test(value)?'CALL '+value:value;dwRemember();};
$('dw-menu').onchange=()=>dwAction(()=>dwMenu(true));$('dw-next').onclick=dwNext;$('dw-back').onclick=()=>dwShow(Math.max(0,dw.step-1));
$('dw-close').onclick=()=>dwClose();$('dos-wizard-dialog').addEventListener('cancel',event=>{event.preventDefault();dwClose();});
$('dw-reload').onclick=dwReview;$('dw-stop').onclick=()=>dwAction(async()=>{await api('/api/config/doors/dos/cancel',{});await dwStatus();});
$('dw-another').onclick=()=>{dw.savedDoor=0;dw.game=null;dw.uncertain=false;dw.pending=null;dw.inputDraft=false;for(const id of ['name','directory','command','menu-title','description'])$('dw-'+id).value='';$('dw-input-interval').value='0';$('dw-io').value='';$('dw-drop').value='';$('dw-multiple').checked=false;$('dw-sysop').checked=true;dwShow(dw.runtimeSaved?1:0);};
for(const key of dwFields)$('dw-'+key).addEventListener('input',dwRemember);
window.addEventListener('pagehide',dwRemember);

// BotGate rule files have a separate draft and save boundary from the four
// protection/challenge settings. A lost save reply is read back, never resent.
let botgateRule={data:null,text:'',dirty:false,pending:null,working:false,reading:false,generation:0,message:''};
let botgateFeed={data:null,reading:false,working:false,requested:null,uncertain:false,generation:0};
let botgateLeave=null;
const botgateRoute=()=>page==='config'&&route==='security.botgate';
const botgateFile=value=>Number.isInteger(value)&&value>=0&&value<6;
const botgateRevision=value=>typeof value==='string'&&/^[a-f0-9]{64}$/.test(value);
const botgateText=value=>typeof value==='string'&&new TextEncoder().encode(value).length<=65535;
function botgateHasDraft(){return botgateRoute()&&(botgateRule.dirty||!!botgateRule.pending);}
function botgateCanDiscard(){return !botgateRule.pending||!!botgateRule.pending.observed;}
function botgateDraft(){
  if(!botgateHasDraft()||!botgateRule.data)return null;
  return {file:botgateRule.data.file,revision:botgateRule.data.revision,base_text:botgateRule.data.text,text:botgateRule.text,
    pending:botgateRule.pending?{...botgateRule.pending,observed:undefined}:null};
}
function botgateRestoreDraft(value){
  if(!botgateRoute()||!botgateRule.data||!value||!botgateFile(value.file)||value.file===4||!botgateRevision(value.revision)||!botgateText(value.base_text)||!botgateText(value.text))return;
  const sameFile=botgateRule.data.file===value.file;
  botgateRule.data={...botgateRule.data,file:value.file,revision:value.revision,text:value.base_text,editable:sameFile&&botgateRule.data.editable};
  botgateRule.text=value.text;botgateRule.dirty=value.text!==value.base_text;botgateRule.pending=null;
  const pending=value.pending;
  if(pending&&pending.file===value.file&&pending.revision===value.revision&&pending.text===value.text&&typeof pending.known==='boolean')botgateRule.pending={file:pending.file,revision:pending.revision,text:pending.text,known:pending.known,observed:null};
  botgateRenderRule();botgateNotice(botgateRule.pending?'The previous rule-file save needs checking. Use Check Save Result; it will not be sent again.':'Your rule-file draft was restored. Review it before saving.');
  if(!sameFile)return botgateRestoreSelection(value.file);
}
async function botgateRestoreSelection(file){
  const generation=++botgateRule.generation;botgateRule.reading=true;botgateControls();
  try{const current=botgateRuleResponse(await api('/api/config/botgate/rule?file='+file),file);if(generation!==botgateRule.generation||!botgateRoute())return;
    botgateRule.data={...current,revision:botgateRule.data.revision,text:botgateRule.data.text};botgateRenderRule();
  }catch(error){if(generation===botgateRule.generation&&botgateRoute())botgateNotice('Your restored draft is kept, but this file is read-only until its current state can be checked. '+error.message);}
  finally{if(generation===botgateRule.generation){botgateRule.reading=false;botgateControls();}}
}
function botgateNotice(text){botgateRule.message=text;$('botgate-rule-result').textContent=text;if($('botgate-leave-dialog').open)$('botgate-leave-result').textContent=text;}
function botgateControls(){
  const active=botgateRoute();$('botgate-section').hidden=!active;if(!active)return;
  const locked=busy||saving||offlineBusy||!!offline?.active||maintenanceActive||!!mainPending||botgateRule.working||botgateRule.reading;
  const pending=!!botgateRule.pending,editable=botgateRule.data?.editable===true&&botgateRule.data.file!==4;
  $('botgate-file').disabled=locked||pending||!botgateRule.data;
  $('botgate-rule-text').disabled=locked||pending||!editable;
  $('botgate-rule-save').disabled=locked||(!pending&&(!editable||!botgateRule.dirty||!botgateText(botgateRule.text)));
  $('botgate-rule-save').textContent=pending?'Check Save Result':'Save Rule File';
  $('botgate-rule-reload').disabled=locked||!botgateRule.data||!botgateCanDiscard();
  $('botgate-feed-refresh').disabled=busy||saving||maintenanceActive||botgateFeed.reading||botgateFeed.working;
  $('botgate-feed-update').disabled=busy||saving||offlineBusy||maintenanceActive||botgateFeed.reading||botgateFeed.working||!botgateFeed.data?.running||botgateFeed.data?.updating||!!botgateFeed.requested||botgateFeed.uncertain;
  $('dirty-discard').disabled=busy||saving||!!qwkPending||!!mainPending||messageUnresolved()||!botgateCanDiscard();
  if($('botgate-leave-dialog').open){$('botgate-leave-save').disabled=locked||pending;$('botgate-leave-discard').disabled=locked||!botgateCanDiscard();$('botgate-leave-stay').disabled=locked;}
}
function botgateRenderRule(){
  const current=botgateRule.data,selector=$('botgate-file');selector.replaceChildren();
  for(const row of current?.files||[]){const option=element('option',row.label+' — '+row.name+(row.readonly?' (read-only)':''));option.value=String(row.file);selector.append(option);}
  if(current)selector.value=String(current.file);
  $('botgate-rule-text').value=botgateRule.text;$('botgate-rule-help').textContent=current?.detail||'Choose a rule file after its status is available.';
  botgateControls();
}
function botgateRuleResponse(value,file){
  if(!value||value.available!==true||value.file!==file||!botgateRevision(value.revision)||!botgateText(value.text)||typeof value.editable!=='boolean'||typeof value.detail!=='string'||!Array.isArray(value.files)||value.files.length!==6)throw new Error('Rule-file readback is unavailable. Your draft was kept.');
  const seen=new Set();for(const row of value.files){if(!row||!botgateFile(row.file)||seen.has(row.file)||typeof row.name!=='string'||typeof row.label!=='string'||typeof row.readonly!=='boolean'||(row.file===4&&!row.readonly))throw new Error('Rule-file selection is unavailable. Your draft was kept.');seen.add(row.file);}
  return {...value,editable:value.editable&&file!==4};
}
async function botgateReadRule(file=0){
  const generation=++botgateRule.generation;botgateRule.reading=true;botgateControls();
  try{const current=botgateRuleResponse(await api('/api/config/botgate/rule?file='+file),file);if(generation!==botgateRule.generation||!botgateRoute())return false;
    botgateRule.data=current;botgateRule.text=current.text;botgateRule.dirty=false;botgateRule.pending=null;botgateRenderRule();botgateNotice(current.detail);keepDraft();return true;
  }catch(error){if(generation===botgateRule.generation&&botgateRoute()){if(botgateRule.data)botgateRule.data={...botgateRule.data,editable:false,available:false};botgateRenderRule();botgateNotice(error.message);}return false;}
  finally{if(generation===botgateRule.generation){botgateRule.reading=false;botgateControls();}}
}
async function botgateLoad(){
  if(!botgateRoute())return;botgateControls();
  if(!botgateHasDraft())await botgateReadRule(botgateRule.data?.file??0);
  else botgateRenderRule();
  await botgateRefreshFeed();
}
function botgatePageChanged(){
  $('botgate-section').hidden=!botgateRoute();
  if(!botgateRoute()){
    botgateRule={data:null,text:'',dirty:false,pending:null,working:false,reading:false,generation:botgateRule.generation+1,message:''};
    botgateFeed={data:null,reading:false,working:false,requested:null,uncertain:false,generation:botgateFeed.generation+1};
    botgateLeave=null;if($('botgate-leave-dialog').open)$('botgate-leave-dialog').close();
  }
}
function botgateDiscard(){botgateRule.pending=null;botgateRule.dirty=false;botgateRule.text=botgateRule.data?.text||'';keepDraft();botgateControls();}
async function botgateTransition(action){
  if(busy||saving||botgateRule.reading||botgateRule.working||maintenanceActive)return;
  if(botgateHasDraft()){
    if(!botgateCanDiscard()){botgateNotice('Check Save Result before reloading or selecting another rule file. No save will be repeated.');return;}
    botgateLeave=action;$('botgate-leave-detail').textContent='Save this rule file, discard its edits, or stay here. Protection and challenge settings above are separate.';
    $('botgate-leave-result').textContent=botgateRule.message;$('botgate-leave-dialog').showModal();botgateControls();return;
  }
  await action();
}
async function botgateLeaveAnswer(answer){
  const action=botgateLeave;if(!action)return;
  if(answer==='stay'){botgateLeave=null;$('botgate-leave-dialog').close();return;}
  if(answer==='save'){if(botgateRule.pending||!await botgateSave())return;}
  else {if(!botgateCanDiscard())return;botgateDiscard();}
  botgateLeave=null;$('botgate-leave-dialog').close();await action();
}
async function botgateCheckSave(){
  const pending=botgateRule.pending;if(!pending||!botgateRoute()||botgateRule.working||botgateRule.reading)return false;
  botgateRule.working=true;const generation=botgateRule.generation;botgateControls();
  try{const current=botgateRuleResponse(await api('/api/config/botgate/rule?file='+pending.file),pending.file);if(!botgateRoute()||generation!==botgateRule.generation)return false;
    if(current.text===pending.text){botgateRule.data=current;botgateRule.text=current.text;botgateRule.dirty=false;botgateRule.pending=null;botgateRenderRule();botgateNotice('Saved rule-file contents confirmed by readback. No save was repeated.');keepDraft();return true;}
    pending.observed=current;keepDraft();botgateNotice('The current file differs from your requested save. Your draft is kept and no save was repeated. Check again, or use Reload Rules and review before explicitly discarding the draft.');return false;
  }catch(error){botgateNotice('Rule-file save is still unconfirmed. Your draft is kept. '+error.message);return false;}
  finally{botgateRule.working=false;botgateControls();}
}
async function botgateSave(){
  if(botgateRule.pending)return botgateCheckSave();
  if(!botgateRoute()||busy||saving||offlineBusy||maintenanceActive||botgateRule.reading||botgateRule.working||!botgateRule.dirty||!botgateRule.data?.editable||botgateRule.data.file===4||!botgateText(botgateRule.text))return false;
  const request={file:botgateRule.data.file,revision:botgateRule.data.revision,text:botgateRule.text};
  if(new TextEncoder().encode(JSON.stringify(request)).length>131072){botgateNotice('This rule file needs more than the browser request limit after encoding. Shorten it, or edit the file on Linux as the BBS owner. Your draft was kept; no save was sent.');return false;}
  botgateRule.pending={...request,known:false,observed:null};botgateRule.working=true;botgateControls();keepDraft();
  try{const result=await api('/api/config/botgate/rule/save',request);if(result.ok!==true||result.file!==request.file)throw new Error('Rule-file save reply could not be verified.');
    botgateRule.pending.known=true;keepDraft();botgateRule.working=false;const confirmed=await botgateCheckSave();if(confirmed)botgateNotice(result.detail||'Rule file saved. Valid rules are picked up within about two seconds.');return confirmed;
  }catch(error){
    if(error.response&&['botgate_rule_changed','botgate_rule_invalid'].includes(error.response.diagnostic)){botgateRule.pending=null;botgateNotice(error.message+' Your rule-file draft was kept.');}
    else botgateNotice('Rule-file save is unconfirmed. Use Check Save Result; no save will be repeated. '+error.message);
    keepDraft();return false;
  }finally{botgateRule.working=false;botgateControls();}
}
function botgateTime(seconds){return Number.isFinite(seconds)&&seconds>0?new Date(seconds*1000).toISOString().slice(0,19).replace('T',' ')+' UTC':'Unavailable';}
function botgateFeedLabel(current){
  if(!current.running)return 'Not running';
  if(current.updating)return current.result==='feed_validating'?'Checking downloaded feed':'Updating threat feed';
  const labels={feed_cache_unavailable:'No valid cached feed',feed_worker_unavailable:'Updater unavailable',feed_worker_failed:'Updater stopped unexpectedly',
    feed_cache_migration_or_security_failed:'Saved feed could not be verified',feed_candidate_invalid:'Downloaded feed was rejected',feed_cache_publication_failed:'Downloaded feed could not be activated',
    feed_update_exception:'Update failed; see update output',feed_cancelled_or_timed_out:'Update cancelled or timed out',feed_https_initialization_failed:'Secure download could not start',
    feed_https_signal_mask_failed:'Secure download unavailable',feed_download_too_large:'Downloaded feed exceeded the size limit',feed_download_callback_failed:'Download could not complete',
    feed_tls_certificate_date_invalid_check_clock:'Certificate date check failed; check the server clock',feed_tls_certificate_hostname_invalid:'Certificate name check failed'};
  if(current.result==='feed_update_success')return current.available?'Update completed and verified':'Feed unavailable; see update output';
  if(current.result==='feed_cache_loaded')return current.available?'Saved threat feed loaded':'Feed unavailable; see update output';
  if(Object.hasOwn(labels,current.result))return labels[current.result];
  if(/^feed_http_status_[0-9]+$/.test(current.result))return 'Download server returned an error; see update output';
  if(/^feed_https_curl_error_[0-9]+$/.test(current.result))return 'Secure download failed; see update output';
  if(/^feed_tls_certificate_validation_failed_[0-9]+$/.test(current.result))return 'Certificate validation failed; see update output';
  if(/^feed_cache_rename_error_[0-9]+$/.test(current.result))return 'Downloaded feed could not be saved; see update output';
  return 'Outcome unavailable; see update output';
}
function botgateRuleLabel(current){
  if(!current.running)return 'Not running';
  if(current.rules_ready&&current.rule_status==='rules_loaded')return 'Rules loaded and active';
  const failed=['rules_reload_failed','botgate_rule_parse_failed','botgate_geo_parse_failed','botgate_geo_scan_failed','botgate_migration_marker_invalid','botgate_migration_invalid','botgate_migration_conflict_preserved','botgate_owner_mismatch','botgate_file_inspection_failed','botgate_directory_create_failed','botgate_directory_sync_failed','botgate_geo_file_limit','botgate_geo_name_invalid','botgate_geo_total_limit','botgate_rule_index_invalid','botgate_temporary_parse_failed','botgate_temporary_time_failed'].includes(current.rule_status);
  if(current.rules_ready)return failed?'Last good rules active; latest reload failed':'Last good rules active; latest reload outcome unavailable';
  if(current.rule_status==='rules_not_loaded')return 'Rules have not been loaded';
  return failed?'Rules unavailable; reload failed':'Rules unavailable; latest reload outcome unavailable';
}
function botgateRenderFeed(){
  const current=botgateFeed.data,host=$('botgate-feed-status');host.replaceChildren();
  if(!current){host.append(element('p','Threat-feed status unavailable. Use Refresh Feed Status.'));botgateControls();return;}
  const rows=[['BBS',current.running?'Running':'Stopped'],['Active feed',!current.running?'Not running':current.available?current.entries+' addresses':'Unavailable; no valid cached feed'],['Update state',botgateFeedLabel(current)],['Last attempt',botgateTime(current.last_attempt)],['Last successful update',botgateTime(current.last_success)],['Rules',botgateRuleLabel(current)]];
  const list=element('dl','','botgate-status-grid');for(const [label,value] of rows)list.append(element('dt',label),element('dd',value));host.append(list);
  if(current.running&&current.progress)host.append(element('pre',current.progress,'cp-log-text'));
  const diagnostics=[];
  if(current.running&&!current.updating&&!['feed_update_success','feed_cache_loaded','feed_cache_unavailable'].includes(current.result)&&current.result)diagnostics.push('Feed result: '+current.result);
  if(current.running&&!['rules_loaded','rules_not_loaded'].includes(current.rule_status)&&current.rule_status)diagnostics.push('Rule reload: '+current.rule_status);
  if(diagnostics.length){const details=element('details');details.append(element('summary','Diagnostic details'),element('pre',diagnostics.join('\n'),'cp-log-text'));host.append(details);}
  if(!current.running)host.append(element('p','Start the BBS to update its threat feed. Rule files can be saved separately while it is stopped.','field-help'));
  botgateControls();
}
async function botgateRefreshFeed(review=false){
  if(!botgateRoute()||botgateFeed.reading||maintenanceActive)return false;const generation=botgateFeed.generation;botgateFeed.reading=true;botgateControls();
  try{const current=await api('/api/config/botgate/status');
    if(typeof current.running!=='boolean'||typeof current.available!=='boolean'||typeof current.updating!=='boolean'||typeof current.rules_ready!=='boolean'||!['entries','bytes_received','last_attempt','last_success'].every(key=>Number.isFinite(current[key])&&current[key]>=0)||!['result','progress','rule_status'].every(key=>typeof current[key]==='string'))throw new Error('Threat-feed status could not be verified.');
    if(generation!==botgateFeed.generation||!botgateRoute())return false;botgateFeed.data=current;
    if(botgateFeed.requested&&(review||current.updating||current.last_attempt!==botgateFeed.requested.last_attempt||!current.running)){botgateFeed.requested=null;botgateFeed.uncertain=false;}
    if(review){botgateFeed.uncertain=false;$('botgate-feed-result').textContent='Current feed status read. No update was repeated.';}
    botgateRenderFeed();return true;
  }catch(error){if(generation===botgateFeed.generation&&botgateRoute()){$('botgate-feed-result').textContent=error.message;botgateFeed.data=null;botgateRenderFeed();}return false;}
  finally{if(generation===botgateFeed.generation){botgateFeed.reading=false;botgateControls();}}
}
async function botgateUpdateFeed(){
  if(!botgateRoute()||busy||saving||maintenanceActive||botgateFeed.working||botgateFeed.reading||!botgateFeed.data?.running||botgateFeed.data.updating||botgateFeed.requested||botgateFeed.uncertain)return;
  botgateFeed.working=true;botgateFeed.requested={last_attempt:botgateFeed.data.last_attempt};botgateControls();
  try{const result=await api('/api/config/botgate/update',{});if(result.ok!==true)throw new Error('Update request was not confirmed.');$('botgate-feed-result').textContent=result.detail;}
  catch(error){if(error.response&&error.status===409)botgateFeed.requested=null;else botgateFeed.uncertain=true;$('botgate-feed-result').textContent=error.message+' Refresh Feed Status before requesting another update. No request was repeated.';}
  finally{botgateFeed.working=false;await botgateRefreshFeed();botgateControls();}
}
$('botgate-file').onchange=()=>{const file=Number($('botgate-file').value);$('botgate-file').value=String(botgateRule.data?.file??0);if(botgateFile(file))botgateTransition(()=>botgateReadRule(file)).catch(error=>botgateNotice(error.message));};
$('botgate-rule-text').oninput=()=>{if(!botgateRule.data?.editable||botgateRule.pending)return;botgateRule.text=$('botgate-rule-text').value;botgateRule.dirty=botgateRule.text!==botgateRule.data.text;setDirty(dirty);};
$('botgate-rule-save').onclick=()=>botgateSave();$('botgate-rule-reload').onclick=()=>botgateTransition(()=>botgateReadRule(botgateRule.data?.file??0));
$('botgate-feed-update').onclick=botgateUpdateFeed;$('botgate-feed-refresh').onclick=()=>botgateRefreshFeed(true);
for(const answer of ['save','discard','stay'])$('botgate-leave-'+answer).onclick=()=>botgateLeaveAnswer(answer).catch(error=>botgateNotice(error.message));
$('botgate-leave-dialog').addEventListener('cancel',event=>{event.preventDefault();botgateLeaveAnswer('stay');});
