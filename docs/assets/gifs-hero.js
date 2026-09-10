(() => {
  const hero = document.querySelector('[data-gifs-hero]');
  if (!hero) return;

  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const screens = Array.from(hero.querySelectorAll('[data-gifs-animation]'), (image) => ({ image, poster: image.src }));
  let visible = false;
  let playing = false;
  let timers = [];

  const update = () => {
    const shouldPlay = visible && !document.hidden && !reducedMotion.matches;
    if (shouldPlay === playing) return;
    playing = shouldPlay;
    timers.forEach(window.clearTimeout);
    timers = [];
    hero.classList.toggle('is-paused', !playing);
    screens.forEach(({ image, poster }, index) => {
      image.src = poster;
      if (playing) {
        timers.push(window.setTimeout(() => {
          image.src = image.dataset.gifsAnimation;
        }, 150 + index * 550 + Math.random() * 500));
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
