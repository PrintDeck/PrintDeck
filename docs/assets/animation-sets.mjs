import {assetRoot, validateCatalog, validateManifest, families, collectionMarkup, reactionsMarkup, variantsMarkup, pageMarkup, safeId} from './animation-catalog.mjs';
const config = JSON.parse(document.querySelector('#animation-config').textContent);
const t = source => config.labels[source] || source;
const options = {...config,t};
const teaser = document.querySelector('[data-animation-teaser]');
let catalog, generation = 0;
const cache = new Map();
let heroTimer;
let heroRotation = 0;
function stopHeroRotation() {
  clearInterval(heroTimer);
  heroRotation++;
}
function startHeroRotation(set, manifest) {
  stopHeroRotation();
  const rotation = heroRotation;
  const hero = document.querySelector('.animation-hero-art img');
  if (!hero) return;
  const sources = Object.values(manifest.files).map(file =>
    `${assetRoot}466x466/sets/${set.id}/${file.file}?v=${file.sha256}`);
  let previous = -1;
  let pending = false;
  heroTimer = setInterval(async () => {
    if (document.hidden || pending || !sources.length) return;
    const candidates = sources.map((_, index) => index).filter(index => index !== previous);
    const next = candidates[Math.floor(Math.random() * candidates.length)];
    if (next === undefined) return;
    pending = true;
    // Decode before swapping so a slower connection keeps the current image visible.
    const image = new Image();
    image.referrerPolicy = 'no-referrer';
    image.src = sources[next];
    try {
      await image.decode();
      if (rotation !== heroRotation || !hero.isConnected) return;
      hero.src = image.src;
      hero.hidden = false;
      hero.style.display = '';
      hero.nextElementSibling.hidden = true;
      previous = next;
    } catch { /* Keep the last successfully loaded reaction. */ }
    finally { pending = false; }
  }, 3000);
}
window.addEventListener('pagehide', stopHeroRotation);
window.addEventListener('pageshow', event => { if (event.persisted) refresh(); });
function watchImages() {
  document.querySelectorAll('.animation-display img').forEach(img => {
    const fail = () => {img.hidden=true;img.style.display='none';img.nextElementSibling.hidden=false;};
    img.addEventListener('error',fail,{once:true});
    if(img.complete && !img.naturalWidth) fail();
  });
}
async function json(path) {
  const response = await fetch(assetRoot+path,{cache:'no-cache',credentials:'omit',referrerPolicy:'no-referrer',redirect:'error',signal:AbortSignal.timeout(10000)});
  if (!response.ok) throw Error('Unavailable');
  const reader = response.body.getReader(); let bytes=0, content=''; const decoder=new TextDecoder();
  while(true){const {done,value}=await reader.read();if(done)break;bytes+=value.byteLength;if(bytes>65536){await reader.cancel();throw Error('Too large');}content+=decoder.decode(value,{stream:true});}
  return JSON.parse(content+decoder.decode());
}
function notice(message, retry=false) {
  const el=document.querySelector('.animation-notice');if(!el)return;
  el.textContent=t(message);el.hidden=!message;
  document.querySelector('.animation-retry').hidden=!retry;
}
async function detail(family, selected, replacePage=false) {
  const run=++generation;
  stopHeroRotation();
  const set=family.variants.find(v=>v.id===selected)||family.variants[0];
  notice('Loading reactions…');
  document.querySelector('#animation-grid').setAttribute('aria-busy','true');
  try {
    const key=`${set.id}@${set.version}`;
    const manifest=cache.get(key)||validateManifest(await json(`466x466/sets/${set.id}/manifest.json?v=${encodeURIComponent(set.version)}`),set.id);
    cache.set(key,manifest);
    if(run!==generation)return;
    if(replacePage) {
      document.querySelector('main').innerHTML=pageMarkup(catalog,{[set.id]:manifest},family.id,{...options,selected:set.id});
      document.title=`${family.name} · ${t('Animation sets')} · PrintDeck`;
    }
    document.querySelector('.animation-hero h1').textContent=family.name;
    document.title=`${family.name} · ${t('Animation sets')} · PrintDeck`;
    document.querySelector('#animation-grid').className='animation-reaction-grid';
    document.querySelector('#animation-grid').innerHTML=reactionsMarkup(set,manifest,t);
    document.querySelector('#animation-variants').innerHTML=variantsMarkup(family,set.id,options);
    const hero=document.querySelector('.animation-hero-art img');
    if(hero){hero.src=assetRoot+set.preview+'?v='+encodeURIComponent(set.version);hero.hidden=false;hero.style.display='';hero.nextElementSibling.hidden=true;}
    startHeroRotation(set,manifest);
    notice('');watchImages();
  } catch {if(run===generation)notice('Preview unavailable. Try again in a moment.',true);}
  finally {if(run===generation)document.querySelector('#animation-grid').setAttribute('aria-busy','false');}
}
async function refresh() {
  try {
    catalog=validateCatalog(await json('catalog.json'));
    if(teaser){
      const shuffled=families(catalog).map(f=>({f,random:Math.random()})).sort((a,b)=>a.random-b.random).slice(0,3);
      teaser.innerHTML=collectionMarkup(shuffled.flatMap(({f})=>f.variants),options);watchImages();return;
    }
    const query=new URLSearchParams(location.search), familyId=config.familyId||query.get('set');
    if(familyId){
      const family=families(catalog).find(f=>f.id===familyId);
      if(!family){stopHeroRotation();document.querySelector('#animation-grid').replaceChildren();document.querySelector('#animation-variants').replaceChildren();notice('This set is currently unavailable. Explore the other animation sets.');return;}
      await detail(family,query.get('variant'),!config.familyId);
    } else {document.querySelector('#animation-grid').innerHTML=collectionMarkup(catalog,options);notice('');watchImages();}
  } catch {notice('The latest collection is temporarily unavailable. Showing the saved previews.',true);}
}
document.addEventListener('click',event=>{
  if(event.target.closest('.animation-retry')){refresh();return;}
  const link=event.target.closest('[data-variant]');
  if(!link||event.ctrlKey||event.metaKey||event.shiftKey||event.altKey)return;
  if(!catalog)return;
  const id=link.dataset.variant;if(!safeId(id))return;
  const family=families(catalog).find(f=>f.variants.some(v=>v.id===id));if(!family)return;
  event.preventDefault();history.pushState(null,'',link.href);detail(family,id);
});
window.addEventListener('popstate',()=>refresh());
watchImages();refresh();
