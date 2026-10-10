// The setup page's behaviour (served as /setup.js; wifi_provision.c):
// firmware upload, backup, password display, health and update status, location summaries,
// adding/removing/moving locations, place search, unsaved-changes marker
// and the checks before saving.
function E(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML}
const F=document.getElementById('cf');
fwb.onclick=()=>{let f=fw.files[0];if(!f)return;fwb.disabled=true;fws.textContent='Laster opp...';let x=new XMLHttpRequest();x.open('POST','/ota');
x.upload.onprogress=e=>fws.textContent='Laster opp '+Math.round(100*e.loaded/e.total)+' %';
x.onload=()=>{fws.textContent=x.responseText;fwb.disabled=x.status==200};x.onerror=()=>{fws.textContent='Opplastingen feilet';fwb.disabled=false};x.send(f)};
function D(){fetch('/status').then(r=>r.json()).then(s=>{let u=s.oppetid_s,t=u>=86400?Math.floor(u/86400)+' d ':'';
t+=Math.floor(u%86400/3600)+' t '+Math.floor(u%3600/60)+' min';
let h='<small>Versjon '+E(s.versjon)+', oppe i '+t+'. Sist startet av: '+E(s.omstart)+'.';
if(s.wifi_dbm!==undefined)h+=' WiFi '+s.wifi_dbm+' dBm.';h+=' Klokka: '+E(s.klokke)+'.';
h+=' Minne: '+Math.round(s.minne.intern_ledig/1024)+' KB internt (lavest '+Math.round(s.minne.intern_lavest/1024)+'), '+Math.round(s.minne.psram_ledig/1024)+' KB PSRAM.';
if(s.krasjdump)h+=' <b>Krasjdump lagret'+(s.krasjgrunn?': '+E(s.krasjgrunn):'')+'.</b>';h+='</small>';
s.tjenester.forEach(v=>{h+='<br><small>'+(v.feiler?'&#9888; ':'&#10003; ')+E(v.navn)+(v.sist_ok=='aldri'?': aldri hentet':': ok '+E(v.sist_ok))+(v.feiler?', feil '+E(v.sist_feil)+' ('+E(v.feil)+')':'')+'</small>'});
diag.innerHTML=h;cdl.hidden=!s.krasjdump}).catch(e=>{diag.innerHTML='<small>Ikke tilgjengelig i oppsettmodus.</small>'})}
cde.onclick=e=>{e.preventDefault();fetch('/coredump/erase',{method:'POST'}).then(D)};D();
cfgsec.onchange=()=>{cfgdl.href='/config.json'+(cfgsec.checked?'?hemmeligheter=1':'')};
/* "Vis passordet": the password field named in data-show, in plain text. */
document.querySelectorAll('[data-show]').forEach(c=>c.onchange=()=>{F.elements[c.dataset.show].type=c.checked?'text':'password'});
cfgb.onclick=()=>{let f=cfgf.files[0];if(!f)return;cfgb.disabled=true;cfgs.textContent='Gjenoppretter...';
fetch('/config.json',{method:'POST',body:f}).then(r=>r.text().then(t=>{cfgs.textContent=t;cfgb.disabled=r.ok}))
.catch(e=>{cfgs.textContent='Feilet';cfgb.disabled=false})};
function U(){fetch('/ota/status').then(r=>r.json()).then(s=>{let t='Kjører '+s.running+'. ';
if(s.busy)t+=s.progress>=0?'Installerer '+s.available.version+': '+s.progress+' %':'Sjekker...';
else if(s.checked)t+='Sist sjekket '+s.checked+': '+s.result;else t+='Ikke sjekket ennå.';
otast.innerHTML='<small>'+E(t)+'</small>';
let a=s.available;otanew.innerHTML=a?'<p style="margin:.4rem 0 0"><b>'+E(a.version)+'</b>'+(a.released?' ('+E(a.released)+')':'')+(a.notes?': '+E(a.notes):'')+'</p>':'';
otains.hidden=!a||s.busy;otachk.disabled=s.busy;if(s.busy)setTimeout(U,1500)})
.catch(e=>{otast.innerHTML='<small>Ikke tilgjengelig i oppsettmodus.</small>';otachk.disabled=true})}
function P(u){otachk.disabled=true;otains.hidden=true;fetch(u,{method:'POST'}).then(()=>setTimeout(U,800))}
otachk.onclick=()=>P('/ota/check');otains.onclick=()=>{if(confirm('Installere nå? Skjermen starter på nytt.'))P('/ota/install')};U();
fetch('/scan').then(r=>r.json()).then(l=>{let d=document.getElementById('nets');l.forEach(n=>{let o=document.createElement('option');o.value=n.s;d.appendChild(o)})}).catch(e=>{});
let L=[...document.querySelectorAll('fieldset[data-loc]')];
const N=(f,n)=>f.querySelector('[name='+n+f.dataset.loc+']');
/* Each location: its summary line, and only the fields its screens need. */
function T(f){let i=f.dataset.loc,n=N(f,'name').value.trim(),
v=[...f.querySelectorAll('.vis')].filter(x=>x.checked).map(x=>x.parentNode.textContent.trim().toLowerCase());
f.parentNode.querySelector('.lt').innerHTML='Sted '+(+i+1)+': '+(n?E(n)+' <span class=ls>'+E(v.join(', '))+'</span>':'<span class=ls>uten navn</span>');
f.querySelectorAll('.sr').forEach(r=>{r.querySelector('.rot').style.visibility=r.querySelector('.vis').checked?'':'hidden'});
f.querySelectorAll('[data-need]').forEach(d=>{d.hidden=!N(f,d.dataset.need).checked})}
/* ▲▼ only between the locations in use. */
function M(){let u=L.filter(f=>!f.parentNode.hidden);L.forEach(f=>f.parentNode.querySelectorAll('.mv').forEach(b=>{
let k=u.indexOf(f)+ +b.dataset.d;b.disabled=k<0||k>=u.length}));addloc.hidden=u.length==L.length}
L.forEach(f=>{T(f);f.addEventListener('input',()=>T(f));f.addEventListener('change',()=>T(f))});M();
document.querySelectorAll('.mv').forEach(b=>b.onclick=e=>{e.preventDefault();let a=b.closest('details').querySelector('fieldset'),
j=+a.dataset.loc+ +b.dataset.d,o=L[j];if(!o||o.parentNode.hidden)return;
a.querySelectorAll('input[name]').forEach(x=>{let y=o.querySelector('[name='+x.name.replace(/\d+$/,j)+']');
if(!y)return;if(x.type=='checkbox'){let c=x.checked;x.checked=y.checked;y.checked=c}else{let v=x.value;x.value=y.value;y.value=v}});
[a,o].forEach(f=>f.querySelectorAll('input[name^=dep]').forEach(x=>x.dispatchEvent(new Event('change'))));
T(a);T(o);S();if(o.parentNode.scrollIntoView)o.parentNode.scrollIntoView({block:'nearest',behavior:'smooth'})});
/* Add: the first unused block; remove: empty it (it isn't saved). */
addloc.onclick=()=>{let d=L.map(f=>f.parentNode).find(d=>d.hidden);if(!d)return;d.hidden=false;d.open=true;M();S();d.querySelector('.plq').focus()};
document.querySelectorAll('.rm').forEach(b=>b.onclick=()=>{let f=b.closest('fieldset');
if(!confirm('Fjerne '+(N(f,'name').value||'stedet')+'?'))return;
['name','lat','lon','shipname','shiplat','shiplon'].forEach(k=>N(f,k).value='');f.querySelectorAll('input[type=checkbox]').forEach(x=>x.checked=false);
let dp=N(f,'dep');dp.value='';dp.dispatchEvent(new Event('change'));f.parentNode.open=false;f.parentNode.hidden=true;T(f);M();S()});
/* Place search (Kartverket's place names): for the location, and for
where its ships are centred. */
function Q(q,h,pick){let t,c;q.oninput=()=>{clearTimeout(t);if(c)c.abort();let v=q.value.trim();
if(v.length<2){h.innerHTML='';return}t=setTimeout(()=>{c=new AbortController();
fetch('https://ws.geonorge.no/stedsnavn/v1/navn?fuzzy=true&utkoordsys=4258&treffPerSide=8&side=1&sok='+encodeURIComponent(v),{signal:c.signal})
.then(r=>r.json()).then(d=>{h.innerHTML='';(d.navn||[]).forEach(n=>{let p=n.representasjonspunkt,b=document.createElement('button');b.type='button';
b.innerHTML=E(n['skrivemåte'])+' <small>'+E(n.navneobjekttype)+', '+E((n.kommuner||[]).map(k=>k.kommunenavn).join(', '))+'</small>';
b.onclick=()=>{pick(n['skrivemåte'],p.nord.toFixed(4),p['øst'].toFixed(4));h.innerHTML='';q.value='';S()};h.appendChild(b)});
if(!h.children.length)h.innerHTML='<small>Ingen treff</small>'})
.catch(e=>{if(e.name!='AbortError')h.innerHTML='<small>Søket trenger internett.</small>'})},300)};
q.onkeydown=e=>{if(e.key=='Enter')e.preventDefault()}}
L.forEach(f=>{const set=(k,n,a,o)=>{N(f,k+'name').value=n;N(f,k+'lat').value=a;N(f,k+'lon').value=o;T(f)};
Q(f.querySelector('.plq'),f.querySelector('.plq+.hits'),(n,a,o)=>set('',n,a,o));
Q(f.querySelector('.spq'),f.querySelector('.spq+.hits'),(n,a,o)=>set('ship',n,a,o))});
/* Unsaved changes. */
let dirty=false;function S(){dirty=true;document.getElementById('dirty').textContent='Ulagrede endringer'}
/* Not settings: the place searches, "Vis passordet" and the backup's checkbox. */
const SQ=x=>x.classList.contains('plq')||x.classList.contains('spq')||x.dataset.show!==undefined||x.id=='cfgsec';
F.addEventListener('input',e=>{if(!SQ(e.target))S()});F.addEventListener('change',e=>{if(!SQ(e.target)&&e.target.type!='file')S()});
window.addEventListener('beforeunload',e=>{if(dirty){e.preventDefault();e.returnValue=''}});
/* Times: phones' number pads often have no ':', so 2230, 930, 22.30 and
22,30 are taken as well and shown as 22:30 once the field is left. */
const TM=['dimstart','dimend','otaat'];
function hm(v){let m=v.trim().match(/^(\d{1,2})[:.,]?(\d{2})$/);return m&&+m[1]<24&&+m[2]<60?m[1].padStart(2,'0')+':'+m[2]:null}
TM.forEach(k=>{let x=F.querySelector('[name='+k+']');x.addEventListener('change',()=>{let t=hm(x.value);if(t!==null)x.value=t})});
/* Checks before saving, shown by the field. */
function X(el,msg){el.classList.add('bad');let s=document.createElement('span');s.className='err';s.textContent=msg;el.insertAdjacentElement('afterend',s);return el}
F.addEventListener('submit',e=>{F.querySelectorAll('.err').forEach(x=>x.remove());F.querySelectorAll('.bad').forEach(x=>x.classList.remove('bad'));
let bad=[];const num=v=>v.trim()!==''&&isFinite(+v.replace(',','.'));
F.querySelectorAll('input[type=number]').forEach(x=>{if(x.closest('[hidden]'))return;let v=+x.value;
if(x.value===''||v<+x.min||v>+x.max)bad.push(X(x,'Må være fra '+x.min+' til '+x.max+'.'))});
L.forEach(f=>{if(f.parentNode.hidden)return;let a=N(f,'lat'),o=N(f,'lon'),n=N(f,'name');
if(!n.value.trim()&&!a.value.trim()&&!o.value.trim())return;
if(!num(a.value)||Math.abs(+a.value.replace(',','.'))>90)bad.push(X(a,'Breddegrad fra -90 til 90.'));
if(!num(o.value)||Math.abs(+o.value.replace(',','.'))>180)bad.push(X(o,'Lengdegrad fra -180 til 180.'));
let sa=N(f,'shiplat'),so=N(f,'shiplon');if(sa.closest('[hidden]')||!sa.value.trim()&&!so.value.trim())return;
if(!num(sa.value)||Math.abs(+sa.value.replace(',','.'))>90)bad.push(X(sa,'Breddegrad fra -90 til 90, eller tomt.'));
if(!num(so.value)||Math.abs(+so.value.replace(',','.'))>180)bad.push(X(so,'Lengdegrad fra -180 til 180, eller tomt.'))});
TM.forEach(k=>{let x=F.querySelector('[name='+k+']'),t=hm(x.value);if(t===null)bad.push(X(x,'Skriv tid som TT:MM, f.eks. 22:30 eller 2230.'));else x.value=t});
F.querySelectorAll('[name^=calurl]').forEach(x=>{let v=x.value.trim();x.value=v;if(v&&!/^(https?|webcal):\/\/\S+$/.test(v))bad.push(X(x,'Adressen må begynne med https://, http:// eller webcal://.'))});
let ss=F.querySelector('[name=ssid]');if(!ss.value.trim())bad.push(X(ss,'Skriv inn WiFi-nettet.'));
F.querySelectorAll('[name^=lat],[name^=lon],[name^=shiplat],[name^=shiplon]').forEach(x=>x.value=x.value.replace(',','.'));
if(bad.length){e.preventDefault();let d=bad[0].closest('details');while(d){d.open=true;d=d.parentNode.closest('details')}
bad[0].scrollIntoView({block:'center'});bad[0].focus();return}dirty=false});

