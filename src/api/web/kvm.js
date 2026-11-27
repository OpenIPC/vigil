// SPDX-License-Identifier: GPL-3.0-or-later
// kvm.js: the zero channel's page (docs/api.md "Zero channel"). Pictures
// come over /ws as changed parts, each a JPEG (src/app/kvm_enc.h); the
// mouse, wheel, touch and keyboard go back as JSON (src/app/kvm_input.h).
'use strict';
(function () {
  const cv = document.getElementById('screen'), ctx = cv.getContext('2d');
  const wrap = document.getElementById('wrap'), msgEl = document.getElementById('msg');
  const $ = (id) => document.getElementById(id);
  let ws = null, scrW = 1280, scrH = 720, retry = 0, chain = Promise.resolve();
  let drawn = 0, drawnAt = performance.now(), rtt = null, pingT = null;
  const held = new Set();                       // keys down, released on blur
  let buttons = 0;                              // mouse buttons down (bits)

  function store(k, v) { try { localStorage.setItem('vigil-zc-' + k, v); } catch (e) { /* none */ } }
  function load(k) { try { return localStorage.getItem('vigil-zc-' + k); } catch (e) { return null; } }
  for (const id of ['size', 'quality']) {
    const v = load(id);
    if (v && [...$(id).options].some((o) => o.value === v)) $(id).value = v;
    $(id).addEventListener('change', () => { store(id, $(id).value); opts(); cv.focus(); });
  }

  function state(text, cls) {
    $('state').textContent = text;
    $('dot').className = cls || '';
  }
  function message(text) { msgEl.textContent = text || ''; msgEl.style.display = text ? 'flex' : 'none'; }
  function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }
  function opts() { send({ t: 'opts', scale: +$('size').value, quality: +$('quality').value }); }

  // The canvas as large as the window allows at the screen's shape.
  function fit() {
    const W = wrap.clientWidth, H = wrap.clientHeight, s = Math.min(W / scrW, H / scrH);
    const w = Math.max(1, Math.floor(scrW * s)), h = Math.max(1, Math.floor(scrH * s));
    cv.style.width = w + 'px'; cv.style.height = h + 'px';
    cv.style.left = Math.floor((W - w) / 2) + 'px'; cv.style.top = Math.floor((H - h) / 2) + 'px';
  }
  window.addEventListener('resize', fit);

  // ---- pictures -------------------------------------------------------------------------
  function picture(buf) {
    const dv = new DataView(buf);
    if (dv.getUint8(0) !== 0x46) return;
    const w = dv.getUint16(2, true), h = dv.getUint16(4, true), n = dv.getUint16(6, true);
    const seq = dv.getUint32(8, true), parts = [];
    let off = 12;
    for (let i = 0; i < n; ++i) {
      const x = dv.getUint16(off, true), y = dv.getUint16(off + 2, true), len = dv.getUint32(off + 8, true);
      parts.push({ x, y, blob: new Blob([new Uint8Array(buf, off + 12, len)], { type: 'image/jpeg' }) });
      off += 12 + len;
    }
    // Decoded side by side, drawn in order, then acked: the server sends
    // the next ones as these are drawn.
    chain = chain.then(() => Promise.all(parts.map((p) => createImageBitmap(p.blob)))).then((bms) => {
      if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
      bms.forEach((bm, i) => { ctx.drawImage(bm, parts[i].x, parts[i].y); bm.close(); });
      ++drawn;
      message('');
    }).catch(() => { /* a broken part: the next whole picture mends it */ }).then(() => send({ t: 'ack', seq }));
  }

  function connect() {
    state(retry ? 'reconnecting' : 'connecting', 'wait');
    ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    let hello = false;
    ws.onopen = () => { retry = 0; };
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') { picture(e.data); return; }
      let m;
      try { m = JSON.parse(e.data); } catch (err) { return; }
      if (m.type === 'hello') {
        hello = true;
        scrW = m.width; scrH = m.height; fit();
        $('user').textContent = m.user;
        state('live', 'live');
        opts();
        cv.focus();
      } else if (m.type === 'stats') {
        $('rate').textContent = Math.round(m.kbps) + ' kbit/s, ' + m.ms + ' ms to encode';
      } else if (m.type === 'pong') {
        rtt = performance.now() - m.c;
        $('rtt').textContent = 'RTT ' + Math.round(rtt) + ' ms';
      } else if (m.type === 'error') {
        message(m.message);
      }
    };
    ws.onclose = () => {
      ws = null;
      releaseAll();
      if (!hello) {
        // Refused at the door: logged out (or the session ended)?
        fetch('/session', { cache: 'no-store' }).then((r) => {
          if (r.status === 401) location.href = '/login';
          else again();
        }).catch(again);
      } else {
        again();
      }
    };
  }
  function again() {
    const s = Math.min(10, 1 << Math.min(retry++, 4));
    state('reconnecting in ' + s + ' s', 'bad');
    setTimeout(connect, s * 1000);
  }

  setInterval(() => {
    const now = performance.now();
    $('fps').textContent = (drawn * 1000 / (now - drawnAt)).toFixed(1) + ' fps';
    drawn = 0; drawnAt = now;
    send({ t: 'ping', c: now });
  }, 1000);

  // ---- mouse, wheel, touch ----------------------------------------------------------------
  let moveAt = 0, pending = null, moveTimer = null;
  function pos(e) {
    const r = cv.getBoundingClientRect();
    return { x: Math.round((e.clientX - r.left) * scrW / r.width), y: Math.round((e.clientY - r.top) * scrH / r.height) };
  }
  function flushMove() {
    if (moveTimer) { clearTimeout(moveTimer); moveTimer = null; }
    if (pending) { send({ t: 'move', x: pending.x, y: pending.y }); pending = null; moveAt = performance.now(); }
  }
  function move(p) {
    pending = p;
    const wait = 15 - (performance.now() - moveAt);  // at most ~60 moves a second
    if (wait <= 0) flushMove(); else if (!moveTimer) moveTimer = setTimeout(flushMove, wait);
  }
  function button(b, down) {
    flushMove();
    if (down) buttons |= 1 << b; else buttons &= ~(1 << b);
    send({ t: 'btn', b, down });
  }
  cv.addEventListener('mousemove', (e) => move(pos(e)));
  cv.addEventListener('mousedown', (e) => {
    e.preventDefault(); cv.focus();
    move(pos(e));
    if (e.button <= 2) button(e.button, true);
  });
  window.addEventListener('mouseup', (e) => { if (e.button <= 2 && buttons & (1 << e.button)) button(e.button, false); });
  cv.addEventListener('contextmenu', (e) => e.preventDefault());
  let wheelAcc = 0;
  cv.addEventListener('wheel', (e) => {
    e.preventDefault();
    wheelAcc += e.deltaY * (e.deltaMode === 1 ? 33 : e.deltaMode === 2 ? 300 : 1);
    const n = Math.trunc(wheelAcc / 100);
    if (n) { wheelAcc -= n * 100; flushMove(); send({ t: 'wheel', n: -n }); }
  }, { passive: false });
  // A tap is a click; a finger moved is the pointer dragged.
  cv.addEventListener('touchstart', (e) => {
    e.preventDefault();
    const t = e.changedTouches[0];
    move(pos(t)); button(0, true);
  }, { passive: false });
  cv.addEventListener('touchmove', (e) => { e.preventDefault(); move(pos(e.changedTouches[0])); }, { passive: false });
  const touchEnd = (e) => { e.preventDefault(); move(pos(e.changedTouches[0])); button(0, false); };
  cv.addEventListener('touchend', touchEnd, { passive: false });
  cv.addEventListener('touchcancel', touchEnd, { passive: false });

  // ---- keyboard -----------------------------------------------------------------------------
  // Keys by their place (KeyboardEvent.code), as a USB keyboard at the
  // recorder sends them; the browser keeps none of them while the
  // picture has the focus.
  function key(code, v) { flushMove(); send({ t: 'key', code, v }); }
  cv.addEventListener('keydown', (e) => {
    if (!e.code) return;
    e.preventDefault();
    held.add(e.code);
    key(e.code, e.repeat ? 2 : 1);
  });
  cv.addEventListener('keyup', (e) => {
    if (!e.code) return;
    e.preventDefault();
    held.delete(e.code);
    key(e.code, 0);
  });
  function releaseAll() {
    for (const c of held) key(c, 0);
    held.clear();
    for (let b = 0; b < 3; ++b) if (buttons & (1 << b)) button(b, false);
  }
  cv.addEventListener('blur', releaseAll);
  for (const b of document.querySelectorAll('button[data-key]')) {
    b.addEventListener('click', () => { key(b.dataset.key, 1); key(b.dataset.key, 0); cv.focus(); });
  }

  fit();
  connect();
})();
