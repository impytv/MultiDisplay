// The navigation page (served as /nav.js; wifi_provision.c): the display's
// screens as buttons, grouped as a tap steps through them, the one on show
// marked. A press switches the display as a tap on it would. Below them, a
// button switches the screen off till the next touch, or lights it.
// Last, a button to report an error, when the update site takes them (#feil's
// data-url, server/feil/feil.py): it shows the screen as it is now and asks
// what is wrong, then gives the screenshot and the text to the display
// (POST /feil, main/report.c), which adds its /status and /log, signs the
// report and sends it there.
const N={oversikt:'Oversikt',kalender:'Kalender',maned:'Måned',klokke:'Klokke',spotify:'Spotify',sonos:'Sonos',europa:'Europa',iss:'ISS',bilder:'Bilder',vaer:'Vær',uke:'Uke',fly:'Fly',skip:'Skip',
nedbor:'Nedbør',avganger:'Avganger',luft:'Luft',tidevann:'Tidevann',satellitt:'Satellitt'};
const nav=document.getElementById('nav'),st=document.createElement('small');
let last=null;
function show(d){last=d;nav.textContent='';st.textContent='';let grp=null,key=null;
d.skjermer.forEach(s=>{const k=s.sted?'s'+s.sted:'g';
if(k!==key){key=k;const h=document.createElement('h2');h.textContent=s.sted?s.navn:'Felles';nav.appendChild(h);
grp=document.createElement('div');grp.className='navg';nav.appendChild(grp)}
const b=document.createElement('button');b.type='button';b.textContent=N[s.type]||s.type;
if(s.vis===d.vises){b.className='on';b.setAttribute('aria-current','true')}
b.onclick=()=>go(s.vis);grp.appendChild(b)});
const o=document.createElement('button');o.type='button';o.className='lt2';
o.textContent=d.lyser?'Slå av skjermen':'Skjermen er av - slå den på';
o.onclick=()=>post({lys:d.lyser?0:1});nav.appendChild(o);nav.appendChild(st)}
// Logged out meanwhile (the password changed): back to the login form.
function got(r){if(r.status===401){location.reload();throw 0}if(!r.ok)throw r.status;return r.json()}
function bad(e){st.className='err';st.textContent=e===403?'Navigasjonssiden er slått av på oppsettsiden.':
'Får ikke kontakt med skjermen.';if(!st.parentNode)nav.appendChild(st)}
function load(){fetch('/nav.json',{cache:'no-store'}).then(got).then(show).catch(bad)}
function post(f){fetch('/nav',{method:'POST',body:new URLSearchParams(f)}).then(got).then(show).catch(bad)}
function go(v){post({vis:v})}
load();setInterval(()=>{if(!document.hidden)load()},5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)load()});
const fe=document.getElementById('feil'),fu=fe&&fe.dataset.url;
function el(t,p,f){const e=document.createElement(t);if(f)f(e);p.appendChild(e);return e}
function report(){fe.textContent='';const msg=el('small',fe);msg.textContent='Tar skjermbilde...';
fetch('/screen.png',{cache:'no-store'}).then(r=>{if(r.status===401){location.reload();throw 0}if(!r.ok)throw r.status;return r.blob()})
.then(png=>{msg.remove();
const img=el('img',fe,e=>{e.src=URL.createObjectURL(png);e.alt='Skjermbildet'});
el('label',fe,e=>{e.htmlFor='ft';e.textContent='Hva er feil? (kan stå tomt)'});
const t=el('textarea',fe,e=>{e.id='ft';e.maxLength=2000;e.rows=4});
el('small',fe,e=>e.textContent='Skjermbildet, teksten, loggen og tilstanden til skjermen sendes til '+new URL(fu,location.href).host+'.');
const send=el('button',fe,e=>{e.type='button';e.textContent='Send feilmeldingen'});
const no=el('button',fe,e=>{e.type='button';e.className='lt2';e.textContent='Avbryt'});
const st2=el('small',fe);
no.onclick=()=>{URL.revokeObjectURL(img.src);start()};
send.onclick=()=>{send.disabled=no.disabled=true;st2.className='';st2.textContent='Sender...';
const s=last&&last.skjermer.find(x=>x.vis===last.vises);
small(png).then(p=>{const f=new FormData();
f.append('skjerm',p,'skjerm.png');f.append('tekst',t.value);f.append('skjermtype',s?(N[s.type]||s.type)+(s.navn?' '+s.navn:''):'');
return fetch('/feil',{method:'POST',body:f}).catch(()=>{throw 'net'})})
.then(r=>{if(r.status===401){location.reload();throw 0}if(!r.ok)throw r.status;return r.json()})
.then(()=>{URL.revokeObjectURL(img.src);fe.textContent='';el('p',fe).textContent='Takk! Feilmeldingen er sendt.';
el('button',fe,e=>{e.type='button';e.className='lt2';e.textContent='Meld fra om en feil til';e.onclick=report})})
.catch(e=>{send.disabled=no.disabled=false;st2.className='err';
const host=new URL(fu,location.href).host;
st2.textContent=e===429?'For mange feilmeldinger på kort tid. Prøv igjen om noen minutter.':
e==='net'?'Får ikke kontakt med skjermen.':e===502?'Skjermen får ikke kontakt med '+host+'.':
e===503?'Skjermen vet ikke hva klokken er ennå. Prøv igjen om litt.':e===507?'Skjermen har ikke minne nok til feilmeldingen akkurat nå. Prøv igjen om litt.':'Feilmeldingen ble ikke tatt imot ('+e+').'})}})
.catch(e=>{msg.className='err';msg.textContent='Fikk ikke skjermbildet fra skjermen.';
el('button',fe,b=>{b.type='button';b.className='lt2';b.textContent='Prøv igjen';b.onclick=report})})}
// The screenshot (uncompressed from the display, about 1 MB) packed by the
// browser, or as it was where it can't.
function small(png){return createImageBitmap(png).then(b=>{const c=document.createElement('canvas');c.width=b.width;c.height=b.height;
c.getContext('2d').drawImage(b,0,0);return new Promise(ok=>c.toBlob(x=>ok(x||png),'image/png'))}).catch(()=>png)}
function start(){fe.textContent='';el('button',fe,e=>{e.type='button';e.className='lt2';e.textContent='Meld fra om feil';e.onclick=report})}
if(fu)start();
