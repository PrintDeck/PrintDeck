export const assetRoot = 'https://raw.githubusercontent.com/PrintDeck/PrintDeck/main/reaction-sets/';
export const safeId = value => typeof value === 'string' && /^[a-z0-9][a-z0-9_-]{0,63}$/.test(value);
export const escape = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
export const slug = id => id.replaceAll('_', '-');
export const eventNames = {'standby':'Standby','preparing':'Preparing','nozzle-heating':'Heating nozzle','bed-heating':'Heating bed','homing':'Homing toolhead','bed-leveling':'Leveling the bed','nozzle-cleaning':'Cleaning nozzle','calibrating':'Calibrating','filament-changing':'Changing filament','filament-unloading':'Unloading filament','filament-loading':'Loading filament','filament-purging':'Purging filament','printing':'Printing','paused':'Paused','completed':'Complete','failed':'Failed','cancelled':'Cancelled','unavailable':'Unavailable'};
export function validateCatalog(data) {
  if (data.schema !== 1 || !Array.isArray(data.sets) || data.sets.length > 128) throw Error('Invalid catalog');
  const seen = new Set();
  return data.sets.map(set => {
    if (!safeId(set.id) || !safeId(set.family_id || set.id) || seen.has(set.id) ||
        ![set.name, set.family_name || set.name, set.variant_name || '', set.version].every(v => typeof v === 'string' && v.length <= 100) ||
        set.preview !== `previews/${set.id}.webp`) throw Error('Invalid set');
    seen.add(set.id); return set;
  });
}
export function families(sets) {
  const result = new Map();
  for (const set of sets) { const id = set.family_id || set.id; if (!result.has(id)) result.set(id, []); result.get(id).push(set); }
  return [...result].map(([id, variants]) => ({id, name:variants[0].family_name || variants[0].name, variants}));
}
export function validateManifest(data, id) {
  if (data.set !== id || !data.files || Array.isArray(data.files) || Object.keys(data.files).length > 64) throw Error('Invalid manifest');
  for (const [state, file] of Object.entries(data.files)) {
    if (!safeId(state) || file.file !== `${state}.gif` || !/^[a-f0-9]{64}$/.test(file.sha256)) throw Error('Invalid animation');
  }
  return data;
}
const assetUrl = (path, version) => `${assetRoot}${path}?v=${encodeURIComponent(version)}`;
export function screen(src, t, hero = false) {
  return `<span class="animation-device"><span class="animation-display"><img src="${escape(src)}" alt="" width="466" height="466" loading="${hero ? 'eager':'lazy'}" decoding="async" referrerpolicy="no-referrer"><span class="animation-image-error" hidden>${escape(t('Preview unavailable'))}</span></span><span class="animation-device-mark" aria-hidden="true">PRINTDECK</span></span>`;
}
export function collectionMarkup(sets, {t, base, known}) {
  return families(sets).map((family, index) => {
    const set = family.variants[0];
    const href = known.includes(family.id) ? `${base}${slug(family.id)}/` : `${base}?set=${encodeURIComponent(family.id)}`;
    return `<a class="animation-card" href="${escape(href)}"> <span class="animation-card-stage tone-${index%3}">${screen(assetUrl(set.preview,set.version),t)}</span><span class="animation-card-copy"><strong translate="no">${escape(family.name)}</strong><span>${escape(t('View reactions'))} <span aria-hidden="true">↗</span></span>${family.variants.length>1?`<small><span>${escape(t('Color variants'))}</span> <span translate="no">· ${family.variants.length}</span></small>`:''}</span></a>`;
  }).join('');
}
export function reactionsMarkup(set, manifest, t) {
  return Object.entries(manifest.files).map(([state, file], index) => `<figure class="animation-reaction"><div class="animation-reaction-stage">${screen(assetUrl(`466x466/sets/${set.id}/${file.file}`,file.sha256),t)}</div><figcaption><span class="animation-number" aria-hidden="true" translate="no">${String(index+1).padStart(2,'0')}</span><strong>${escape(t(eventNames[state] || state.replaceAll('-',' ')))}</strong></figcaption></figure>`).join('');
}
export function variantsMarkup(family, selected, {t,base,known}) {
  if (family.variants.length < 2) return '';
  const path = known.includes(family.id) ? `${base}${slug(family.id)}/` : `${base}?set=${encodeURIComponent(family.id)}`;
  return `<div class="animation-variants"><strong>${escape(t('Choose a color'))}</strong><div>${family.variants.map(v=>`<a class="animation-variant" data-variant="${escape(v.id)}" href="${path}${path.includes('?')?'&':'?'}variant=${encodeURIComponent(v.id)}" ${v.id===selected?'aria-current="true"':''}>${escape(t(v.variant_name||v.name))}</a>`).join('')}</div></div>`;
}
export function pageMarkup(sets, manifests, familyId, options) {
  const {t,base} = options, groups = families(sets), family = groups.find(f=>f.id===familyId), set = family?.variants.find(v=>v.id===options.selected) || family?.variants[0];
  const featured = set || groups.find(f=>f.id==='timmy')?.variants[0] || sets[0];
  const hero = featured ? screen(assetUrl(featured.preview,featured.version),t,true) : '';
  return `<section class="animation-hero"><div class="reactions-inner"><div class="animation-hero-copy"><a class="animation-breadcrumb" href="${escape(family?base:base.replace('animation-sets/',''))}"><span translate="no">←</span> <span>${escape(t(family?'All animation sets':'GIFs'))}</span></a><span class="reactions-eyebrow">${escape(t('Animation sets'))}</span><h1>${family?`<span translate="no">${escape(family.name)}</span>`:`${escape(t('A little character.'))}<br><strong>${escape(t('For every print.'))}</strong>`}</h1><p class="reactions-lead">${escape(t(family?'See how this set responds to each printer status. Choose it in Web Config or in the PrintDeck Cloud device panel.':'Pick a personality for your PrintDeck. Explore complete animation sets, from the first warm-up to the final celebration.'))}</p><div class="site-actions"><a class="site-button" href="#animation-gallery">${escape(t(family?'View reactions':'Browse animation sets'))} <span aria-hidden="true" translate="no">↓</span></a><a class="site-button secondary animation-back" href="${escape(family?base:base.replace('animation-sets/',''))}"><span aria-hidden="true" translate="no">←</span> ${escape(t(family?'Back to animation sets':'Back to GIFs'))}</a></div></div><div class="animation-hero-art" aria-hidden="true">${hero}</div></div></section>
  <section class="reactions-section animation-collection" id="animation-gallery"><div class="reactions-inner"><header class="animation-section-head"><div><span class="reactions-kicker">${escape(t(family?'Printer reactions':'Animation sets'))}</span><h2>${escape(t(family?'One style. Every reaction.':'Find your favourite.'))}</h2></div></header><p class="animation-notice" role="status" hidden></p><button class="animation-retry" type="button" hidden>${escape(t('Retry'))}</button><div id="animation-variants">${family?variantsMarkup(family,set.id,options):''}</div><div id="animation-grid" class="${family?'animation-reaction-grid':'animation-set-grid'}">${family?reactionsMarkup(set,manifests[set.id],t):collectionMarkup(sets,options)}</div></div></section>
  <section class="animation-install"><div class="reactions-inner"><span class="reactions-kicker">PrintDeck</span><h2>${escape(t('Ready for your PrintDeck.'))}</h2><p>${escape(t('Open Web Config or the PrintDeck Cloud device panel, go to Reactions and choose your animation set. You can still add your own GIFs to individual reactions.'))}</p><a class="site-button" href="${escape(base.replace('animation-sets/',''))}">${escape(t('Make it your own'))} <span aria-hidden="true">↗</span></a></div></section>`;
}
