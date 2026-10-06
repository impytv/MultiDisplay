// The navigation page (served as /nav.js; wifi_provision.c): the display's
// screens as buttons, grouped as a tap steps through them, the one on show
// marked. A press switches the display as a tap on it would.
const N={oversikt:'Oversikt',kalender:'Kalender',europa:'Europa',vaer:'Vær',uke:'Uke',fly:'Fly',skip:'Skip',
nedbor:'Nedbør',avganger:'Avganger',luft:'Luft',tidevann:'Tidevann',satellitt:'Satellitt'};
const nav=document.getElementById('nav'),st=document.createElement('small');
function show(d){nav.textContent='';st.textContent='';let grp=null,key=null;
d.skjermer.forEach(s=>{const k=s.sted?'s'+s.sted:'g';
if(k!==key){key=k;const h=document.createElement('h2');h.textContent=s.sted?s.navn:'Felles';nav.appendChild(h);
grp=document.createElement('div');grp.className='navg';nav.appendChild(grp)}
const b=document.createElement('button');b.type='button';b.textContent=N[s.type]||s.type;
if(s.vis===d.vises){b.className='on';b.setAttribute('aria-current','true')}
b.onclick=()=>go(s.vis);grp.appendChild(b)});nav.appendChild(st)}
// Logged out meanwhile (the password changed): back to the login form.
function got(r){if(r.status===401){location.reload();throw 0}if(!r.ok)throw r.status;return r.json()}
function bad(e){st.className='err';st.textContent=e===403?'Navigasjonssiden er slått av på oppsettsiden.':
'Får ikke kontakt med skjermen.';if(!st.parentNode)nav.appendChild(st)}
function load(){fetch('/nav.json',{cache:'no-store'}).then(got).then(show).catch(bad)}
function go(v){fetch('/nav',{method:'POST',body:new URLSearchParams({vis:v})}).then(got).then(show).catch(bad)}
load();setInterval(()=>{if(!document.hidden)load()},5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)load()});
