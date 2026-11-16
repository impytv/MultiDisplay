// The picture page (served as /bilder.js; main/pictures.c): the pictures on
// the display's microSD card, with a button to delete each, and adding
// more from files. Each picture to add is shown as the screen will show it,
// to choose how: the whole picture (black where its shape doesn't fit), or
// filling the screen with the part chosen by dragging it and zooming in. It
// is then made to that, here in the browser (800x480 at most), and sent as
// a JPEG of ~100-250 KB, so the display never has a big photo to decode.
// Sharing (main/share.c): once the update site has approved the display, a
// picture can be shared with a group it is in, and pictures shared by
// others are downloaded by the display by itself (Oppsett, Bilder).
const W=800,H=480,ZMAX=4,box=document.getElementById('pics');
function vis(e,on){e.style.display=on?'':'none'}
function el(t,p,f){const e=document.createElement(t);if(f)f(e);p.appendChild(e);return e}
function got(r){if(r.status===401){location.reload();throw 0}return r}
const ERR={kort:'Fant ikke noe minnekort i skjermen.',fullt:'Minnekortet er fullt.',mange:'Det er ikke plass til flere bilder. Slett noen først.',
minne:'Skjermen har ikke minne nok akkurat nå. Prøv igjen om litt.','for stor':'Bildet er for stort.',lagring:'Kunne ikke lagre bildet på minnekortet.',
'ikke jpeg':'Skjermen tok ikke imot bildet.',opptatt:'Skjermen er opptatt. Prøv igjen om litt.',
'ingen kontakt':'Skjermen fikk ikke kontakt med oppdateringssiden.','ingen adresse':'Deling går gjennom oppdateringssiden, og skjermen har ingen adresse til den (Oppsett, Oppdateringer).',
klokken:'Skjermen vet ikke hva klokken er ennå. Prøv igjen om litt.','ikke godkjent':'Skjermen er ikke godkjent for deling i den gruppen.',
'for mange':'Det er delt mange bilder fra skjermen i dag. Prøv igjen i morgen.',siden:'Oppdateringssiden tok ikke imot det.',
'finnes ikke':'Fant ikke bildet på minnekortet.'};
// A card in the slot that the display can't read (sd_card.c, what_is_on).
const FMT=' Formater det som exFAT eller FAT32 (på en Mac: «ExFAT» eller «MS-DOS (FAT)»), og last siden på nytt. Formatering sletter alt på kortet.';
const BAD={ntfs:'Minnekortet er formatert som NTFS, som skjermen ikke kan lese.'+FMT,uformatert:'Minnekortet er ikke formatert.'+FMT,
ukjent:'Skjermen fant minnekortet, men kan ikke lese filsystemet på det.'+FMT};
function why(r){return r.json().then(j=>ERR[j.feil]||('Feil '+r.status),()=>'Feil '+r.status)}
function post(u,f){return fetch(u,{method:'POST',body:new URLSearchParams(f)}).catch(()=>{throw 'Får ikke kontakt med skjermen.'})
.then(got).then(r=>r.ok?r.json():why(r).then(t=>{throw t}))}
// The picture's pixels, through an <img>: browsers turn a photo upright by
// its EXIF there (and createImageBitmap stalled in a headless test). A big
// photo is brought down to what zooming in ZMAX times needs, so dragging
// it stays quick on a phone.
function bitmap(blob){return new Promise((ok,no)=>{const i=new Image(),u=URL.createObjectURL(blob);
i.onload=()=>{URL.revokeObjectURL(u);ok(i)};i.onerror=()=>{URL.revokeObjectURL(u);no('les')};i.src=u})
.then(i=>{const w=i.width,h=i.height;if(!w||!h)throw 'les';const k=Math.min(1,Math.max(W/w,H/h)*ZMAX);if(k>=1)return i;
const c=document.createElement('canvas');c.width=Math.round(w*k);c.height=Math.round(h*k);const g=c.getContext('2d');
g.imageSmoothingQuality='high';g.drawImage(i,0,0,c.width,c.height);return c})}
// A picture being added: img, fill (or whole), z (zoom, 1 = just fills),
// and cx, cy - the point of img at the middle of the screen.
function scale(e){return Math.max(W/e.img.width,H/e.img.height)*e.z}
function clamp(e){const s=scale(e),hw=W/2/s,hh=H/2/s;e.cx=Math.min(Math.max(e.cx,hw),e.img.width-hw);e.cy=Math.min(Math.max(e.cy,hh),e.img.height-hh)}
function paint(g,e){g.fillStyle='#000';g.fillRect(0,0,W,H);g.imageSmoothingQuality='high';const w=e.img.width,h=e.img.height;
if(e.fill){clamp(e);const s=scale(e);g.drawImage(e.img,W/2-e.cx*s,H/2-e.cy*s,w*s,h*s)}
else{const s=Math.min(W/w,H/h,1);g.drawImage(e.img,(W-w*s)/2,(H-h*s)/2,w*s,h*s)}}
// As the screen gets it: filled, 800x480; whole, its own size or less,
// both sides a multiple of 8 (the display's decoder scales by those).
function render(e){const c=document.createElement('canvas'),g=c.getContext('2d');
if(e.fill){c.width=W;c.height=H;paint(g,e)}
else{const w=e.img.width,h=e.img.height,k=Math.min(W/w,H/h,1);c.width=Math.max(8,Math.floor(w*k/8)*8);c.height=Math.max(8,Math.floor(h*k/8)*8);
g.imageSmoothingQuality='high';g.drawImage(e.img,0,0,c.width,c.height)}
const enc=q=>new Promise(ok=>c.toBlob(ok,'image/jpeg',q));
return enc(.88).then(b=>b&&b.size>max*1024?enc(.7):b).then(b=>{if(!b)throw 'les';return b})}
let max=1024,note=['',''],fillOn=false,shareTo='',sh=null,last=null;
try{fillOn=localStorage.getItem('bilder.fyll')==='1';shareTo=localStorage.getItem('bilder.del')||''}catch(x){}
function send(blob){return fetch('/bilder/last-opp',{method:'POST',headers:{'Content-Type':'image/jpeg'},body:blob})
.catch(()=>{throw 'Får ikke kontakt med skjermen.'}).then(got).then(r=>r.ok?r.json():why(r).then(t=>{throw t}))}
function msg(e){return e==='les'?'Nettleseren kunne ikke lese bildet (filtypen støttes kanskje ikke).':String(e)}
// The groups the display may share with, once approved.
function groups(){return sh&&sh.status==='godkjent'&&sh.grupper?sh.grupper:[]}
function shareOne(n,g){return post('/bilder/del',{n,gruppe:g})}
let lst,shBox,shSel,shLab;
function draw(d){last=d;box.textContent='';max=d.maks_kb||max;
if(!d.kort){el('p',box,p=>{p.className='err';p.textContent=d.kortfeil?BAD[d.kortfeil]||BAD.ukjent:'Fant ikke noe minnekort i skjermen. Bildeskjermen trenger et microSD-kort: sett inn et kort, og last siden på nytt.'});return}
el('small',box,s=>s.textContent=d.bilder.length+(d.bilder.length===1?' bilde':' bilder')+
(d.mb_ledig!==undefined?' · '+d.mb_ledig.toLocaleString('nb-NO')+' MB ledig av '+d.mb_totalt.toLocaleString('nb-NO')+' MB':''));
el('h2',box,h=>h.textContent='Legg til bilder');
el('label',box,l=>{l.htmlFor='pf';l.textContent='Velg bilder'});
const f=el('input',box,i=>{i.type='file';i.accept='image/*';i.multiple=true;i.id='pf'});
const st=el('small',box);
function say(c,t){note=[c,t];st.className=c;st.textContent=t}say(note[0],note[1]);
// The editor: one picture at a time, as the screen will show it.
const ed=el('div',box,x=>{vis(x,false);x.style.marginTop='.8rem'});
const head=el('strong',ed,x=>{x.style.cssText='display:block;overflow-wrap:anywhere'});
const modes=el('div',ed,x=>x.className='chk');
function radio(v,t){const i=el('input',el('label',modes),i=>{i.type='radio';i.name='pmode';i.value=v});i.parentNode.appendChild(document.createTextNode(t));return i}
const rFill=radio('1','Fyll skjermen'),rWhole=radio('0','Vis hele bildet');
const cv=el('canvas',ed,c=>{c.width=W;c.height=H;c.style.cssText='display:block;width:100%;max-width:800px;aspect-ratio:5/3;background:#000;touch-action:none;margin:.4rem 0'});
const zl=el('label',ed,l=>{l.htmlFor='pz';l.textContent='Forstørr'});
const zr=el('input',ed,i=>{i.type='range';i.id='pz';i.min=1;i.max=ZMAX;i.step=.01;i.value=1});
const hint=el('small',ed);
shLab=el('label',ed,l=>{l.htmlFor='pd';l.textContent='Del med'});
shSel=el('select',ed,s=>{s.id='pd';s.onchange=()=>{shareTo=s.value;try{localStorage.setItem('bilder.del',shareTo)}catch(x){}}});
const bUp=el('button',ed,b=>{b.type='button';b.textContent='Last opp'});
const bRest=el('button',ed,b=>{b.type='button';b.className='lt2'});
const bSkip=el('button',ed,b=>{b.type='button';b.className='lt2';b.textContent='Hopp over'});
let cur=null,queue=[],total=0,n=0,okN=0,bad=[];
function show(){if(!cur)return;
head.textContent=(total>1?'Bilde '+n+' av '+total+': ':'')+cur.name;
rFill.checked=cur.fill;rWhole.checked=!cur.fill;vis(zl,cur.fill);vis(zr,cur.fill);zr.value=cur.z;
cv.style.cursor=cur.fill?'grab':'default';
hint.textContent=cur.fill?'Dra i bildet for å velge hvilken del som vises, og forstørr med glideren, musehjulet eller to fingre.'
:'Hele bildet vises, med svart rundt der formen ikke passer skjermen.';
vis(bRest,queue.length>0);bRest.textContent='Last opp resten uten å justere ('+queue.length+')';
paint(cv.getContext('2d'),cur)}
function mode(fill){cur.fill=fillOn=fill;try{localStorage.setItem('bilder.fyll',fill?'1':'0')}catch(x){}show()}
rFill.onchange=()=>mode(true);rWhole.onchange=()=>mode(false);
function zoom(z){cur.z=Math.min(Math.max(z,1),ZMAX);show()}
zr.oninput=()=>zoom(+zr.value);
// Dragging moves the picture under the screen's frame; two fingers zoom.
const pts=new Map();let pinch=0;
cv.onpointerdown=v=>{if(!cur||!cur.fill)return;try{cv.setPointerCapture(v.pointerId)}catch(x){}pts.set(v.pointerId,[v.clientX,v.clientY]);pinch=0;cv.style.cursor='grabbing'};
cv.onpointermove=v=>{const p=pts.get(v.pointerId);if(!p||!cur)return;
if(pts.size===1){const k=W/cv.clientWidth/scale(cur);cur.cx-=(v.clientX-p[0])*k;cur.cy-=(v.clientY-p[1])*k}
pts.set(v.pointerId,[v.clientX,v.clientY]);
if(pts.size===2){const[a,b]=[...pts.values()],dist=Math.hypot(a[0]-b[0],a[1]-b[1]);if(pinch)cur.z=Math.min(Math.max(cur.z*dist/pinch,1),ZMAX);pinch=dist}
show()};
cv.onpointerup=cv.onpointercancel=v=>{pts.delete(v.pointerId);pinch=0;if(cur&&cur.fill)cv.style.cursor='grab'};
cv.onwheel=v=>{if(!cur||!cur.fill)return;v.preventDefault();zoom(cur.z*Math.exp(-v.deltaY/400))};
function busy(on){bUp.disabled=bRest.disabled=bSkip.disabled=on}
function start(items){queue=items;total=items.length;n=0;okN=0;bad=[];f.disabled=true;next()}
function finish(){cur=null;vis(ed,false);busy(false);f.disabled=false;f.value='';
say(bad.length?'err':'',(okN?(okN===1?'Bildet er lagt til.':okN+' bilder er lagt til.'):'')+(bad.length?' '+bad.join(' '):''));
if(okN)load()}
function fresh(img,name){return{img,name,fill:fillOn,z:1,cx:img.width/2,cy:img.height/2}}
function next(){if(!queue.length){finish();return}
const it=queue.shift();n++;
bitmap(it.blob).then(img=>{cur=fresh(img,it.name);vis(ed,true);busy(false);say('','');show();ed.scrollIntoView({block:'nearest'})})
.catch(()=>{bad.push(it.name+': '+msg('les'));next()})}
// Onto the card, and shared if a group is chosen: a picture that couldn't
// be shared is still on the card, and says so.
function upload(e){const g=groups().includes(shSel.value)?shSel.value:'';
return render(e).then(send).then(r=>{okN++;if(g&&r.n)return shareOne(r.n,g).catch(x=>{bad.push(e.name+' ble lagt til, men ikke delt: '+msg(x))})},
x=>{bad.push(e.name+': '+msg(x))})}
bUp.onclick=()=>{busy(true);say('','Laster opp...');upload(cur).then(next)};
bSkip.onclick=()=>{busy(true);next()};
// This one as adjusted, the rest as they are: in the mode chosen, filled
// from the middle.
bRest.onclick=()=>{busy(true);const rest=[cur].concat(queue.splice(0)),from=n;let i=0;
const one=()=>{if(i>=rest.length){finish();return}const e=rest[i++];say('','Laster opp '+(from-1+i)+' av '+total+'...');
(e.img?Promise.resolve(e):bitmap(e.blob).then(img=>fresh(img,e.name)))
.then(upload,()=>{bad.push(e.name+': '+msg('les'))}).then(one)};one()};
f.onchange=()=>{const files=[...f.files];if(files.length)start(files.map(x=>({name:x.name,blob:x})))};
el('h2',box,h=>h.textContent='Deling');
shBox=el('div',box);
el('h2',box,h=>h.textContent='Bildene på kortet');
lst=el('div',box);
drawShare();drawList()}
// The sharing section and the editor's choice of group, from sh.
function drawShare(){if(!shBox)return;shBox.textContent='';
const gs=groups();shSel.textContent='';
el('option',shSel,o=>{o.value='';o.textContent='Ikke del'});
gs.forEach(g=>el('option',shSel,o=>{o.value=g;o.textContent='Gruppen '+g}));
shSel.value=gs.includes(shareTo)?shareTo:'';vis(shLab,gs.length>0);vis(shSel,gs.length>0);
const p=t=>el('small',shBox,s=>{s.style.display='block';s.textContent=t});
if(!sh){p('Spør oppdateringssiden om deling...');return}
if(sh.feil){el('small',shBox,s=>{s.className='err';s.textContent=ERR[sh.feil]||sh.feil});return}
if(sh.status==='venter'){p('Bilder kan deles med andre skjermer gjennom oppdateringssiden. Skjermen har bedt om å få være med, '+
'og venter på at den som driver oppdateringssiden godkjenner den og velger hvilke grupper den skal være med i. '+
'Skjermens nøkkel er '+(sh.nokkel||'?')+'.');return}
if(sh.status!=='godkjent'){p('Skjermen er ikke godkjent for deling av bilder.');return}
if(sh.opptatt)el('small',shBox,s=>{s.className='err';s.style.display='block';s.textContent='Kallenavnet brukes allerede av en annen skjerm. Velg et annet under Bilder i oppsettet.'});
if(!gs.length){p('Skjermen er godkjent for deling, men er ikke med i noen grupper ennå.');return}
p('Skjermen er med i '+(gs.length===1?'gruppen ':'gruppene ')+gs.join(', ')+'. Del et bilde med knappen under det, eller velg en gruppe når du legger til bilder.');
p(sh.auto?'Bilder andre deler med '+(sh.fra?'gruppene '+sh.fra.split(',').join(', '):'gruppene')+' lastes ned av seg selv.'
:'Bilder andre deler lastes ikke ned. Det slås på under Bilder i oppsettet.');
// Shared from here, but no longer on the card.
const onCard=new Set((last&&last.bilder||[]).map(b=>b.n));
(sh.mine||[]).filter(m=>!onCard.has(m.navn)).forEach(m=>el('div',shBox,x=>{
el('small',x,s=>s.textContent=(m.navn||m.id)+' er delt med '+m.gruppe+', men er ikke på kortet lenger. ');
el('button',x,b=>{b.type='button';b.className='lt2';b.textContent='Slutt å dele';b.onclick=()=>unshare(m,b)})}))}
function unshare(m,b){if(!confirm('Slutte å dele '+(m.navn||'bildet')+' med '+m.gruppe+'? Det slettes fra de andre skjermene.'))return;
b.disabled=true;post('/bilder/slutt-a-dele',{id:m.id}).then(loadShare,t=>{b.disabled=false;alert(t)})}
function drawList(){if(!lst)return;lst.textContent='';const d=last;
if(!d.bilder.length){el('small',lst,s=>s.textContent='Ingen bilder ennå.');return}
const g=el('div',lst,x=>{x.style.cssText='display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:.6rem'});
const gs=groups(),mine=(sh&&sh.mine)||[];
d.bilder.forEach(p=>{const c=el('div',g);
el('img',c,i=>{i.src='/bilder/fil?n='+encodeURIComponent(p.n);i.loading='lazy';i.alt=p.n;i.style.cssText='width:100%;aspect-ratio:5/3;object-fit:contain;background:#000;display:block'});
const shared=mine.filter(m=>m.navn===p.n);
el('small',c,s=>{s.style.cssText='display:block;overflow-wrap:anywhere';s.textContent=(p.delt?'Delt av '+(p.fra||'en annen skjerm'):p.n)+' · '+p.kb+' kB'+(p.for_stor?' · for stort til å vises':'')+
(shared.length?' · delt med '+shared.map(m=>m.gruppe).join(', '):'')});
if(!p.delt){gs.filter(x=>!shared.some(m=>m.gruppe===x)).forEach(x=>el('button',c,b=>{b.type='button';b.className='lt2';
b.textContent=gs.length===1?'Del':'Del med '+x;b.onclick=()=>{b.disabled=true;shareOne(p.n,x).then(loadShare,t=>{b.disabled=false;alert(t)})}}));
shared.forEach(m=>el('button',c,b=>{b.type='button';b.className='lt2';b.textContent=shared.length===1?'Slutt å dele':'Slutt å dele med '+m.gruppe;b.onclick=()=>unshare(m,b)}))}
el('button',c,b=>{b.type='button';b.className='lt2';b.textContent=p.delt?'Fjern':'Slett';b.onclick=()=>{
if(!confirm(p.delt?'Fjerne bildet fra denne skjermen? Det lastes ikke ned igjen.':'Slette '+p.n+'?'+(shared.length?' Det er fortsatt delt med de andre.':'')))return;b.disabled=true;
fetch('/bilder/slett',{method:'POST',body:new URLSearchParams({n:p.n})}).then(got)
.then(r=>r.ok?(note=['',''],load()):why(r).then(t=>{b.disabled=false;alert(t)})).catch(()=>{b.disabled=false;alert('Får ikke kontakt med skjermen.')})}})})}
function loadShare(){fetch('/bilder/deling.json',{cache:'no-store'}).then(got)
.then(r=>r.ok?r.json():r.json().then(j=>({feil:j.feil||'Feil '+r.status}),()=>({feil:'Feil '+r.status})))
.catch(e=>{if(e===0)throw e;return{feil:'Får ikke kontakt med skjermen.'}})
.then(j=>{sh=j;drawShare();if(last&&last.kort)drawList()},()=>{})}
function load(){fetch('/bilder.json',{cache:'no-store'}).then(got).then(r=>{if(!r.ok)throw r.status;return r.json()}).then(d=>{draw(d);if(d.kort)loadShare()})
.catch(e=>{if(e===0)return;box.textContent='';el('p',box,p=>{p.className='err';p.textContent='Får ikke kontakt med skjermen.'})})}
load();
