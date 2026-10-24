// SPDX-License-Identifier: MIT
// Host web UI: frame generation and upscalers per app, the GPUs of this computer, OptiScaler per
// Steam game (docs/MULTI-GPU.md). Adds its own section to index.html; talks to /api/mgpu.
(function () {
  'use strict';
  const ENGINES = [
    ['auto', 'Automatic (second card if this seat has one)'],
    ['helper', 'Second graphics card (no cost for the game’s card)'],
    ['afmf', 'On the game’s card: afmf-linux (open source)'],
    ['lsfg', 'On the game’s card: Lossless Scaling (lsfg-vk, needs your purchase)'],
    ['nvsm', 'On the game’s card: NVIDIA Smooth Motion (RTX 40/50)'],
    ['off', 'Off'],
  ];
  const ENGINE_SHORT = { helper: 'second card', afmf: 'afmf-linux', lsfg: 'Lossless Scaling', nvsm: 'Smooth Motion', off: 'off' };
  const QUALITY = [['auto', 'automatic'], ['quality', 'best picture'], ['performance', 'fastest']];
  const PROXIES = ['dxgi', 'winmm', 'version', 'dbghelp', 'd3d12', 'wininet', 'winhttp'];

  const el = (tag, attrs, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') e.className = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
    }
    for (const k of kids) if (k !== null && k !== undefined) e.append(k.nodeType ? k : document.createTextNode(String(k)));
    return e;
  };
  const select = (opts, value, onchange) => {
    const s = el('select', { onchange, style: 'width:auto; max-width:100%' });
    for (const [v, t] of opts) s.append(el('option', { value: v, selected: v === value }, t));
    return s;
  };
  async function api(path, body) {
    const r = await fetch(path, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    let j = {};
    try { j = await r.json(); } catch (e) { j = { ok: false, error: 'HTTP ' + r.status }; }
    if (!r.ok && j.ok === undefined) j.ok = false;
    return j;
  }

  const sec = el('section', { id: 'mgpu' });
  const status = el('span', { class: 'mut', style: 'font-size:14px' });
  const appsBox = el('div', { class: 'list' });
  const enginesBox = el('div', { class: 'list' });
  const gpusBox = el('div', { class: 'scroll' });
  const gamesBox = el('div', { class: 'list' }, el('span', { class: 'mut' }, 'Open to look through the Steam library.'));
  const instLog = el('pre', { class: 'hidden' });
  const gamesDetails = el('details', {}, el('summary', {}, 'OptiScaler per Steam game (FSR / XeSS / frame generation instead of DLSS)'),
    el('p', { class: 'mut', style: 'font-size:14px; margin:0 0 8px' },
      'OptiScaler replaces the upscaler of a game that offers DLSS, FSR or XeSS, and can add frame generation (FSR 3 or XeSS) ',
      'on cards without DLSS frame generation, such as the RTX 3080. It is copied into the game’s folder; Remove restores ',
      'the game’s own files. Not for online games with anti-cheat.'),
    gamesBox);
  gamesDetails.addEventListener('toggle', () => { if (gamesDetails.open) loadGames(); });
  sec.append(
    el('h2', {}, 'Frame generation and upscalers ', status),
    el('p', { class: 'mut', style: 'font-size:14px; margin:0 0 10px' },
      'Frame generation puts a computed picture between two of the game’s pictures: up to twice the frames on the client’s ',
      'screen, about one frame more delay. With a second graphics card it costs the game’s card nothing.'),
    appsBox,
    el('details', {}, el('summary', {}, 'Graphics cards and installed parts'), gpusBox, enginesBox, instLog),
    gamesDetails);

  function engineLine(a) {
    const fg = a.framegen || {}, os = a.optiscaler || {};
    const osOn = os === false ? false : os.enabled !== false;
    const save = async (patch) => {
      const body = { id: a.id, framegen: Object.assign({}, fg, patch.framegen || {}), optiscaler: patch.optiscaler !== undefined ? patch.optiscaler : os };
      const j = await api('/api/mgpu/app', body);
      if (!j.ok) { status.textContent = j.error || 'not saved'; return; }
      status.textContent = 'saved';
      render(j);
    };
    const eng = fg.engine || 'auto';
    const row = el('div', { class: 'prog', style: 'flex-wrap:wrap' },
      el('div', { class: 'txt', style: 'flex:1 1 220px' },
        el('div', {}, a.name || a.id),
        el('div', {}, 'now: ', ENGINE_SHORT[a.engine] || a.engine, osOn ? ' · OptiScaler loads where installed' : '')));
    const ctl = el('div', { class: 'row' },
      select(ENGINES, eng, (e) => save({ framegen: { engine: e.target.value } })),
      select(QUALITY, fg.quality || 'auto', (e) => save({ framegen: { quality: e.target.value } })));
    if (eng === 'lsfg') {
      ctl.append(select([['2', '2×'], ['3', '3×'], ['4', '4×']], String(fg.multiplier || 2),
        (e) => save({ framegen: { multiplier: Number(e.target.value) } })));
    }
    ctl.append(select([['on', 'OptiScaler on'], ['off', 'OptiScaler off']], osOn ? 'on' : 'off',
      (e) => save({ optiscaler: { enabled: e.target.value === 'on', proxy: os.proxy || 'dxgi' } })));
    row.append(ctl);
    return row;
  }

  function linkText(g) {
    const l = g.link || {};
    const gen = { '2.5': 1, '5.0': 2, '8.0': 3, '16.0': 4, '32.0': 5, '64.0': 6 };
    const cur = (l.current_link_speed || '').split(' ')[0], max = (l.max_link_speed || '').split(' ')[0];
    if (!cur) return '';
    return 'PCIe ' + (gen[cur] || cur) + ' x' + l.current_link_width + (max && (max !== cur || l.max_link_width !== l.current_link_width)
      ? ' (card: ' + (gen[max] || max) + ' x' + l.max_link_width + ')' : '');
  }

  function render(j) {
    appsBox.replaceChildren(...(j.apps || []).map(engineLine));
    if (!(j.apps || []).length) appsBox.replaceChildren(el('span', { class: 'mut' }, 'No program apps yet (add Steam above).'));
    const seat = j.seat || {};
    const t = el('table', {}, el('tr', {}, el('th', {}, 'Card'), el('th', {}, 'PCI'), el('th', {}, 'Link'), el('th', {}, 'Slot'), el('th', {}, 'Role')));
    for (const g of j.gpus || []) {
      t.append(el('tr', {}, el('td', {}, g.name + (g.vram_mib ? ' (' + Math.round(g.vram_mib / 1024) + ' GB)' : '')),
        el('td', {}, g.pci), el('td', {}, linkText(g)),
        el('td', {}, g.root_port ? 'CPU' : 'chipset / switch'),
        el('td', {}, g.seat_gpu ? 'this seat' : g.helper_gpu ? 'frame generation' : '')));
    }
    gpusBox.replaceChildren(t, el('p', { class: 'mut', style: 'font-size:13px' },
      seat.helper_gpu ? 'Second card of this seat: ' + seat.helper_gpu + ' (set in Advanced settings → This seat).'
        : 'This seat has no second card (Advanced settings → This seat).'));
    const e = j.engines || {}, inst = j.install || {};
    const part = (name, have, comp, note) => el('div', { class: 'prog' },
      el('div', { class: 'txt' }, el('div', {}, name), el('div', {}, have ? 'installed' : note)),
      comp ? el('button', { disabled: inst.running, onclick: () => install(comp, !!have) }, have ? 'Remove' : 'Install') : '');
    enginesBox.replaceChildren(
      part('afmf-linux (open source frame generation)', e.afmf, 'afmf', 'not installed (built on this computer, about a minute)'),
      part('lsfg-vk (Lossless Scaling frame generation)', e.lsfg, 'lsfg',
        e.lossless_dll ? 'not installed' : 'not installed; needs Lossless Scaling from Steam (branch "lsfg-vk")'),
      part('Lossless Scaling DLL', e.lossless_dll, null, 'not found in the Steam library'),
      part('NVIDIA Smooth Motion (driver)', e.nv_smooth_motion_layer, null, 'not in this driver'),
      part('7z (unpacks OptiScaler)', e.sevenzip, e.sevenzip ? null : '7z', 'missing'));
    if (inst.component) {
      instLog.classList.remove('hidden');
      instLog.textContent = (inst.running ? 'Installing ' + inst.component + '…\n' : inst.component + ': exit ' + inst.exit + '\n') + (inst.log || '');
      if (inst.running) setTimeout(refresh, 3000);
    }
  }

  async function install(comp, remove) {
    const j = await api('/api/mgpu/install', { component: comp, remove });
    status.textContent = j.ok ? (remove ? 'removing ' : 'installing ') + comp + '…' : (j.error || 'failed');
    refresh();
  }

  async function loadGames() {
    gamesBox.replaceChildren(el('span', { class: 'mut' }, 'Looking through the Steam library…'));
    const j = await api('/api/mgpu/games');
    if (!j.ok) { gamesBox.replaceChildren(el('span', { class: 'mut' }, j.error || 'no answer')); return; }
    if (!(j.games || []).length) { gamesBox.replaceChildren(el('span', { class: 'mut' }, 'No Steam games found.')); return; }
    gamesBox.replaceChildren(...j.games.map((g) => {
      const has = g.optiscaler;
      const proxy = select(PROXIES.map((p) => [p, p + '.dll']), has ? has.proxy : 'dxgi');
      const fg = select([['native', 'frame generation: as the game offers'], ['fsrfg', 'add FSR 3 frame generation'], ['xefg', 'add XeSS frame generation']],
        has ? has.fg : 'native');
      const msg = el('span', { class: 'mut', style: 'font-size:13px' });
      const act = async (action) => {
        msg.textContent = action === 'install' ? 'copying…' : 'removing…';
        const r = await api('/api/mgpu/optiscaler', { action, appid: g.appid, proxy: proxy.value, fg: fg.value });
        msg.textContent = r.ok ? 'done' : (r.error || 'failed');
        if (r.ok) loadGames();
      };
      return el('div', { class: 'prog', style: 'flex-wrap:wrap' },
        el('div', { class: 'txt', style: 'flex:1 1 220px' }, el('div', {}, g.name),
          el('div', {}, (g.upscalers.length ? 'has ' + g.upscalers.join(', ').toUpperCase() : 'no upscaler DLL found') +
            (has ? ' · OptiScaler ' + has.version + ' as ' + has.proxy + '.dll' : ''))),
        el('div', { class: 'row' }, proxy, fg,
          el('button', { onclick: () => act('install') }, has ? 'Reinstall' : 'Install OptiScaler'),
          has ? el('button', { onclick: () => act('remove') }, 'Remove') : '', msg));
    }));
  }

  async function refresh() {
    const j = await api('/api/mgpu');
    if (j.ok === false && !j.gpus) { status.textContent = j.error || 'not available on this host'; return; }
    render(j);
  }

  function mount() {
    const adv = document.getElementById('adv');
    const main = document.querySelector('main');
    if (!main) return;
    const anchor = adv ? adv.closest('section') : null;
    if (anchor) main.insertBefore(sec, anchor); else main.append(sec);
    refresh();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount); else mount();
})();
