module.exports=async function({assert,sandbox,evaluate,elements,drafts,response}){
  const entry={id:2,name:'Gateway',code:'GATEWAY',enabled:true,access:{mode:0,level:10,sysop_only:false},execution:{mode:0,level:10,sysop_only:false},command:'rlogin example.invalid:513 -q -P',startup_directory:'',cleanup_command:'',program_type:0,io:1,command_shell:false,drop_file:0,drop_location:0,doordrop_utf8:false,allow_multiple:true,automatic_event:0,event_only:false,authentication:0,caller_password:false,salt:'',banner:false,password:'',pepper:'',clear_password:false,clear_pepper:false};
  const catalog={revision:'5',operation:'abcdef0123456789abcdef0123456789',menu:0,menus:[{id:0,name:'Door Menu',access:entry.access,execution:entry.execution},{id:1,name:'Games',access:entry.access,execution:entry.execution}],doors:[{id:2,name:'Gateway',enabled:true,code:'GATEWAY'}],entries:[{menu:0,number:4,kind:0,target:2,title:'Gateway',description:''}]};
  sandbox.doorFixture=entry;sandbox.doorCatalogFixture=catalog;
  evaluate("authenticated=false;page='config';route='doors.manager';busy=false;saving=false;offline=null;offlineUnknown=null;offlineBusy=false;maintenanceActive=false;mainPending=null;draftKey='door-draft-check';doorCatalogState={data:doorCatalogFixture,menu:0,action:null,entry:null,unconfirmed:null};renderDoorCatalogEditor(doorFixture,'door');$('field-name').value='Changed Gateway';$('field-password').value='never-save-this-secret';formChanged()");
  assert.equal(elements.get('field-password').type,'password','new remote password is masked');
  assert.doesNotMatch(drafts.get('door-draft-check'),/never-save-this-secret/,'password never enters sessionStorage');
  assert.equal(JSON.parse(drafts.get('door-draft-check')).main.values.name,'Changed Gateway','nonsecret Door edits retained');
  sandbox.unsentDoorDraft=JSON.parse(drafts.get('door-draft-check'));evaluate("renderDoorCatalogEditor(null,null);restoreDraft(unsentDoorDraft)");assert.equal(evaluate('dirty'),true,'restored unsent Door draft remains unsaved');assert.equal(elements.get('save-board').disabled,false);assert.equal(elements.get('discard-board').disabled,false);evaluate("$('field-password').value='never-save-this-secret';formChanged()");
  let writes=0,reads=0,reject;
  const pending=new Promise((_resolve,fail)=>reject=fail);
  sandbox.fetch=async(path,options)=>{
    if(options.method==='POST'){++writes;assert.equal(path,'/api/config/doors/change');return pending;}
    ++reads;return response(catalog);
  };
  let action=evaluate('saveBoard()');elements.get('offline-decline').onclick();assert.equal(await action,false);assert.equal(writes,0,'Door Save Cancel makes no request');
  action=evaluate('saveBoard()');elements.get('offline-accept').onclick();await new Promise(resolve=>setImmediate(resolve));
  assert.equal(writes,1);let retained=JSON.parse(drafts.get('door-draft-check'));assert.equal(retained.main.door_catalog.unconfirmed,true,'review lock retained before the write resolves');assert.doesNotMatch(drafts.get('door-draft-check'),/never-save-this-secret/);
  reject(new Error('response lost after send'));assert.equal(await action,false);assert.equal(evaluate('board.can_save'),false);assert.equal(elements.get('field-password').value,'','password cleared after dispatch');
  assert.equal(await evaluate('saveBoard()'),false);assert.equal(writes,1,'unknown save never automatically repeated');
  retained=JSON.parse(drafts.get('door-draft-check'));sandbox.retainedDoor=retained;
  evaluate("renderDoorCatalogEditor(null,null);restoreDraft(retainedDoor)");assert.equal(evaluate('board.readback_failed'),true,'unknown save lock restored across reload');assert.equal(elements.get('save-board').disabled,true);
  evaluate('setDirty(false)');await evaluate('loadDoorCatalog()');assert.equal(reads,1);assert.equal(writes,1,'discard reads saved catalog without replay');
  evaluate("renderDoorCatalogEditor({placement:doorCatalogFixture.entries[0],previous_number:4,source_menu:0,create:false,move:false,order:[]},'entry');$('field-destination').value='1';$('field-number').value='9';formChanged()");
  const move=evaluate('doorCandidate()');assert.equal(move.entry.move,true);assert.equal(move.entry.create,true);assert.equal(move.entry.source_menu,0);assert.equal(move.entry.previous_number,4);assert.equal(move.entry.placement.menu,1);assert.equal(move.entry.placement.number,9);
  evaluate("setDirty(false);renderDoorCatalogEditor(doorFixture,'door');$('field-password').value='clear-on-lock';lockWebAccess()");assert.equal(elements.get('field-password').value,'','authentication lock clears Door password');
  evaluate("doorCatalogState.unconfirmed=null;setDirty(false);route='general.board_identity'");
};
