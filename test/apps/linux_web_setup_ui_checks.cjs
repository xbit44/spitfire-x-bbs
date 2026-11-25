'use strict';
module.exports=async({assert,sandbox,evaluate,elements,drafts,response})=>{
 const defaults={bbs_name:'Fresh board',time_zone:'Etc/UTC',nodes:4,bind_address:'0.0.0.0',telnet_port:2323,ssh_enabled:true,ssh_port:2222,strong_passwords:true};
 for(const key of Object.keys(defaults)){evaluate("$('setup-"+key+"')");elements.get('setup-'+key).type=typeof defaults[key]==='boolean'?'checkbox':typeof defaults[key]==='number'?'number':'text';}
 let reads=0,posts=0;
 sandbox.fetch=async path=>{assert.equal(path,'/api/setup');++reads;return response({required:true,defaults});};
 evaluate("authenticated=true;setupLoaded=false;setupUncertain=false;$('first-run').hidden=true;page='panel'");
 await evaluate('setupRefresh({setup_required:true,recovery_required:false})');
 assert.equal(elements.get('first-run').hidden,false);assert.equal(elements.get('panel').hidden,true);assert.equal(elements.get('config-tab').disabled,true);assert.equal(elements.get('setup-telnet_port').value,'2323');assert.equal(elements.get('setup-ssh_enabled').checked,true);
 evaluate("$('setup-bbs_name').value='My unfinished name'");await evaluate('setupRefresh({setup_required:true,recovery_required:false})');assert.equal(reads,1);assert.equal(elements.get('setup-bbs_name').value,'My unfinished name','periodic status does not erase first-run typing');
 function passwords(){evaluate("$('setup-sysop_name').value='Test Sysop';$('setup-sysop_password').value=$('setup-sysop-confirm').value='Caller password 83!';$('setup-system_password').value=$('setup-system-confirm').value='System password 94!'");}
 passwords();evaluate("$('setup-sysop-confirm').value='mismatch'");await evaluate('setupSubmit()');assert.match(elements.get('setup-result').textContent,/must match/);assert.equal(posts,0);
 passwords();let release,body;
 sandbox.fetch=async(path,options)=>{assert.equal(path,'/api/setup');++posts;body=JSON.parse(options.body);await new Promise(resolve=>release=resolve);throw new Error('lost reply');};
 const pending=evaluate('setupSubmit()');assert.equal(elements.get('setup-sysop_password').value,'');assert.equal(elements.get('setup-system_password').value,'');assert.equal(elements.get('setup-submit').disabled,true);assert.equal(body.nodes,4);assert.equal(body.strong_passwords,true);
 release();await pending;assert.equal(evaluate('setupUncertain'),true);await evaluate('setupSubmit()');assert.equal(posts,1,'unknown result cannot cause duplicate setup');assert.equal([...drafts.values()].some(x=>x.includes('Caller password')||x.includes('System password')),false,'setup passwords never enter session storage');
 sandbox.fetch=async(path,options)=>{assert.equal(path,'/api/status');assert.notEqual(options.method,'POST');return response({setup_required:true,recovery_required:false});};
 await evaluate('setupCheck()');assert.equal(evaluate('setupUncertain'),false);assert.equal(elements.get('setup-submit').disabled,false);assert.match(elements.get('setup-result').textContent,/same passwords/);assert.equal(posts,1);
 passwords();sandbox.fetch=async()=>{++posts;await new Promise(resolve=>release=resolve);return response({ok:true,detail:'Setup complete'});};
 const late=evaluate('setupSubmit()');evaluate('lockWebAccess()');release();await late;assert.equal(evaluate('authenticated'),false);assert.equal(elements.get('workspace').hidden,true);assert.equal(elements.get('setup-system_password').value,'','sign-out cannot resurrect secrets');
 evaluate("authenticated=true;setupLoaded=true;$('first-run').hidden=false;page='panel'");await evaluate('setupRefresh({setup_required:false,recovery_required:false})');assert.equal(elements.get('first-run').hidden,true);assert.equal(elements.get('control-tab').disabled,false);assert.equal(elements.get('panel').hidden,false,'completion in another browser restores controls');
 evaluate('setupLoaded=false;setupUncertain=false');
};
