'use strict';
module.exports=async({assert,sandbox,evaluate,elements,response})=>{
  sandbox.document.createElementNS=(_ns,name)=>sandbox.document.createElement(name);
  const data={sample_ms:'1000',bbs_running:false,cpu:{present:true,percent:null,cores:[{name:'cpu0',percent:null}]},memory:{total:'1000000000',used:'400000000',available:'600000000'},disks:{present:false,devices:[],read_rate:null,write_rate:null},network:{present:true,interfaces:['eth0'],received:'9007199254740993',sent:'0',receive_rate:null,send_rate:null},sqlite:{available:true,interval_available:false,reads:null,writes:null,cache_basis_points:null},uptime:{os_ms:'60000',bbs_ms:null}};
  const text=node=>[node.textContent,...node.children.map(child=>typeof child==='object'?text(child):String(child))].join(' ');
  evaluate("authenticated=true;maintenanceActive=false;page='panel';cpClearMonitor();cpMonitorOpen=true");let calls=0;sandbox.fetch=async()=>{++calls;return response(data);};await evaluate('cpRefreshMonitor()');
  assert.match(text(elements.get('cp-monitor-body')),/Sampling/);assert.match(text(elements.get('cp-monitor-body')),/BBS Stopped/);assert.match(text(elements.get('cp-monitor-body')),/Unavailable/);assert.equal(evaluate('cpMonitorData.network.received'),'9007199254740993');
  elements.get('cp-monitor-cores').open=true;elements.get('cp-monitor-cores').ontoggle();evaluate('cpRenderMonitor()');assert.equal(elements.get('cp-monitor-cores').open,true,'per-core expansion survives refresh');
  elements.get('cp-monitor-pause').onclick();const before=calls;data.sample_ms='3000';data.cpu.percent=25;data.cpu.cores[0].percent=25;await evaluate('cpRefreshMonitor()');assert.equal(calls,before,'pause performs no HTTP request');assert.equal(evaluate('cpMonitorLast'),'1000');
  evaluate('cpMonitorPaused=false');await evaluate('cpRefreshMonitor()');assert.match(text(elements.get('cp-monitor-body')),/25.0%/);
  for(let i=0;i<80;++i){data.sample_ms=String(5000+i*2000);sandbox.monitorFixture=data;evaluate('cpMonitorRecord(monitorFixture)');}assert.equal(evaluate('cpMonitorHistory.cpu.length'),60,'history bounded');
  let release;sandbox.fetch=()=>new Promise(resolve=>{release=()=>resolve(response(data));});const pending=evaluate('cpRefreshMonitor()');evaluate('authenticated=false;cpClearCallerViews()');release();await pending;assert.equal(evaluate('cpMonitorData'),null);assert.equal(elements.get('cp-monitor-body').children.length,0,'late reply cannot restore signed-out monitor');
  evaluate('authenticated=true;cpClearMonitor()');
};
