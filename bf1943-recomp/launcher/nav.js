// Keyboard and controller navigation, as in the game's own menus, and the
// pages' animations (typed headings, sheets sliding in).
//
//   arrows / D-pad / left stick   move the focus (on a row of choices, left and
//                                 right change the value, as < > in the game)
//   Enter / A                     press
//   Esc / B                       back: to the side menu, then to BATTLE
//   Q E / LB RB                   the previous / next page
//   LT RT                         the previous / next game
//   right stick                   scrolls the page
//
// The mouse keeps working as before; the prompts in the side menu show the
// keys, or the pad's buttons once a pad is used (body.pad).
(() => {
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)');
  const pages = () => [...document.querySelectorAll('#nav > button[data-page]')];
  const activePage = () => document.querySelector('.page.active');

  // ---- animations ---------------------------------------------------------------
  function typeOut(el) {
    if (reduced.matches) return;
    el.style.setProperty('--n', Math.max(1, el.textContent.trim().length));
    el.classList.remove('typing');
    void el.offsetWidth;  // (restarts the animation)
    el.classList.add('typing');
  }
  function animatePage(page) {
    if (!page) return;
    let i = 0;
    for (const el of page.querySelectorAll('.memo, .card, .play-row, .srv-row')) el.style.setProperty('--i', i++);
    page.querySelectorAll('.type').forEach(typeOut);
  }
  for (const b of pages()) b.addEventListener('click', () => animatePage(activePage()));
  // Another game: its title typed out again.
  new MutationObserver(() => typeOut(document.getElementById('year')))
    .observe(document.body, { attributes: true, attributeFilter: ['data-game'] });
  animatePage(activePage());

  // ---- focus ----------------------------------------------------------------------
  const focusable = 'button, input, [tabindex]:not([tabindex="-1"])';
  const visible = (el) => !el.disabled && el.getClientRects().length > 0 && el.closest('.page, #nav, .gametabs');
  function candidates() {
    const page = activePage();
    return [...document.querySelectorAll(focusable)].filter((el) => visible(el)
      && (!el.closest('.page') || el.closest('.page') === page) && el.tabIndex >= 0);
  }
  function focusEl(el) {
    if (!el) return;
    el.focus({ preventScroll: true });
    el.scrollIntoView({ block: 'nearest', behavior: reduced.matches ? 'auto' : 'smooth' });
  }
  // The nearest control in a direction: along it first, then across it.
  function move(dir) {
    const cur = document.activeElement;
    const list = candidates();
    if (!cur || cur === document.body || !list.includes(cur)) return focusEl(pageStart() || list[0]);
    const r = cur.getBoundingClientRect();
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    let best = null, bestScore = Infinity;
    for (const el of list) {
      if (el === cur) continue;
      const c = el.getBoundingClientRect();
      const ex = c.left + c.width / 2, ey = c.top + c.height / 2;
      let along, across;
      if (dir === 'up') { if (c.bottom > r.top + 4 && ey >= cy - 2) continue; along = r.top - c.bottom; across = Math.max(0, Math.abs(ex - cx) - (r.width + c.width) / 2); }
      else if (dir === 'down') { if (c.top < r.bottom - 4 && ey <= cy + 2) continue; along = c.top - r.bottom; across = Math.max(0, Math.abs(ex - cx) - (r.width + c.width) / 2); }
      else if (dir === 'left') { if (c.right > r.left + 4 && ex >= cx - 2) continue; along = r.left - c.right; across = Math.max(0, Math.abs(ey - cy) - (r.height + c.height) / 2); }
      else { if (c.left < r.right - 4 && ex <= cx + 2) continue; along = c.left - r.right; across = Math.max(0, Math.abs(ey - cy) - (r.height + c.height) / 2); }
      const score = Math.max(0, along) + across * 3 + (dir === 'up' || dir === 'down' ? Math.abs(ex - cx) * 0.05 : 0);
      if (score < bestScore) best = el, bestScore = score;
    }
    focusEl(best);
  }
  const pageStart = () => candidates().find((el) => el.closest('.page'));

  // A row of choices (graphics, effects): left and right pick the one beside.
  // The effects rows are built again on a click: the focus follows to the new one.
  function stepChoice(dir) {
    const b = document.activeElement;
    const seg = b && b.closest && b.closest('.seg');
    if (!seg || b.tagName !== 'BUTTON') return false;
    if (seg.classList.contains('stepper')) {  // bots, minutes: - and +
      const btn = seg.querySelectorAll('button')[dir < 0 ? 0 : 1];
      if (btn && !btn.disabled) { btn.click(); press(btn); }
      return true;
    }
    const opts = [...seg.querySelectorAll('button')];
    const to = opts[opts.indexOf(b) + dir];
    if (!to) return true;  // (the end of the row: stays)
    const list = seg.closest('#rows, #fxrows');
    const rowIndex = list ? [...list.children].indexOf(seg.closest('.row')) : -1;
    const optIndex = opts.indexOf(to);
    to.click();
    if (document.contains(to)) focusEl(to);
    else if (list && list.children[rowIndex]) focusEl(list.children[rowIndex].querySelectorAll('.seg button')[optIndex]);
    return true;
  }

  function press(el) {
    if (reduced.matches || !el.classList) return;
    el.classList.remove('pressed');
    void el.offsetWidth;
    el.classList.add('pressed');
    setTimeout(() => el.classList.remove('pressed'), 250);
  }
  function activate() {
    const el = document.activeElement;
    if (!el || el === document.body) return focusEl(pageStart());
    if (el.tagName === 'INPUT' && el.type === 'text') return;  // (typing)
    if (el.disabled) return;
    press(el);
    el.click();
  }
  function back() {
    const el = document.activeElement;
    const navOn = document.querySelector('#nav > button.active');
    if (el && el.closest && el.closest('.page')) focusEl(navOn);
    else if (navOn && navOn.dataset.page !== 'home') { pages()[0].click(); focusEl(pages()[0]); }
    else if (el && el.blur) el.blur();
  }
  function cyclePage(d) {
    const list = pages();
    const i = list.findIndex((b) => b.classList.contains('active'));
    const to = list[(i + d + list.length) % list.length];
    to.click();
    focusEl(to);
  }
  function cycleGame(d) {
    const tabs = [...document.querySelectorAll('.gametabs .game')].filter((t) => !t.disabled);
    const i = tabs.findIndex((t) => t.classList.contains('on'));
    const to = tabs[(i + d + tabs.length) % tabs.length];
    if (to) to.click();
  }

  // ---- keyboard -------------------------------------------------------------------
  const typing = (el) => el && el.tagName === 'INPUT' && el.type === 'text';
  document.addEventListener('keydown', (e) => {
    if (e.altKey || e.metaKey) return;
    const el = document.activeElement;
    const keyNav = () => { document.body.classList.add('kbnav'); document.body.classList.remove('pad'); };
    const dirs = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    if (dirs[e.key]) {
      // In a text field left and right move the caret; up and down leave it.
      if (typing(el) && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) return;
      e.preventDefault();
      keyNav();
      if ((e.key === 'ArrowLeft' || e.key === 'ArrowRight') && stepChoice(e.key === 'ArrowLeft' ? -1 : 1)) return;
      move(dirs[e.key]);
    } else if (e.key === 'Escape') {
      e.preventDefault();
      keyNav();
      back();
    } else if (e.key === 'Enter' && !typing(el)) {
      // (buttons press themselves on Enter; switches do not)
      if (el && el.tagName === 'INPUT' && el.type === 'checkbox') { e.preventDefault(); el.click(); }
      else if (el && el.tagName === 'BUTTON') press(el);
      keyNav();
    } else if (!typing(el) && !e.ctrlKey && (e.key === 'q' || e.key === 'e' || e.key === 'Q' || e.key === 'E')) {
      keyNav();
      cyclePage(e.key.toLowerCase() === 'q' ? -1 : 1);
    } else if (e.ctrlKey && (e.key === 'PageUp' || e.key === 'PageDown')) {
      e.preventDefault();
      keyNav();
      cyclePage(e.key === 'PageUp' ? -1 : 1);
    } else if (e.key === 'Tab') {
      keyNav();
    }
  });
  document.addEventListener('mousedown', () => document.body.classList.remove('kbnav'));

  // ---- controller (the Gamepad API: standard mapping) ------------------------------
  const kRepeatDelay = 380, kRepeat = 110, kDead = 0.5;
  const held = {};  // control -> next time it repeats
  let polling = false;
  function edge(name, down, now, fn, repeat = false) {
    if (!down) { delete held[name]; return; }
    if (held[name] === undefined) { held[name] = now + kRepeatDelay; fn(); usePad(); }
    else if (repeat && now >= held[name]) { held[name] = now + kRepeat; fn(); }
  }
  function usePad() { document.body.classList.add('pad', 'kbnav'); }
  function poll(now) {
    const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) : [];
    if (!pads.length) { polling = false; return; }
    if (document.hasFocus()) {
      const btn = (i) => pads.some((p) => p.buttons[i] && p.buttons[i].pressed);
      const axis = (i) => pads.reduce((v, p) => (Math.abs(p.axes[i] || 0) > Math.abs(v) ? p.axes[i] : v), 0);
      const dir = (name, b, a, sign) => edge(name, btn(b) || axis(a) * sign > kDead, now, () => {
        if ((name === 'left' || name === 'right') && stepChoice(name === 'left' ? -1 : 1)) return;
        move(name);
      }, true);
      dir('up', 12, 1, -1);
      dir('down', 13, 1, 1);
      dir('left', 14, 0, -1);
      dir('right', 15, 0, 1);
      edge('a', btn(0), now, activate);
      edge('b', btn(1), now, back);
      edge('lb', btn(4), now, () => cyclePage(-1));
      edge('rb', btn(5), now, () => cyclePage(1));
      edge('lt', btn(6), now, () => cycleGame(-1));
      edge('rt', btn(7), now, () => cycleGame(1));
      const scroll = axis(3);
      if (Math.abs(scroll) > 0.2 && activePage()) activePage().scrollBy({ top: scroll * 14, behavior: 'auto' });
    }
    requestAnimationFrame(poll);
  }
  function startPolling() {
    if (polling) return;
    polling = true;
    requestAnimationFrame(poll);
  }
  window.addEventListener('gamepadconnected', startPolling);
  if (navigator.getGamepads && [...navigator.getGamepads()].some(Boolean)) startPolling();
})();
