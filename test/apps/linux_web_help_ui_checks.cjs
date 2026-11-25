'use strict';
module.exports=async({assert,sandbox,evaluate,elements,response})=>{
  evaluate("authenticated=true;maintenanceActive=false;route='general.board_identity';dirty=true");
  sandbox.fetch=async()=>response({product:'Spitfire-X BBS vfixture',platform:'Linux AMD64',release_date:'Fixture date'});
  await evaluate('cpOpenAbout()');assert.equal(elements.get('help-about-dialog').open,true);assert.equal(elements.get('help-about-version').textContent,'Spitfire-X BBS vfixture');assert.equal(evaluate('dirty'),true,'About preserves unsaved configuration');
  assert.equal(elements.get('help-about-image').children[0].src,'/help/sysop/images/sfx-about-approved.png');
  elements.get('help-about-close').onclick();assert.equal(elements.get('help-about-dialog').open,false);assert.equal(evaluate('route'),'general.board_identity');
  let release;sandbox.fetch=()=>new Promise(resolve=>{release=()=>resolve(response({product:'Late private result'}));});const pending=evaluate('cpOpenAbout()');evaluate('authenticated=false;cpClearCallerViews()');release();await pending;
  assert.equal(elements.get('help-about-dialog').open,false);assert.equal(elements.get('help-about-version').textContent,'','late response cannot reopen closed authentication');
  evaluate('authenticated=true;dirty=false');
};
