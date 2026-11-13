// The picture page (served as /bilder.js; main/pictures.c): the pictures on
// the display's microSD card, with a button to delete each, and adding
// more - from files, or from an address. Each picture to add is shown as
// the screen will show it, to choose how: the whole picture (black where
// its shape doesn't fit), or filling the screen with the part chosen by
// dragging it and zooming in. It is then made to that, here in the browser
// (800x480 at most), and sent as a JPEG of ~100-250 KB, so the display
// never has a big photo to decode. A picture from an address is fetched by
// the display (/bilder/hent), as a page can't read other sites' pictures.
const W=800,H=480,ZMAX=4,box=document.getElementById('pics');
function vis(e,on){e.style.display=on?'':'none'}
function el(t,p,f){const e=document.createElement(t);if(f)f(e);p.appendChild(e);return e}
function got(r){if(r.status===401){location.reload();throw 0}return r}
const ERR={kort:'Fant ikke noe minnekort i skjermen.',fullt:'Minnekortet er fullt.',mange:'Det er ikke plass til flere bilder. Slett noen først.',
minne:'Skjermen har ikke minne nok akkurat nå. Prøv igjen om litt.','for stor':'Bildet er for stort.',lagring:'Kunne ikke lagre bildet på minnekortet.',
'ikke jpeg':'Skjermen tok ikke imot bildet.','ikke bilde':'Adressen gir ikke et bilde.',henting:'Skjermen fikk ikke hentet bildet fra adressen.',
adresse:'Adressen må begynne med http:// eller https://.',opptatt:'Skjermen henter et annet bilde. Prøv igjen om litt.'};
// A card in the slot that the display can't read (sd_card.c, what_is_on).
const FMT=' Formater det som exFAT eller FAT32 (på en Mac: «ExFAT» eller «MS-DOS (FAT)»), og last siden på nytt. Formatering sletter alt på kortet.';
const BAD={ntfs:'Minnekortet er formatert som NTFS, som skjermen ikke kan lese.'+FMT,uformatert:'Minnekortet er ikke formatert.'+FMT,
ukjent:'Skjermen fant minnekortet, men kan ikke lese filsystemet på det.'+FMT};
function why(r){return r.json().then(j=>ERR[j.feil]||('Feil '+r.status),()=>'Feil '+r.status)}
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
let max=1024,note=['',''],fillOn=false;
try{fillOn=localStorage.getItem('bilder.fyll')==='1'}catch(x){}
function send(blob){return fetch('/bilder/last-opp',{method:'POST',headers:{'Content-Type':'image/jpeg'},body:blob})
.catch(()=>{throw 'Får ikke kontakt med skjermen.'}).then(got).then(r=>r.ok?r.json():why(r).then(t=>{throw t}))}
function msg(e){return e==='les'?'Nettleseren kunne ikke lese bildet (filtypen støttes kanskje ikke).':String(e)}
function draw(d){box.textContent='';max=d.maks_kb||max;
if(!d.kort){el('p',box,p=>{p.className='err';p.textContent=d.kortfeil?BAD[d.kortfeil]||BAD.ukjent:'Fant ikke noe minnekort i skjermen. Bildeskjermen trenger et microSD-kort: sett inn et kort, og last siden på nytt.'});return}
el('small',box,s=>s.textContent=d.bilder.length+(d.bilder.length===1?' bilde':' bilder')+
(d.mb_ledig!==undefined?' · '+d.mb_ledig.toLocaleString('nb-NO')+' MB ledig av '+d.mb_totalt.toLocaleString('nb-NO')+' MB':''));
el('h2',box,h=>h.textContent='Legg til bilder');
el('label',box,l=>{l.htmlFor='pf';l.textContent='Velg bilder'});
const f=el('input',box,i=>{i.type='file';i.accept='image/*';i.multiple=true;i.id='pf'});
el('label',box,l=>{l.htmlFor='pu';l.textContent='Eller fra en adresse'});
const u=el('input',box,i=>{i.type='url';i.id='pu';i.placeholder='https://...';i.inputMode='url'});
const get=el('button',box,b=>{b.type='button';b.className='lt2';b.textContent='Hent bildet'});
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
function lockIn(on){f.disabled=get.disabled=u.disabled=on}
function busy(on){bUp.disabled=bRest.disabled=bSkip.disabled=on}
function start(items){queue=items;total=items.length;n=0;okN=0;bad=[];lockIn(true);next()}
function finish(){cur=null;vis(ed,false);busy(false);lockIn(false);f.value='';
say(bad.length?'err':'',(okN?(okN===1?'Bildet er lagt til.':okN+' bilder er lagt til.'):'')+(bad.length?' '+bad.join(' '):''));
if(okN)load()}
function fresh(img,name){return{img,name,fill:fillOn,z:1,cx:img.width/2,cy:img.height/2}}
function next(){if(!queue.length){finish();return}
const it=queue.shift();n++;
bitmap(it.blob).then(img=>{cur=fresh(img,it.name);vis(ed,true);busy(false);say('','');show();ed.scrollIntoView({block:'nearest'})})
.catch(()=>{bad.push(it.name+': '+msg('les'));next()})}
function upload(e){return render(e).then(send).then(()=>{okN++},x=>{bad.push(e.name+': '+msg(x))})}
bUp.onclick=()=>{busy(true);say('','Laster opp...');upload(cur).then(next)};
bSkip.onclick=()=>{busy(true);next()};
// This one as adjusted, the rest as they are: in the mode chosen, filled
// from the middle.
bRest.onclick=()=>{busy(true);const rest=[cur].concat(queue.splice(0)),from=n;let i=0;
const one=()=>{if(i>=rest.length){finish();return}const e=rest[i++];say('','Laster opp '+(from-1+i)+' av '+total+'...');
(e.img?Promise.resolve(e):bitmap(e.blob).then(img=>fresh(img,e.name)))
.then(upload,()=>{bad.push(e.name+': '+msg('les'))}).then(one)};one()};
f.onchange=()=>{const files=[...f.files];if(files.length)start(files.map(x=>({name:x.name,blob:x})))};
get.onclick=()=>{const a=u.value.trim();if(!/^https?:\/\//i.test(a)){say('err',ERR.adresse);return}
lockIn(true);say('','Henter bildet...');
fetch('/bilder/hent?url='+encodeURIComponent(a),{cache:'no-store'}).catch(()=>{throw 'Får ikke kontakt med skjermen.'}).then(got)
.then(r=>r.ok?r.blob():why(r).then(t=>{throw t}))
.then(b=>{u.value='';let nm='';try{nm=decodeURIComponent(a.split(/[?#]/)[0].split('/').pop())}catch(x){}
start([{name:nm||'Bildet fra adressen',blob:b}])})
.catch(e=>{if(e===0)return;lockIn(false);say('err',msg(e))})};
el('h2',box,h=>h.textContent='Bildene på kortet');
if(!d.bilder.length){el('small',box,s=>s.textContent='Ingen bilder ennå.');return}
const g=el('div',box,x=>{x.style.cssText='display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:.6rem'});
d.bilder.forEach(p=>{const c=el('div',g);
el('img',c,i=>{i.src='/bilder/fil?n='+encodeURIComponent(p.n);i.loading='lazy';i.alt=p.n;i.style.cssText='width:100%;aspect-ratio:5/3;object-fit:contain;background:#000;display:block'});
el('small',c,s=>{s.style.overflowWrap='anywhere';s.textContent=p.n+' · '+p.kb+' kB'+(p.for_stor?' · for stort til å vises':'')});
el('button',c,b=>{b.type='button';b.className='lt2';b.textContent='Slett';b.onclick=()=>{if(!confirm('Slette '+p.n+'?'))return;b.disabled=true;
fetch('/bilder/slett',{method:'POST',body:new URLSearchParams({n:p.n})}).then(got)
.then(r=>r.ok?(note=['',''],load()):why(r).then(t=>{b.disabled=false;alert(t)})).catch(()=>{b.disabled=false;alert('Får ikke kontakt med skjermen.')})}})})}
function load(){fetch('/bilder.json',{cache:'no-store'}).then(got).then(r=>{if(!r.ok)throw r.status;return r.json()}).then(draw)
.catch(e=>{if(e===0)return;box.textContent='';el('p',box,p=>{p.className='err';p.textContent='Får ikke kontakt med skjermen.'})})}
load();
