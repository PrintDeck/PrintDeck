(() => {
  const storageKey = 'printdeck-installer-appearance';
  const systemDark = window.matchMedia('(prefers-color-scheme: dark)');
  const allowedModes = new Set(['light', 'dark']);
  let followsSystem = true;
  let mode = systemDark.matches ? 'dark' : 'light';

  try {
    const stored = localStorage.getItem(storageKey);
    if (allowedModes.has(stored)) {
      mode = stored;
      followsSystem = false;
    }
  } catch (_) {}

  const updateControls = () => {
    document.querySelectorAll('[data-site-appearance], .appearance-switch').forEach((control) => {
      control.setAttribute('aria-checked', String(mode === 'dark'));
      control.querySelectorAll('[data-appearance-mode]').forEach((option) => {
        option.setAttribute('aria-pressed', String(option.dataset.appearanceMode === mode));
      });
    });
  };

  const apply = (nextMode, persist = true) => {
    mode = nextMode === 'light' || nextMode === 'dark' ? nextMode : (systemDark.matches ? 'dark' : 'light');
    document.documentElement.dataset.appearance = mode;
    const themeColor = document.querySelector('meta[name="theme-color"]');
    if (themeColor) themeColor.content = mode === 'dark' ? '#101010' : '#f5f5f6';
    if (persist) {
      followsSystem = false;
      try { localStorage.setItem(storageKey, mode); } catch (_) {}
    }
    updateControls();
  };

  const bind = () => {
    document.querySelectorAll('[data-site-appearance], .appearance-switch').forEach((control) => {
      if (control.dataset.bound) return;
      control.dataset.bound = 'true';
      const toggle = () => apply(mode === 'dark' ? 'light' : 'dark');
      control.addEventListener('click', toggle);
      control.addEventListener('keydown', (event) => {
        if (event.key !== 'Enter' && event.key !== ' ') return;
        event.preventDefault();
        toggle();
      });
    });
    updateControls();
  };

  const followSystem = () => { if (followsSystem) apply(systemDark.matches ? 'dark' : 'light', false); };
  if (systemDark.addEventListener) systemDark.addEventListener('change', followSystem);
  else systemDark.addListener(followSystem);

  const syncStoredAppearance = (storedMode) => {
    if (allowedModes.has(storedMode)) {
      followsSystem = false;
      apply(storedMode, false);
      return;
    }
    followsSystem = true;
    apply(systemDark.matches ? 'dark' : 'light', false);
  };

  window.addEventListener('storage', (event) => {
    if (event.key !== storageKey) return;
    syncStoredAppearance(event.newValue);
  });

  window.addEventListener('pageshow', () => {
    let storedMode = null;
    try { storedMode = localStorage.getItem(storageKey); } catch (_) {}
    syncStoredAppearance(storedMode);
  });

  apply(mode, false);
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bind);
  else bind();
})();

(() => {
  const bindCaseStories = () => {
    document.querySelectorAll('[data-case-story]').forEach((showcase) => {
      if (showcase.dataset.storyBound) return;
      showcase.dataset.storyBound = 'true';

      const overview = showcase.querySelector('[data-case-overview]');
      const story = showcase.querySelector('[data-case-story-panel]');
      const open = showcase.querySelector('[data-case-story-open]');
      const close = showcase.querySelector('[data-case-story-close]');
      if (!overview || !story || !open || !close) return;

      const showPanel = (showStory) => {
        overview.hidden = showStory;
        story.hidden = !showStory;
        open.setAttribute('aria-expanded', String(showStory));
        showcase.classList.toggle('is-story-visible', showStory);

        const visible = showStory ? story : overview;
        visible.classList.remove('is-entering');
        requestAnimationFrame(() => visible.classList.add('is-entering'));

        const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
        showcase.scrollIntoView({ behavior: reducedMotion ? 'auto' : 'smooth', block: 'start' });
        if (showStory) story.focus({ preventScroll: true });
        else open.focus({ preventScroll: true });
      };

      open.addEventListener('click', () => showPanel(true));
      close.addEventListener('click', () => showPanel(false));
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindCaseStories);
  else bindCaseStories();
})();

(() => {
  const bindCaseJumps = () => {
    document.querySelectorAll('[data-case-jump]').forEach((link) => {
      if (link.dataset.caseJumpBound) return;
      link.dataset.caseJumpBound = 'true';
      link.addEventListener('click', (event) => {
        const href = link.getAttribute('href');
        if (!href || !href.startsWith('#')) return;

        const target = document.getElementById(href.slice(1));
        if (!target) return;

        event.preventDefault();
        const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
        target.scrollIntoView({ behavior: reducedMotion ? 'auto' : 'smooth', block: 'start' });
        if (window.location.hash !== href) history.pushState(null, '', href);
      });
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindCaseJumps);
  else bindCaseJumps();
})();

(() => {
  const bindHeroScreenRotation = () => {
    const product = document.querySelector('[data-hero-product]');
    const device = product?.querySelector('[data-hero-device]');
    const floorShadow = product?.querySelector('.home-hero-floor-shadow');
    const screenLayers = product ? Array.from(product.querySelectorAll('[data-hero-screen]')) : [];
    if (!product || !device || screenLayers.length < 2) return;

    const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
    let currentScreen = 0;
    let timer = 0;
    let isVisible = true;
    let isAnimating = false;

    const showScreen = (screenIndex) => {
      screenLayers.forEach((screen) => {
        screen.classList.toggle('is-active', Number(screen.dataset.heroScreen) === screenIndex);
      });
      currentScreen = screenIndex;
    };

    const scheduleNext = () => {
      window.clearTimeout(timer);
      if (!isVisible || document.hidden) return;
      timer = window.setTimeout(changeScreen, 3000);
    };

    const fadeToNextScreen = (nextScreen) => {
      product.classList.add('is-screen-fading');
      window.setTimeout(() => showScreen(nextScreen), 150);
      window.setTimeout(() => {
        product.classList.remove('is-screen-fading');
        isAnimating = false;
        scheduleNext();
      }, 310);
    };

    const changeScreen = async () => {
      if (isAnimating || !isVisible || document.hidden) return;
      isAnimating = true;
      const nextScreen = (currentScreen + 1) % screenLayers.length;

      if (reducedMotion.matches || typeof device.animate !== 'function') {
        fadeToNextScreen(nextScreen);
        return;
      }

      const turnOut = device.animate([
        {
          transform: 'rotateX(0deg) rotateY(0deg) rotateZ(0deg) scale(1)',
          filter: 'drop-shadow(0 30px 24px #0008) blur(0)'
        },
        {
          transform: 'rotateX(-7deg) rotateY(88deg) rotateZ(180deg) scale(.92)',
          filter: 'drop-shadow(-20px 34px 18px #0009) blur(1.1px)'
        }
      ], {
        duration: 360,
        easing: 'cubic-bezier(.55,.05,.75,.36)',
        fill: 'forwards'
      });

      const shadowOut = floorShadow?.animate([
        { transform: 'translateY(38%) scaleX(1)', opacity: .74, filter: 'blur(20px)' },
        { transform: 'translateY(54%) scaleX(.34)', opacity: .4, filter: 'blur(12px)' }
      ], { duration: 360, easing: 'ease-in', fill: 'forwards' });

      try {
        await turnOut.finished;
        showScreen(nextScreen);

        const turnIn = device.animate([
          {
            transform: 'rotateX(7deg) rotateY(-88deg) rotateZ(180deg) scale(.92)',
            filter: 'drop-shadow(20px 34px 18px #0009) blur(1.1px)'
          },
          {
            offset: .78,
            transform: 'rotateX(-1.5deg) rotateY(5deg) rotateZ(348deg) scale(1.012)',
            filter: 'drop-shadow(1px 32px 25px #0008) blur(0)'
          },
          {
            transform: 'rotateX(0deg) rotateY(0deg) rotateZ(360deg) scale(1)',
            filter: 'drop-shadow(0 30px 24px #0008) blur(0)'
          }
        ], {
          duration: 470,
          easing: 'cubic-bezier(.18,.78,.2,1)',
          fill: 'forwards'
        });

        const shadowIn = floorShadow?.animate([
          { transform: 'translateY(54%) scaleX(.34)', opacity: .4, filter: 'blur(12px)' },
          { transform: 'translateY(38%) scaleX(1.04)', opacity: .78, filter: 'blur(21px)', offset: .78 },
          { transform: 'translateY(38%) scaleX(1)', opacity: .74, filter: 'blur(20px)' }
        ], { duration: 470, easing: 'cubic-bezier(.18,.78,.2,1)', fill: 'forwards' });

        await turnIn.finished;
        turnOut.cancel();
        turnIn.cancel();
        shadowOut?.cancel();
        shadowIn?.cancel();
      } catch (_) {
        showScreen(nextScreen);
      }

      isAnimating = false;
      scheduleNext();
    };

    document.addEventListener('visibilitychange', scheduleNext);

    if ('IntersectionObserver' in window) {
      const observer = new IntersectionObserver(([entry]) => {
        isVisible = entry.isIntersecting;
        scheduleNext();
      }, { threshold: .15 });
      observer.observe(product);
    }

    scheduleNext();
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindHeroScreenRotation);
  else bindHeroScreenRotation();
})();

(() => {
  const bindScreenCarousels = () => {
    const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');

    document.querySelectorAll('[data-screen-carousel]').forEach((carousel) => {
      const track = carousel.querySelector('[data-screen-carousel-track]');
      const slides = track ? Array.from(track.children) : [];
      if (!track || slides.length < 2) return;

      const slideCount = slides.length;
      const firstClone = slides[0].cloneNode(true);
      firstClone.setAttribute('aria-hidden', 'true');
      track.append(firstClone);

      let current = 0;
      let timer = 0;
      let isVisible = true;

      const moveTrack = () => {
        track.style.transform = `translate3d(-${current * 100}%, 0, 0)`;
      };

      const resetToFirst = () => {
        current = 0;
        track.classList.add('is-resetting');
        moveTrack();
        window.requestAnimationFrame(() => {
          window.requestAnimationFrame(() => track.classList.remove('is-resetting'));
        });
      };

      const scheduleNext = () => {
        window.clearTimeout(timer);
        if (!isVisible || document.hidden) return;
        timer = window.setTimeout(advance, 3000);
      };

      const advance = () => {
        if (!isVisible || document.hidden) return;
        current += 1;
        moveTrack();

        if (current === slideCount) {
          if (reducedMotion.matches) resetToFirst();
          else window.setTimeout(resetToFirst, 580);
        }

        scheduleNext();
      };

      document.addEventListener('visibilitychange', scheduleNext);

      if ('IntersectionObserver' in window) {
        const observer = new IntersectionObserver(([entry]) => {
          isVisible = entry.isIntersecting;
          scheduleNext();
        }, { threshold: .2 });
        observer.observe(carousel);
      }

      scheduleNext();
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindScreenCarousels);
  else bindScreenCarousels();
})();

(() => {
  const bindHomeVideos = () => {
    const mediaIds = Array.from(document.querySelectorAll('[data-home-video]'), (media) => media.dataset.homeVideo);
    if (typeof window.Plyr !== 'function' || !mediaIds.length) return;

    const players = new Map();
    const viewportStates = new Map(mediaIds.map((mediaId) => [mediaId, {
      isIntersecting: false,
      hasStartedInView: false,
      resumeWhenDocumentVisible: false,
      waitingForPlayer: false
    }]));

    const translatedLabels = () => {
      const t = window.printDeckInstallerI18n?.t || ((value) => value);
      return {
        play: t('Play'),
        pause: t('Pause'),
        seek: t('Seek'),
        seekLabel: t('{currentTime} of {duration}'),
        played: t('Played'),
        buffered: t('Buffered'),
        currentTime: t('Current time'),
        volume: t('Volume'),
        mute: t('Mute'),
        unmute: t('Unmute'),
        enterFullscreen: t('Enter fullscreen'),
        exitFullscreen: t('Exit fullscreen')
      };
    };

    const createPlayer = (mediaId, state = {}) => {
      const media = document.querySelector(`[data-home-video="${mediaId}"]`);
      if (!media) return null;

      const t = window.printDeckInstallerI18n?.t || ((value) => value);
      media.autoplay = false;
      media.muted = typeof state.muted === 'boolean' ? state.muted : true;
      media.setAttribute('aria-label', t(media.dataset.videoLabel));
      const player = new window.Plyr(media, {
        autoplay: false,
        controls: ['play-large', 'play', 'progress', 'current-time', 'mute', 'volume', 'fullscreen'],
        i18n: translatedLabels(),
        keyboard: { focused: true, global: false },
        resetOnEnd: true
      });

      if (Object.keys(state).length) {
        const restoreState = () => {
          if (Number.isFinite(state.currentTime) && state.currentTime > 0) player.currentTime = state.currentTime;
          if (Number.isFinite(state.volume)) player.volume = state.volume;
          if (typeof state.muted === 'boolean') player.muted = state.muted;
          if (state.paused === false) player.play().catch(() => {});
        };
        if (player.media.readyState >= 1) restoreState();
        else player.once('loadedmetadata', restoreState);
      }

      return player;
    };

    mediaIds.forEach((mediaId) => players.set(mediaId, createPlayer(mediaId)));

    const playVisibleVideo = (mediaId) => {
      const player = players.get(mediaId);
      const viewportState = viewportStates.get(mediaId);
      if (!player || !viewportState || !viewportState.isIntersecting || document.hidden) return;

      if (!player.ready) {
        if (viewportState.waitingForPlayer) return;
        viewportState.waitingForPlayer = true;
        player.once('ready', () => {
          viewportState.waitingForPlayer = false;
          playVisibleVideo(mediaId);
        });
        return;
      }

      const playback = player.play();
      if (playback?.catch) {
        playback.catch(() => {
          if (!viewportState.isIntersecting || document.hidden || player.muted) return;
          player.muted = true;
          player.play().catch(() => {});
        });
      }
    };

    const updateVideoVisibility = (mediaId, isIntersecting, intersectionRatio) => {
      const player = players.get(mediaId);
      const viewportState = viewportStates.get(mediaId);
      if (!player || !viewportState) return;

      viewportState.isIntersecting = isIntersecting;

      if (!isIntersecting) {
        viewportState.hasStartedInView = false;
        viewportState.resumeWhenDocumentVisible = false;
        player.pause();
        return;
      }

      if (viewportState.hasStartedInView || intersectionRatio < .25) return;
      viewportState.hasStartedInView = true;
      viewportState.resumeWhenDocumentVisible = document.hidden;
      playVisibleVideo(mediaId);
    };

    const observedElements = new Map();
    mediaIds.forEach((mediaId) => {
      const media = document.querySelector(`[data-home-video="${mediaId}"]`);
      const viewport = media?.closest('.home-section-video') || media;
      if (viewport) observedElements.set(viewport, mediaId);
    });

    if ('IntersectionObserver' in window) {
      const observer = new IntersectionObserver((entries) => {
        entries.forEach((entry) => {
          const mediaId = observedElements.get(entry.target);
          if (mediaId) updateVideoVisibility(mediaId, entry.isIntersecting, entry.intersectionRatio);
        });
      }, { threshold: [0, .25] });
      observedElements.forEach((_, element) => observer.observe(element));
    } else {
      let fallbackFrame = 0;
      const updateFallbackVisibility = () => {
        fallbackFrame = 0;
        observedElements.forEach((mediaId, element) => {
          const rect = element.getBoundingClientRect();
          const visibleHeight = Math.max(0, Math.min(rect.bottom, window.innerHeight) - Math.max(rect.top, 0));
          const ratio = rect.height > 0 ? visibleHeight / rect.height : 0;
          updateVideoVisibility(mediaId, ratio > 0, ratio);
        });
      };
      const scheduleFallbackVisibility = () => {
        if (!fallbackFrame) fallbackFrame = window.requestAnimationFrame(updateFallbackVisibility);
      };
      window.addEventListener('scroll', scheduleFallbackVisibility, { passive: true });
      window.addEventListener('resize', scheduleFallbackVisibility);
      scheduleFallbackVisibility();
    }

    document.addEventListener('visibilitychange', () => {
      viewportStates.forEach((viewportState, mediaId) => {
        const player = players.get(mediaId);
        if (!player) return;

        if (document.hidden) {
          viewportState.resumeWhenDocumentVisible = viewportState.isIntersecting
            && (!player.paused || viewportState.waitingForPlayer);
          player.pause();
          return;
        }

        if (viewportState.resumeWhenDocumentVisible) playVisibleVideo(mediaId);
        viewportState.resumeWhenDocumentVisible = false;
      });
    });

    window.addEventListener('printdeck-installer-language-changed', () => {
      players.forEach((player, mediaId) => {
        if (!player?.ready) return;
        const state = {
          currentTime: player.currentTime,
          paused: player.paused,
          volume: player.volume,
          muted: player.muted
        };
        player.destroy(() => players.set(mediaId, createPlayer(mediaId, state)));
      });
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindHomeVideos);
  else bindHomeVideos();
})();

(() => {
  const bindApiExplorer = () => {
    const explorer = document.querySelector('[data-api-explorer]');
    const example = document.querySelector('[data-api-example]');
    if (!explorer || !example || explorer.dataset.apiBound) return;

    const pathLabel = example.querySelector('[data-api-example-path]');
    const requestCode = example.querySelector('[data-api-request] code');
    const responseCode = example.querySelector('[data-api-response] code');
    const requestPanel = example.querySelector('[data-api-request]');
    const responsePanel = example.querySelector('[data-api-response]');
    const endpoints = Array.from(explorer.querySelectorAll('[data-api-endpoint]'));
    if (!pathLabel || !requestCode || !responseCode || !endpoints.length) return;

    explorer.dataset.apiBound = 'true';

    const printer = {
      id: 1,
      name: 'Workshop Voron',
      protocol: 'moonraker',
      manufacturer: 'Voron',
      model: '2.4',
      endpoint: '192.168.1.50:7125',
      selected: true,
      reachability: 'online',
      capabilities: { status: true, nozzles: true, materials: true }
    };
    const connection = {
      state: 'online',
      reachability: 'online',
      detail_level: 'full',
      stale: false,
      updated_at_ms: 418520
    };
    const job = {
      phase: 'printing',
      kind: 'print',
      activity: 'printing',
      name: 'Bracket',
      progress_percent: 42.5,
      elapsed_seconds: 1260,
      remaining_seconds: 1740,
      current_layer: 48,
      total_layers: 112
    };

    const responses = {
      '/v1/info': {
        api_version: 'v1',
        product: 'PrintDeck',
        firmware_version: '1.0.3',
        hardware: 'amoled_1_75',
        network: {
          wifi_name: 'Workshop Wi-Fi',
          ipv4: '192.168.1.42',
          hostname: 'printdeck.local'
        },
        read_only: true
      },
      '/v1/printers': { api_version: 'v1', printers: [printer] },
      '/v1/printers/status': {
        api_version: 'v1',
        statuses: [{ printer_id: 1, connection, job }]
      },
      '/v1/printers/{id}': { api_version: 'v1', printer },
      '/v1/printers/{id}/status': {
        api_version: 'v1',
        status: {
          printer_id: 1,
          connection,
          job,
          temperatures: {
            nozzle_current_c: 200.4,
            nozzle_target_c: 200,
            bed_current_c: 59.8,
            bed_target_c: 60,
            chamber_current_c: null
          }
        }
      },
      '/v1/printers/{id}/nozzles': {
        api_version: 'v1',
        printer_id: 1,
        detail_level: 'full',
        stale: false,
        updated_at_ms: 418520,
        nozzles: [{
          id: 'T0',
          active: true,
          state: 'ready',
          diameter_mm: 0.4,
          temperature: { current_c: 133, target_c: 200 },
          material: { type: 'PLA', color: '#111111FF' },
          filament_detected: true
        }]
      },
      '/v1/printers/{id}/materials': {
        api_version: 'v1',
        printer_id: 1,
        available: true,
        detail_level: 'full',
        stale: false,
        updated_at_ms: 418520,
        system: 'ams_or_ams_lite',
        slots: [{
          id: 'slot-0',
          installed: true,
          feeding: true,
          material: 'PLA',
          color: '#E6B422FF',
          remaining_percent: 72,
          source_unit: 0,
          source_slot: 0
        }],
        external_spools: []
      }
    };

    const selectEndpoint = (endpoint, animate = true) => {
      const path = endpoint.dataset.apiEndpoint;
      const response = responses[path];
      if (!response) return;

      endpoints.forEach((item) => item.setAttribute('aria-pressed', String(item === endpoint)));
      const requestPath = path.replace('{id}', '1');
      pathLabel.textContent = `GET ${path}`;
      requestCode.textContent = `curl http://PRINTDECK-IP${requestPath} \\\n  -H 'Authorization: Bearer pd_your_token_here'`;
      responseCode.textContent = JSON.stringify(response, null, 2);
      if (requestPanel) requestPanel.scrollLeft = 0;
      if (responsePanel) {
        responsePanel.scrollLeft = 0;
        responsePanel.scrollTop = 0;
      }

      if (!animate) return;
      example.classList.remove('is-updating');
      void example.offsetWidth;
      example.classList.add('is-updating');
    };

    endpoints.forEach((endpoint, index) => {
      endpoint.addEventListener('click', () => selectEndpoint(endpoint));
      endpoint.addEventListener('keydown', (event) => {
        const last = endpoints.length - 1;
        const nextIndex = event.key === 'ArrowDown' ? Math.min(index + 1, last)
          : event.key === 'ArrowUp' ? Math.max(index - 1, 0)
            : event.key === 'Home' ? 0
              : event.key === 'End' ? last
                : -1;
        if (nextIndex < 0) return;
        event.preventDefault();
        endpoints[nextIndex].focus();
        selectEndpoint(endpoints[nextIndex]);
      });
    });

    window.addEventListener('printdeck-installer-language-changed', () => {
      const selected = endpoints.find((endpoint) => endpoint.getAttribute('aria-pressed') === 'true') || endpoints[0];
      selectEndpoint(selected, false);
    });

    const selected = endpoints.find((endpoint) => endpoint.getAttribute('aria-pressed') === 'true') || endpoints[0];
    selectEndpoint(selected, false);
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindApiExplorer);
  else bindApiExplorer();
})();

(() => {
  const bindExampleDialogs = () => {
    document.querySelectorAll('[data-example-dialog]').forEach((button) => {
      if (button.dataset.exampleBound) return;
      button.dataset.exampleBound = 'true';
      const dialog = document.getElementById(button.dataset.exampleDialog);
      if (!(dialog instanceof HTMLDialogElement)) return;
      button.addEventListener('click', () => dialog.showModal());
    });

    document.querySelectorAll('.ha-example-dialog').forEach((dialog) => {
      if (dialog.dataset.dialogBound) return;
      dialog.dataset.dialogBound = 'true';
      dialog.querySelector('.ha-example-close')?.addEventListener('click', () => dialog.close());
      dialog.addEventListener('click', (event) => {
        if (event.target === dialog) dialog.close();
      });
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', bindExampleDialogs);
  else bindExampleDialogs();
})();
