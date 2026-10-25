// San11 world editor (see server.py). Hex (lo, hi): lo grows east (x), hi grows south (y);
// odd lo columns sit half a hex further south. Neighbours as in gen_bases.py hex_nbrs.
'use strict';

const $ = (id) => document.getElementById(id);
const S = {
  meta: null, R: 0, C: 0, t: null, areas: null, links: null,
  cities: [], koei: [], sel: -1, tool: 'pan', ter: 0, brush: 3,
  s: 1, ox: 0, oy: 0, dirtyT: false, dirtyC: false,
  base: null, baseCtx: null, img: null, areaCanvas: null,
};
const cv = $('map'), ctx = cv.getContext('2d');

// ---------------------------------------------------------------- geometry
const nbrs = (y, x) => (y % 2 === 0
  ? [[0, -1], [0, 1], [-1, -1], [-1, 0], [1, -1], [1, 0]]
  : [[0, -1], [0, 1], [-1, 0], [-1, 1], [1, 0], [1, 1]]).map(([a, b]) => [y + a, x + b]);
const centre = (lo, hi) => [lo + 0.5, hi + 0.5 * (lo & 1) + 0.5];
const toScreen = (X, Y) => [(X - S.ox) * S.s, (Y - S.oy) * S.s];
function toHex(mx, my) {
  const X = mx / S.s + S.ox, Y = my / S.s + S.oy;
  const lo = Math.floor(X), hi = Math.floor(Y - 0.5 * (lo & 1));
  return [lo, hi];
}
const inWorld = (lo, hi) => lo >= 0 && lo < S.R && hi >= 0 && hi < S.C;
const inChina = (lo, hi) => {
  const m = S.meta; return lo >= m.china_lo && lo < m.china_lo + 200 && hi >= m.china_hi && hi < m.china_hi + 200;
};
const WATER = new Set([6, 7, 8]);

// ---------------------------------------------------------------- loading
async function load() {
  S.meta = await (await fetch('/api/meta')).json();
  S.R = S.meta.rows; S.C = S.meta.cols;
  S.cities = S.meta.cities.map((c) => ({ ...c }));
  S.koei = S.meta.koei;
  S.t = new Uint8Array(await (await fetch('/api/terrain')).arrayBuffer());
  buildPalette();
  buildBase();
  await loadBuildOutputs();
  $('bC').value = Math.min(S.meta.max_cities, 42 + S.cities.length);
  $('bOut').textContent = `${S.meta.game}\\${S.meta.out}`;
  fit();
  renderList();
  select(-1);
  setTool('pan');
}

async function loadBuildOutputs() {
  const a = await fetch('/api/areas');
  S.areas = a.ok ? new Uint16Array(await a.arrayBuffer()) : null;
  S.links = await (await fetch('/api/links')).json();
  S.areaCanvas = null;
  $('vAreas').disabled = !S.areas; $('vLinks').disabled = !S.links || !S.links.new;
}

function colorOf(id) { return S.meta.terrain[id] ? S.meta.terrain[id].color : [255, 0, 255]; }

function buildBase() {
  S.base = document.createElement('canvas');
  S.base.width = S.R; S.base.height = 2 * S.C + 1;
  S.baseCtx = S.base.getContext('2d');
  S.img = S.baseCtx.createImageData(S.base.width, S.base.height);
  for (let lo = 0; lo < S.R; lo++) for (let hi = 0; hi < S.C; hi++) paintPixel(lo, hi);
  S.baseCtx.putImageData(S.img, 0, 0);
}

function paintPixel(lo, hi) {
  const c = colorOf(S.t[lo * S.C + hi]), d = S.img.data, w = S.base.width;
  const y = 2 * hi + (lo & 1);
  for (const yy of [y, y + 1]) {
    const k = (yy * w + lo) * 4;
    d[k] = c[0]; d[k + 1] = c[1]; d[k + 2] = c[2]; d[k + 3] = 255;
  }
}

function buildAreaCanvas() {
  const c = document.createElement('canvas');
  c.width = S.R; c.height = 2 * S.C + 1;
  const g = c.getContext('2d'), im = g.createImageData(c.width, c.height), d = im.data;
  const cityCount = (S.links && S.links.C) || 42;
  for (let lo = 0; lo < S.R; lo++) for (let hi = 0; hi < S.C; hi++) {
    const a = S.areas[lo * S.C + hi];
    if (a >= cityCount) continue;                            // gates, ports, unclaimed and special areas
    const h = (a * 2654435761) >>> 0;
    const r = 80 + (h & 127), gg = 80 + ((h >> 8) & 127), b = 80 + ((h >> 16) & 127);
    let edge = false;
    for (const [p, q] of nbrs(lo, hi)) if (inWorld(p, q) && S.areas[p * S.C + q] !== a) { edge = true; break; }
    const y = 2 * hi + (lo & 1);
    for (const yy of [y, y + 1]) {
      const k = (yy * c.width + lo) * 4;
      d[k] = edge ? 20 : r; d[k + 1] = edge ? 20 : gg; d[k + 2] = edge ? 20 : b; d[k + 3] = edge ? 200 : 90;
    }
  }
  g.putImageData(im, 0, 0);
  S.areaCanvas = c;
}

// ---------------------------------------------------------------- drawing
let raf = 0;
function draw() { if (!raf) raf = requestAnimationFrame(() => { raf = 0; drawNow(); }); }

function drawNow() {
  const W = cv.clientWidth, H = cv.clientHeight;
  if (cv.width !== W || cv.height !== H) { cv.width = W; cv.height = H; }
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#111'; ctx.fillRect(0, 0, W, H);
  ctx.imageSmoothingEnabled = false;
  ctx.setTransform(S.s, 0, 0, S.s / 2, -S.ox * S.s, -S.oy * S.s);
  ctx.drawImage(S.base, 0, 0);
  if ($('vAreas').checked && S.areas) {
    if (!S.areaCanvas) buildAreaCanvas();
    ctx.drawImage(S.areaCanvas, 0, 0);
  }
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  if ($('vChina').checked) {
    const [x0, y0] = toScreen(S.meta.china_lo, S.meta.china_hi);
    ctx.strokeStyle = '#d8a54acc'; ctx.lineWidth = 1.5; ctx.setLineDash([6, 4]);
    ctx.strokeRect(x0, y0, 200 * S.s, 200.5 * S.s);
    ctx.setLineDash([]);
  }
  if ($('vLinks').checked && S.links && S.links.new) drawLinks();
  drawCities();
  if (S.tool === 'paint' && S.mouse) {
    const [X, Y] = centre(...S.mouse);
    const [x, y] = toScreen(X, Y);
    ctx.strokeStyle = '#fff8'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.arc(x, y, Math.max(2, (S.brush - 0.5) * S.s), 0, Math.PI * 2); ctx.stroke();
  }
}

function posOfBase(id) {                    // screen position of a city id (Koei 0..41 or new 42..)
  if (id < 42) { const k = S.koei.find((c) => c.id === id); return k ? toScreen(...centre(k.lo, k.hi)) : null; }
  const c = S.cities[id - 42]; return c ? toScreen(...centre(c.lo, c.hi)) : null;
}

function drawLinks() {
  const C = S.links.C;
  ctx.strokeStyle = '#ffffff70'; ctx.lineWidth = 1;
  ctx.beginPath();
  for (const c of S.links.new) {
    const a = posOfBase(c.id); if (!a) continue;
    for (const n of c.neighbours) {
      if (n >= C || (n >= 42 && n < c.id)) continue;      // gates/ports have no marker; draw each pair once
      const b = posOfBase(n); if (!b) continue;
      ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]);
    }
  }
  ctx.stroke();
}

function drawCities() {
  const names = $('vNames').checked && S.s >= 1.5;
  const used = +$('bC').value - 42;
  ctx.font = `${Math.round(Math.min(16, 9 + S.s))}px "Microsoft YaHei", sans-serif`;
  ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
  const r = Math.max(2.5, Math.min(10, S.s * 1.2));
  const mark = (x, y, fill, name, sel) => {
    ctx.beginPath(); ctx.arc(x, y, sel ? r + 2 : r, 0, Math.PI * 2);
    ctx.fillStyle = fill; ctx.fill();
    ctx.lineWidth = sel ? 2.5 : 1; ctx.strokeStyle = sel ? '#fff' : '#000a'; ctx.stroke();
    if (names || sel) {
      ctx.lineWidth = 3; ctx.strokeStyle = '#000b'; ctx.strokeText(name, x, y - r - 2);
      ctx.fillStyle = sel ? '#ffd27a' : '#fff'; ctx.fillText(name, x, y - r - 2);
    }
  };
  for (const k of S.koei) { const [x, y] = toScreen(...centre(k.lo, k.hi)); mark(x, y, '#5b8fd1', k.name, false); }
  S.cities.forEach((c, i) => {
    const [x, y] = toScreen(...centre(c.lo, c.hi));
    if (x < -50 || y < -50 || x > cv.width + 50 || y > cv.height + 50) return;
    const bad = cityProblem(i);
    mark(x, y, bad ? '#d0574b' : i < used ? '#e0a63c' : '#8a8a8a', c.name, i === S.sel);
  });
}

// ---------------------------------------------------------------- validation (what gen_bases.py accepts)
function cityProblem(i) {
  const c = S.cities[i];
  const cells = [[c.lo, c.hi], ...nbrs(c.lo, c.hi)];
  if (cells.some(([p, q]) => !inWorld(p, q))) return '超出地图';
  const nIn = cells.filter(([p, q]) => inChina(p, q)).length;
  if (nIn && nIn < cells.length) return '占地跨在中国区域的边上（要整个放在框里或框外）';
  const land = cells.filter(([p, q]) => !WATER.has(S.t[p * S.C + q])).length;
  if (land < 5) return `占地 7 格里只有 ${land} 格陆地（至少 5 格）`;
  for (let j = 0; j < S.cities.length; j++) {
    if (j === i) continue;
    const d = S.cities[j];
    const [ax, ay] = centre(c.lo, c.hi), [bx, by] = centre(d.lo, d.hi);
    if (Math.hypot(ax - bx, ay - by) < 3) return `和 ${d.name} 的占地重叠`;
  }
  return '';
}

// ---------------------------------------------------------------- palette, tools
function buildPalette() {
  const pal = $('pal'); pal.innerHTML = '';
  for (const t of S.meta.terrain) {
    if (t.id >= 16 && t.id <= 18) continue;                  // city / gate / port terrain comes from the build
    const b = document.createElement('button');
    b.innerHTML = `<i style="background:rgb(${t.color.join(',')})"></i>${t.name}`;
    b.dataset.ter = t.id;
    b.onclick = () => { S.ter = t.id; setTool('paint'); markPalette(); };
    pal.appendChild(b);
  }
  markPalette();
}
function markPalette() { for (const b of $('pal').children) b.classList.toggle('on', +b.dataset.ter === S.ter); }

const HINTS = {
  pan: '<b>拖动</b>平移，<b>滚轮</b>缩放。任何工具下都可以用<b>中键或空格+拖动</b>平移。',
  paint: '<b>左键</b>画选中的地形，<b>右键</b>吸取地形。中国区域（虚线框）是光荣原版地图，不能改。',
  city: '<b>单击</b>选城，<b>拖动</b>移动，在空地<b>双击</b>新建城市，<b>Delete</b> 删除。',
};
function setTool(t) {
  S.tool = t;
  for (const b of document.querySelectorAll('[data-tool]')) b.classList.toggle('on', b.dataset.tool === t);
  $('hint').innerHTML = HINTS[t];
  cv.style.cursor = t === 'pan' ? 'grab' : t === 'paint' ? 'crosshair' : 'pointer';
  draw();
}
for (const b of document.querySelectorAll('[data-tool]')) b.onclick = () => setTool(b.dataset.tool);
$('brush').oninput = (e) => { S.brush = +e.target.value; $('brushVal').textContent = S.brush; draw(); };

// ---------------------------------------------------------------- painting
function paintAt(lo, hi) {
  const r = S.brush - 0.5, [X0, Y0] = centre(lo, hi);
  let x0 = S.R, x1 = -1, y0 = 2 * S.C, y1 = -1, n = 0;
  for (let p = Math.floor(lo - r - 1); p <= lo + r + 1; p++) for (let q = Math.floor(hi - r - 1); q <= hi + r + 1; q++) {
    if (!inWorld(p, q) || inChina(p, q)) continue;
    const [X, Y] = centre(p, q);
    if (Math.hypot(X - X0, Y - Y0) > r) continue;
    const k = p * S.C + q;
    if (S.t[k] === S.ter) continue;
    S.t[k] = S.ter; paintPixel(p, q); n++;
    x0 = Math.min(x0, p); x1 = Math.max(x1, p); y0 = Math.min(y0, 2 * q); y1 = Math.max(y1, 2 * q + 2);
  }
  if (n) {
    S.baseCtx.putImageData(S.img, 0, 0, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    setDirty('t');
  }
}

// ---------------------------------------------------------------- cities
function nearestCity(lo, hi, maxD) {
  const [X, Y] = centre(lo, hi);
  let best = -1, bd = maxD;
  S.cities.forEach((c, i) => {
    const [a, b] = centre(c.lo, c.hi), d = Math.hypot(a - X, b - Y);
    if (d < bd) { bd = d; best = i; }
  });
  return best;
}

function renderList() {
  const f = $('find').value.trim();
  const used = +$('bC').value - 42;
  const list = $('list'); list.innerHTML = '';
  S.cities.forEach((c, i) => {
    if (f && !(c.name + (c.full || '') + (c.region || '')).includes(f)) return;
    const d = document.createElement('div');
    const bad = cityProblem(i);
    d.className = (i === S.sel ? 'sel ' : '') + (i >= used ? 'unused' : '');
    d.innerHTML = `<span class="id">${42 + i}</span><span>${c.name}</span><span style="color:var(--muted)">${c.region || ''}</span>` +
      (bad ? '<span class="warn" title="' + bad + '">⚠</span>' : '');
    d.title = (c.full || c.name) + (i >= used ? '（超出城市总数 C，本次不生成）' : '');
    d.onclick = () => { select(i); goTo(i); };
    list.appendChild(d);
  });
  if (!f) for (const k of S.koei) {
    const d = document.createElement('div'); d.className = 'koei';
    d.innerHTML = `<span class="id">${k.id}</span><span>${k.name}</span><span style="color:var(--muted)">光荣原版</span>`;
    d.onclick = () => { S.ox = k.lo + 0.5 - cv.clientWidth / 2 / S.s; S.oy = k.hi + 0.5 - cv.clientHeight / 2 / S.s; draw(); };
    list.appendChild(d);
  }
}

function select(i) {
  S.sel = i;
  const c = S.cities[i];
  for (const id of ['fName', 'fFull', 'fRegion', 'fLo', 'fHi', 'cGo', 'cUp', 'cDown', 'cDel']) $(id).disabled = !c;
  $('fId').textContent = c ? 42 + i : '—';
  $('fName').value = c ? c.name : ''; $('fFull').value = c ? c.full || '' : '';
  $('fRegion').value = c ? c.region || '' : '';
  $('fLo').value = c ? c.lo : ''; $('fHi').value = c ? c.hi : '';
  $('fWarn').textContent = c ? cityProblem(i) : '';
  renderList(); draw();
}

function goTo(i) {
  const c = S.cities[i]; if (!c) return;
  if (S.s < 3) S.s = 4;
  const [X, Y] = centre(c.lo, c.hi);
  S.ox = X - cv.clientWidth / 2 / S.s; S.oy = Y - cv.clientHeight / 2 / S.s; draw();
}

function edited() { setDirty('c'); $('fWarn').textContent = S.sel >= 0 ? cityProblem(S.sel) : ''; renderList(); draw(); }
$('fName').oninput = (e) => { if (S.sel < 0) return; S.cities[S.sel].name = e.target.value.trim(); edited(); };
$('fFull').oninput = (e) => { if (S.sel < 0) return; S.cities[S.sel].full = e.target.value; edited(); };
$('fRegion').oninput = (e) => { if (S.sel < 0) return; S.cities[S.sel].region = e.target.value; edited(); };
$('fLo').onchange = (e) => { if (S.sel < 0) return; S.cities[S.sel].lo = Math.max(0, Math.min(S.R - 1, +e.target.value)); edited(); };
$('fHi').onchange = (e) => { if (S.sel < 0) return; S.cities[S.sel].hi = Math.max(0, Math.min(S.C - 1, +e.target.value)); edited(); };
$('cGo').onclick = () => goTo(S.sel);
$('cDel').onclick = () => removeCity(S.sel);
$('cUp').onclick = () => moveInList(-1);
$('cDown').onclick = () => moveInList(1);
$('find').oninput = renderList;
$('bC').oninput = () => { renderList(); draw(); };

function moveInList(d) {
  const i = S.sel, j = i + d;
  if (i < 0 || j < 0 || j >= S.cities.length) return;
  [S.cities[i], S.cities[j]] = [S.cities[j], S.cities[i]];
  setDirty('c'); select(j);
}
function removeCity(i) {
  if (i < 0) return;
  if (!confirm(`删除城市 ${S.cities[i].name}？后面的城市编号会前移。`)) return;
  S.cities.splice(i, 1); setDirty('c'); select(-1);
}
function addCity(lo, hi) {
  const name = (prompt('新城市的名字（1–2 个字）') || '').trim();
  if (!name) return;
  S.cities.push({ name, full: name, region: '', lo, hi, rank: 3 });
  setDirty('c'); select(S.cities.length - 1);
  if (S.cities.length + 42 <= S.meta.max_cities) $('bC').value = Math.max(+$('bC').value, 42 + S.cities.length);
  renderList();
}

// ---------------------------------------------------------------- mouse
let drag = null, space = false;
cv.addEventListener('contextmenu', (e) => e.preventDefault());
cv.addEventListener('mousedown', (e) => {
  const [lo, hi] = toHex(e.offsetX, e.offsetY);
  if (e.button === 1 || S.tool === 'pan' || space) {
    drag = { kind: 'pan', x: e.offsetX, y: e.offsetY, ox: S.ox, oy: S.oy }; cv.style.cursor = 'grabbing'; return;
  }
  if (S.tool === 'paint') {
    if (e.button === 2) {                                   // eyedropper (city/gate/port terrain comes from the build)
      const v = inWorld(lo, hi) ? S.t[lo * S.C + hi] : -1;
      if (v >= 0 && (v < 16 || v > 18)) { S.ter = v; markPalette(); }
      return;
    }
    drag = { kind: 'paint' }; if (inWorld(lo, hi)) paintAt(lo, hi); draw(); return;
  }
  if (S.tool === 'city' && e.button === 0) {
    const i = nearestCity(lo, hi, Math.max(2, 8 / S.s));
    select(i);
    if (i >= 0) drag = { kind: 'city', i };
  }
});
window.addEventListener('mouseup', () => { if (drag && drag.kind === 'pan') setTool(S.tool); drag = null; });
cv.addEventListener('mousemove', (e) => {
  const [lo, hi] = toHex(e.offsetX, e.offsetY);
  S.mouse = [lo, hi];
  status(lo, hi);
  if (!drag) { if (S.tool === 'paint') draw(); return; }
  if (drag.kind === 'pan') {
    S.ox = drag.ox - (e.offsetX - drag.x) / S.s; S.oy = drag.oy - (e.offsetY - drag.y) / S.s;
  } else if (drag.kind === 'paint') {
    if (inWorld(lo, hi)) paintAt(lo, hi);
  } else if (drag.kind === 'city' && inWorld(lo, hi)) {
    const c = S.cities[drag.i];
    if (c.lo !== lo || c.hi !== hi) { c.lo = lo; c.hi = hi; setDirty('c'); select(drag.i); }
  }
  draw();
});
cv.addEventListener('dblclick', (e) => {
  if (S.tool !== 'city') return;
  const [lo, hi] = toHex(e.offsetX, e.offsetY);
  if (!inWorld(lo, hi) || nearestCity(lo, hi, 3) >= 0) return;
  addCity(lo, hi);
});
cv.addEventListener('wheel', (e) => {
  e.preventDefault();
  const X = e.offsetX / S.s + S.ox, Y = e.offsetY / S.s + S.oy;
  S.s = Math.max(0.3, Math.min(40, S.s * Math.pow(1.2, -Math.sign(e.deltaY))));
  S.ox = X - e.offsetX / S.s; S.oy = Y - e.offsetY / S.s; draw();
}, { passive: false });
window.addEventListener('keydown', (e) => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key === ' ') { space = true; cv.style.cursor = 'grab'; e.preventDefault(); }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); save(); }
  if (e.key === 'Delete' && S.sel >= 0) removeCity(S.sel);
  if (e.key === 'b' || e.key === 'B') setTool('paint');
  if (e.key === 'c' || e.key === 'C') setTool('city');
  if (e.key === 'v' || e.key === 'V') setTool('pan');
});
window.addEventListener('keyup', (e) => { if (e.key === ' ') { space = false; setTool(S.tool); } });
window.addEventListener('resize', draw);
for (const id of ['vAreas', 'vLinks', 'vNames', 'vChina']) $(id).onchange = draw;
$('fit').onclick = fit;

function fit() {
  const W = cv.clientWidth, H = cv.clientHeight;
  S.s = Math.min(W / S.R, H / (S.C + 0.5)); S.ox = -(W / S.s - S.R) / 2; S.oy = -(H / S.s - S.C) / 2; draw();
}

function status(lo, hi) {
  if (!inWorld(lo, hi)) { $('sHex').textContent = '—'; return; }
  $('sHex').textContent = `(${lo}, ${hi})` + (inChina(lo, hi) ? ` 中国 (${lo - S.meta.china_lo}, ${hi - S.meta.china_hi})` : '');
  const t = S.t[lo * S.C + hi];
  $('sTer').textContent = `${t} ${S.meta.terrain[t] ? S.meta.terrain[t].name : '?'}`;
  $('sArea').textContent = S.areas ? S.areas[lo * S.C + hi] : '未生成';
  const i = nearestCity(lo, hi, 1.6);
  $('sCity').textContent = i >= 0 ? `${42 + i} ${S.cities[i].name}` : '—';
}

// ---------------------------------------------------------------- save, build
function setDirty(k) {
  if (k === 't') S.dirtyT = true; else S.dirtyC = true;
  $('sDirty').textContent = '有未保存的修改' + (S.dirtyT ? '（地形）' : '') + (S.dirtyC ? '（城市）' : '');
  $('sDirty').style.color = 'var(--accent)';
}
function toast(msg, ms = 2500) {
  const t = $('toast'); t.textContent = msg; t.style.display = 'block';
  clearTimeout(toast.h); toast.h = setTimeout(() => { t.style.display = 'none'; }, ms);
}

async function save() {
  const bad = S.cities.map((c, i) => [c, cityProblem(i)]).filter(([, p]) => p);
  try {
    if (S.dirtyT) {
      const r = await fetch('/api/terrain', { method: 'PUT', body: S.t });
      if (!r.ok) throw new Error((await r.json()).error);
      S.dirtyT = false;
    }
    if (S.dirtyC) {
      const r = await fetch('/api/cities', { method: 'PUT', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(S.cities) });
      if (!r.ok) throw new Error((await r.json()).error);
      S.dirtyC = false;
    }
    $('sDirty').textContent = '已保存'; $('sDirty').style.color = 'var(--ok)';
    toast(bad.length ? `已保存。注意：${bad.length} 座城的位置有问题（列表里标 ⚠）` : '已保存');
    return true;
  } catch (e) { toast('保存失败：' + e.message, 5000); return false; }
}
$('save').onclick = save;

$('openBuild').onclick = () => { $('build').classList.add('open'); pollBuild(); };
$('closeBuild').onclick = () => $('build').classList.remove('open');
$('bGo').onclick = async () => {
  const C = +$('bC').value;
  const used = S.cities.slice(0, C - 42).map((c, i) => cityProblem(i)).filter(Boolean);
  if (used.length && !confirm(`前 ${C - 42} 座新城里有 ${used.length} 座位置有问题，仍然生成？`)) return;
  if ((S.dirtyT || S.dirtyC) && !(await save())) return;
  const r = await fetch('/api/build', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cities: C, rebuild3d: $('b3d').checked, deploy: $('bDeploy').checked }) });
  if (!r.ok) { toast((await r.json()).error); return; }
  pollBuild();
};
async function pollBuild() {
  const st = await (await fetch('/api/build')).json();
  $('bLog').textContent = st.log.join('\n'); $('bLog').scrollTop = 1e9;
  $('bGo').disabled = st.running;
  $('bState').textContent = st.running ? '生成中…' : '';
  if (st.running) setTimeout(pollBuild, 1000);
  else if (pollBuild.was) { await loadBuildOutputs(); draw(); toast('生成结束，看日志确认结果'); }
  pollBuild.was = st.running;
}
window.addEventListener('beforeunload', (e) => { if (S.dirtyT || S.dirtyC) { e.preventDefault(); e.returnValue = ''; } });

load().catch((e) => { document.body.innerHTML = `<pre style="padding:20px">加载失败：${e.message}\n请用 python tools/editor/server.py 启动编辑器。</pre>`; });
