(() => {
  const controls = [...document.querySelectorAll('input[name="release-hardware"]')];
  const streams = [...document.querySelectorAll('.release-stream')];
  const targetIds = controls.map(control => control.value);
  function select(target, updateUrl = false, releaseId = '') {
    if (!targetIds.includes(target)) target = targetIds[0];
    for (const control of controls) {
      control.checked = control.value === target;
      control.closest('label').classList.toggle('selected', control.checked);
    }
    for (const stream of streams) stream.hidden = stream.id !== target;
    const stream = streams.find(stream => stream.id === target);
    const linkedRelease = document.getElementById(releaseId);
    const expandedRelease = linkedRelease?.matches('details.release-card') && stream?.contains(linkedRelease)
      ? linkedRelease
      : stream?.querySelector('details.release-card');
    for (const release of document.querySelectorAll('details.release-card')) {
      release.open = release === expandedRelease;
    }
    // Keep language changes on the same independent release stream.
    for (const option of document.querySelectorAll('[data-site-language] option')) {
      const link = new URL(option.value, location.origin);
      link.searchParams.set('target', target);
      option.value = link.pathname + link.search;
    }
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.set('target', target);
      url.hash = '';
      history.replaceState(null, '', url);
    }
  }
  function fromUrl() {
    const anchor = location.hash.slice(1);
    const target = targetIds.find(id => anchor === id || anchor.startsWith(`${id}-v`)) || new URL(location.href).searchParams.get('target');
    select(target, false, anchor);
    const entry = document.getElementById(anchor) || (anchor ? document.querySelector('.release-stream:not([hidden])') : null);
    if (entry) {
      // Wait for font layout so a direct link lands on the selected release.
      document.fonts.ready.then(() => requestAnimationFrame(() => {
        if (location.hash.slice(1) === anchor) entry.scrollIntoView({ block: 'start' });
      }));
    }
  }
  controls.forEach(control => control.addEventListener('change', () => select(control.value, true)));
  window.addEventListener('hashchange', fromUrl);
  window.addEventListener('popstate', fromUrl);
  fromUrl();
})();
