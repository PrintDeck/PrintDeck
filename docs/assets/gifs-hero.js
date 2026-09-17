(() => {
  const hero = document.querySelector('[data-gifs-hero]');
  if (!hero) return;

  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const screens = Array.from(hero.querySelectorAll('[data-gifs-animation]'), (image) => ({
    image,
    poster: image.src,
    animations: (image.dataset.gifsAnimations || image.dataset.gifsAnimation).trim().split(/\s+/),
    position: 0,
    timer: null,
  }));
  let visible = false;
  let playing = false;
  let generation = 0;

  const showNext = (screen, activeGeneration) => {
    if (!playing || generation !== activeGeneration) return;
    const src = screen.animations[screen.position];
    const next = new Image();
    const finished = (loaded) => {
      if (!playing || generation !== activeGeneration) return;
      // Load the next GIF before replacing the screen, retaining its fixed
      // projection and current frame if an asset is temporarily unavailable.
      if (loaded) screen.image.src = src;
      screen.position = (screen.position + 1) % screen.animations.length;
      if (screen.animations.length > 1) {
        screen.timer = window.setTimeout(() => showNext(screen, activeGeneration), 5000);
      }
    };
    next.onload = () => finished(true);
    next.onerror = () => finished(false);
    next.src = src;
  };

  const update = () => {
    const shouldPlay = visible && !document.hidden && !reducedMotion.matches;
    if (shouldPlay === playing) return;
    playing = shouldPlay;
    const activeGeneration = ++generation;
    hero.classList.toggle('is-paused', !playing);
    screens.forEach((screen, index) => {
      const { image, poster } = screen;
      window.clearTimeout(screen.timer);
      screen.timer = null;
      image.src = poster;
      if (playing) {
        screen.timer = window.setTimeout(() => showNext(screen, activeGeneration),
          150 + index * 550 + Math.random() * 500);
      }
    });
  };

  reducedMotion.addEventListener('change', update);
  document.addEventListener('visibilitychange', update);
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([entry]) => {
      visible = entry.isIntersecting;
      update();
    }, { threshold: .1 }).observe(hero);
  } else {
    visible = true;
    update();
  }
})();
