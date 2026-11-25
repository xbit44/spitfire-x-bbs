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
function dbulClose(){if(dbul.working)return;dbulRemember();dbulClearPreview();$('dbul-confirm').hidden=true;dbul.confirm=null;$('door-bulletins-dialog').close();}
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
$('dbul-close').onclick=dbulClose;
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
