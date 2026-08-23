/*
 * VrmPeek - WebGL viewer hosted inside the Windows Explorer preview pane.
 *
 * The native preview handler serves this page from a virtual host and
 * intercepts the request for MODEL_URL, streaming the .vrm the user selected.
 */

import * as THREE from 'three';
import { GLTFLoader } from 'three/examples/jsm/loaders/GLTFLoader.js';
import { DRACOLoader } from 'three/examples/jsm/loaders/DRACOLoader.js';
import { KTX2Loader } from 'three/examples/jsm/loaders/KTX2Loader.js';
import { MeshoptDecoder } from 'three/examples/jsm/libs/meshopt_decoder.module.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js';
import { VRMLoaderPlugin, VRMUtils } from '@pixiv/three-vrm';

const params = new URLSearchParams(location.search);
// The handler serves the file at this URL; ?model= is a development override.
const MODEL_URL = params.get('model') || 'https://model.vrmpeek.invalid/__model__.vrm';

const el = (id) => document.getElementById(id);
const dom = {
  stage: el('stage'),
  hud: el('hud'),
  bar: el('bar'),
  title: el('title'),
  sub: el('sub'),
  chips: el('chips'),
  panel: el('panel'),
  panelBody: el('panel-body'),
  loading: el('loading'),
  progress: el('progress'),
  progressFill: el('progress-fill'),
  status: el('status'),
  error: el('error'),
  errorTitle: el('error-title'),
  errorText: el('error-text'),
};

/* ------------------------------------------------------------------ theme */

function applyTheme() {
  const bg = params.get('bg');
  const fg = params.get('fg');
  const dark = params.get('theme') !== 'light';
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  const root = document.documentElement.style;
  if (bg) root.setProperty('--bg', bg);
  if (fg) root.setProperty('--fg', fg);
}
applyTheme();

/* ------------------------------------------------------------------ scene */

let renderer, scene, camera, controls, pmrem;
let vrm = null;
let root = null;
let dirty = true;
let spinning = false;   // user-toggled free turntable
let intro = null;       // one-shot reveal revolution, always lands facing front
let settle = null;      // short ease back to front when the reveal is interrupted
let framing = 'full';
let finished = false;   // a model is on screen, or a failure has been reported
let lastFrame = performance.now();

function initThree() {
  const canvas = document.createElement('canvas');
  canvas.id = 'gl';
  dom.stage.prepend(canvas);

  renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: true,
    alpha: true,
    powerPreference: 'high-performance',
  });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.toneMapping = THREE.NeutralToneMapping;
  renderer.toneMappingExposure = 1.0;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFSoftShadowMap;

  scene = new THREE.Scene();

  camera = new THREE.PerspectiveCamera(30, 1, 0.01, 100);
  camera.position.set(0, 1.3, 3);

  controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.08;
  controls.screenSpacePanning = true;
  controls.minDistance = 0.15;
  controls.maxDistance = 30;
  controls.target.set(0, 1.0, 0);
  controls.addEventListener('start', () => { cancelIntro(); setSpin(false); });
  controls.addEventListener('change', () => { dirty = true; });

  pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
  scene.environmentIntensity = 0.55;

  // Key / fill / rim, tuned so cel-shaded MToon avatars keep clean shading.
  const key = new THREE.DirectionalLight(0xffffff, 2.2);
  key.position.set(1.4, 2.6, 2.2);
  key.castShadow = true;
  key.shadow.mapSize.set(1024, 1024);
  key.shadow.bias = -0.0012;
  key.shadow.normalBias = 0.02;
  const cam = key.shadow.camera;
  cam.left = -1.6; cam.right = 1.6; cam.top = 2.6; cam.bottom = -0.2;
  cam.near = 0.1; cam.far = 12;
  scene.add(key, key.target);

  const fill = new THREE.DirectionalLight(0xdfe8ff, 0.7);
  fill.position.set(-2.2, 1.2, 1.0);
  scene.add(fill);

  const rim = new THREE.DirectionalLight(0xffffff, 1.1);
  rim.position.set(-0.8, 1.8, -2.6);
  scene.add(rim);

  scene.add(new THREE.HemisphereLight(0xffffff, 0x606070, 0.6));

  // Shadow catcher, kept invisible until we know where the feet are.
  const floor = new THREE.Mesh(
    new THREE.PlaneGeometry(12, 12),
    new THREE.ShadowMaterial({ opacity: 0.22 })
  );
  floor.rotation.x = -Math.PI / 2;
  floor.receiveShadow = true;
  floor.name = 'floor';
  scene.add(floor);

  new ResizeObserver(resize).observe(dom.stage);
  resize();
  renderer.setAnimationLoop(tick);
}

function resize() {
  const w = dom.stage.clientWidth || 1;
  const h = dom.stage.clientHeight || 1;
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
  dom.hud.classList.toggle('compact', w < 260);
  dom.bar.classList.toggle('compact', w < 340);
  dirty = true;
}

function tick() {
  const now = performance.now();
  const dt = Math.min((now - lastFrame) / 1000, 0.1);
  lastFrame = now;

  if (intro && root) {
    const k = Math.min(1, (now - intro.t0) / intro.dur);
    root.rotation.y = (k * k * (3 - 2 * k)) * Math.PI * 2;   // smoothstep, ends at 2pi
    dirty = true;
    if (k >= 1) { root.rotation.y = 0; intro = null; }
  } else if (settle && root) {
    const k = Math.min(1, (now - settle.t0) / settle.dur);
    const e = 1 - Math.pow(1 - k, 3);
    root.rotation.y = settle.from + (settle.to - settle.from) * e;
    dirty = true;
    if (k >= 1) { root.rotation.y = 0; settle = null; }
  } else if (spinning && root) {
    root.rotation.y += dt * 0.45;
    dirty = true;
  }

  if (controls.update(dt)) dirty = true;

  // Spring bones need a few frames to settle after load; keep feeding them
  // while anything else is moving.
  if (vrm && dirty) vrm.update(dt);

  if (dirty) {
    dirty = false;
    renderer.render(scene, camera);
  }
}

/* ---------------------------------------------------------------- framing */

function boxForMode(mode) {
  const box = new THREE.Box3().setFromObject(root);
  if (mode === 'full' || !vrm || !vrm.humanoid) return box;

  const bone = (name) => vrm.humanoid.getNormalizedBoneNode(name);
  const worldY = (node) => {
    if (!node) return null;
    return node.getWorldPosition(new THREE.Vector3()).y;
  };

  if (mode === 'upper') {
    const y = worldY(bone('hips')) ?? worldY(bone('spine'));
    if (y != null) box.min.y = Math.max(box.min.y, y - 0.08);
  } else if (mode === 'head') {
    const head = bone('head');
    const neck = bone('neck') || bone('upperChest') || bone('chest');
    const hy = worldY(head);
    if (hy != null) {
      const ny = worldY(neck) ?? hy - 0.18;
      const span = Math.max(0.16, (box.max.y - hy) + (hy - ny));
      box.min.y = ny - span * 0.35;
      box.max.y = Math.max(box.max.y, hy + span);
      const c = head.getWorldPosition(new THREE.Vector3());
      box.min.x = c.x - span; box.max.x = c.x + span;
      box.min.z = c.z - span; box.max.z = c.z + span;
    }
  }
  return box;
}

function frame(mode, { animate = true } = {}) {
  framing = mode;
  const box = boxForMode(mode);
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());

  // A wide accessory (or a T-pose arm span) must not shrink the avatar to
  // nothing in a narrow pane, so cap the width we fit for humanoids and let
  // the extremities run off the sides instead.
  let span = Math.max(size.x, size.z);
  if (vrm?.humanoid) span = Math.min(span, size.y * 1.05);

  const fov = THREE.MathUtils.degToRad(camera.fov);
  const fitH = size.y / (2 * Math.tan(fov / 2));
  const fitW = span / (2 * Math.tan(fov / 2) * camera.aspect);
  const dist = Math.max(fitH, fitW) * (mode === 'head' ? 1.22 : 1.06) + size.z * 0.4;

  const yaw = mode === 'head' ? THREE.MathUtils.degToRad(12) : THREE.MathUtils.degToRad(20);
  const pitch = mode === 'head' ? 0.03 : 0.06;
  const dir = new THREE.Vector3(Math.sin(yaw), pitch, Math.cos(yaw)).normalize();
  const to = center.clone().addScaledVector(dir, dist);

  controls.minDistance = Math.max(0.05, dist * 0.15);
  controls.maxDistance = dist * 6;

  if (!animate) {
    camera.position.copy(to);
    controls.target.copy(center);
    controls.update();
    dirty = true;
    return;
  }

  const fromPos = camera.position.clone();
  const fromTgt = controls.target.clone();
  const t0 = performance.now();
  const dur = 380;
  const step = () => {
    const k = Math.min(1, (performance.now() - t0) / dur);
    const e = k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2;
    camera.position.lerpVectors(fromPos, to, e);
    controls.target.lerpVectors(fromTgt, center, e);
    controls.update();
    dirty = true;
    if (k < 1) requestAnimationFrame(step);
  };
  step();
}

/* -------------------------------------------------------------- metadata */

const VRM0_LICENSE = {
  CC0: 'CC0', CC_BY: 'CC BY', CC_BY_NC: 'CC BY-NC', CC_BY_NC_ND: 'CC BY-NC-ND',
  CC_BY_NC_SA: 'CC BY-NC-SA', CC_BY_ND: 'CC BY-ND', CC_BY_SA: 'CC BY-SA',
  Other: 'Other', Redistribution_Prohibited: 'No redistribution',
};
const WORDS = {
  onlyAuthor: 'Author only', OnlyAuthor: 'Author only',
  onlySeparatelyLicensedPerson: 'Licensed persons', ExplicitlyLicensedPerson: 'Licensed persons',
  everyone: 'Everyone', Everyone: 'Everyone',
  personalNonProfit: 'Personal, non-profit', personalProfit: 'Personal, for-profit',
  corporation: 'Corporate', prohibited: 'Prohibited',
  allowModification: 'Allowed', allowModificationRedistribution: 'Allowed + redistribution',
  required: 'Required', unnecessary: 'Not required',
  Allow: 'Allowed', Disallow: 'Disallowed',
};
const word = (v) => (v == null ? null : WORDS[v] ?? String(v));
const yesNo = (v) => (v == null ? null : v ? 'Allowed' : 'Disallowed');

function readMeta(m) {
  if (!m) return { spec: null, name: null, authors: [], rows: [] };
  const rows = [];
  const push = (k, v) => { if (v != null && v !== '') rows.push([k, v]); };

  if (m.metaVersion === '1') {
    push('Version', m.version);
    push('Copyright', m.copyrightInformation);
    push('Contact', m.contactInformation);
    if (m.references?.length) push('References', m.references.join('\n'));
    push('License', m.licenseUrl);
    push('Other license', m.otherLicenseUrl);
    push('Third-party licenses', m.thirdPartyLicenses);
    push('Avatar use', word(m.avatarPermission));
    push('Commercial use', word(m.commercialUsage));
    push('Redistribution', yesNo(m.allowRedistribution));
    push('Modification', word(m.modification));
    push('Credit', word(m.creditNotation));
    push('Violent content', yesNo(m.allowExcessivelyViolentUsage));
    push('Sexual content', yesNo(m.allowExcessivelySexualUsage));
    push('Political / religious', yesNo(m.allowPoliticalOrReligiousUsage));
    push('Antisocial / hate', yesNo(m.allowAntisocialOrHateUsage));
    return { spec: 'VRM 1.0', name: m.name, authors: m.authors ?? [], rows };
  }

  push('Version', m.version);
  push('Contact', m.contactInformation);
  push('Reference', m.reference);
  push('License', VRM0_LICENSE[m.licenseName] ?? m.licenseName);
  push('Other license', m.otherLicenseUrl);
  push('Other permissions', m.otherPermissionUrl);
  push('Avatar use', word(m.allowedUserName));
  push('Commercial use', word(m.commercialUssageName));
  push('Violent content', word(m.violentUssageName));
  push('Sexual content', word(m.sexualUssageName));
  return {
    spec: 'VRM 0.x',
    name: m.title,
    authors: m.author ? [m.author] : [],
    rows,
  };
}

function countStats(object3d) {
  let tris = 0, verts = 0, meshes = 0, bones = 0;
  const materials = new Set();
  const textures = new Set();
  object3d.traverse((o) => {
    if (o.isBone) bones++;
    const g = o.geometry;
    if (!g || !o.isMesh) return;
    meshes++;
    const pos = g.attributes.position;
    if (pos) verts += pos.count;
    tris += g.index ? g.index.count / 3 : (pos ? pos.count / 3 : 0);
    for (const m of (Array.isArray(o.material) ? o.material : [o.material])) {
      if (!m) continue;
      materials.add(m);
      for (const k of Object.keys(m)) {
        const v = m[k];
        if (v && v.isTexture) textures.add(v.uuid);
      }
    }
  });
  return { tris: Math.round(tris), verts, meshes, bones, materials: materials.size, textures: textures.size };
}

const fmt = (n) => n.toLocaleString('en-US');
function bytes(n) {
  if (!n) return null;
  const u = ['B', 'KB', 'MB', 'GB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n < 10 && i > 0 ? n.toFixed(1) : Math.round(n)} ${u[i]}`;
}

function renderInfo(meta, stats, fileSize, extras) {
  const fileName = params.get('name') || 'model.vrm';
  dom.title.textContent = meta.name || fileName;
  dom.title.title = meta.name || fileName;

  const authors = meta.authors.filter(Boolean).join(', ');
  dom.sub.textContent = authors ? `by ${authors}` : (meta.name ? fileName : '');
  dom.sub.title = dom.sub.textContent;

  const chips = [];
  if (meta.spec) chips.push(meta.spec);
  else chips.push('glTF');
  chips.push(`${fmt(stats.tris)} tris`);
  if (stats.materials) chips.push(`${stats.materials} mat`);
  if (stats.bones) chips.push(`${stats.bones} bones`);
  const size = bytes(fileSize);
  if (size) chips.push(size);
  dom.chips.innerHTML = chips.map((c) => `<span class="chip">${esc(c)}</span>`).join('');

  const sections = [];
  const techRows = [
    ['File', fileName],
    ['Size', size],
    ['Format', meta.spec ?? 'glTF 2.0 (no VRM extension)'],
    ['Triangles', fmt(stats.tris)],
    ['Vertices', fmt(stats.verts)],
    ['Meshes', fmt(stats.meshes)],
    ['Materials', fmt(stats.materials)],
    ['Textures', fmt(stats.textures)],
    ['Bones', fmt(stats.bones)],
    ['Expressions', extras.expressions],
    ['Spring bones', extras.springs],
    ['Look-at', extras.lookAt],
    ['Generator', extras.generator],
  ].filter(([, v]) => v != null && v !== '' && v !== '0');
  sections.push(['Model', techRows]);
  if (meta.rows.length) sections.push(['License & metadata', meta.rows]);

  dom.panelBody.innerHTML = sections.map(([heading, rows]) => `
    <h3>${esc(heading)}</h3>
    <dl>${rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${linkify(String(v))}</dd>`).join('')}</dl>
  `).join('');

  dom.bar.classList.add('ready');
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
function linkify(s) {
  return esc(s)
    .replace(/(https?:\/\/[^\s<]+)/g, '<a href="$1" target="_blank" rel="noreferrer noopener">$1</a>')
    .replace(/\n/g, '<br>');
}

/* ------------------------------------------------------------------ load */

async function fetchModel() {
  const res = await fetch(MODEL_URL, { cache: 'no-store' });
  if (!res.ok) throw new Error(`Could not read the file (HTTP ${res.status}).`);

  const total = Number(res.headers.get('content-length')) || 0;
  if (!res.body) return { buffer: await res.arrayBuffer(), size: total };

  const reader = res.body.getReader();
  const chunks = [];
  let received = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    received += value.length;
    setProgress(total ? received / total : -1, received, total);
  }
  const buffer = new Uint8Array(received);
  let at = 0;
  for (const c of chunks) { buffer.set(c, at); at += c.length; }
  return { buffer: buffer.buffer, size: received };
}

function setProgress(ratio, received, total) {
  if (ratio < 0) {
    dom.progress.classList.add('indeterminate');
    dom.status.textContent = `Loading… ${bytes(received) ?? ''}`;
  } else {
    dom.progress.classList.remove('indeterminate');
    dom.progressFill.style.transform = `scaleX(${ratio})`;
    dom.status.textContent = `Loading… ${Math.round(ratio * 100)}%`;
  }
}

function makeLoader() {
  const loader = new GLTFLoader();

  const draco = new DRACOLoader();
  draco.setDecoderPath('libs/draco/');
  loader.setDRACOLoader(draco);

  const ktx2 = new KTX2Loader();
  ktx2.setTranscoderPath('libs/basis/');
  ktx2.detectSupport(renderer);
  loader.setKTX2Loader(ktx2);

  loader.setMeshoptDecoder(MeshoptDecoder);
  loader.register((parser) => new VRMLoaderPlugin(parser));
  return loader;
}

async function main() {
  if (!isWebGLAvailable()) {
    fail('3D preview unavailable', 'This PC could not start WebGL, so the model cannot be rendered.');
    return;
  }

  initThree();

  let payload;
  try {
    payload = await fetchModel();
  } catch (e) {
    fail('Could not read the file', e.message);
    return;
  }

  const magic = new Uint8Array(payload.buffer, 0, Math.min(4, payload.buffer.byteLength));
  if (magic.length < 4 || magic[0] !== 0x67 || magic[1] !== 0x6c || magic[2] !== 0x54 || magic[3] !== 0x46) {
    fail('Not a VRM file', 'The file does not start with a glTF binary header, so there is no model inside it.');
    return;
  }

  dom.status.textContent = 'Building scene…';
  // Yield with a timer, not rAF: the pane may be occluded and never paint.
  await new Promise((r) => setTimeout(r, 0));

  let gltf;
  try {
    gltf = await makeLoader().parseAsync(payload.buffer, '');
  } catch (e) {
    const msg = String(e && e.message || e);
    fail(
      'This file could not be opened',
      /draco|ktx|basis/i.test(msg)
        ? 'It uses a compression extension that failed to decode.'
        : `${msg.slice(0, 300)}`
    );
    return;
  }

  try {
    buildScene(gltf, payload.size);
  } catch (e) {
    fail('This file could not be displayed', trim(e && e.message || e));
  }
}

function buildScene(gltf, fileSize) {
  vrm = gltf.userData.vrm ?? null;
  root = new THREE.Group();

  const scene0 = gltf.scene ?? gltf.scenes?.[0];
  if (!vrm && !scene0) {
    fail('Nothing to preview', 'The file parsed correctly but contains no scene.');
    return;
  }

  if (vrm) {
    VRMUtils.removeUnnecessaryVertices(vrm.scene);
    VRMUtils.combineSkeletons(vrm.scene);
    VRMUtils.rotateVRM0(vrm);
    vrm.scene.traverse((o) => {
      o.frustumCulled = false;
      if (o.isMesh) { o.castShadow = true; o.receiveShadow = true; }
    });
    root.add(vrm.scene);
    vrm.update(0);
  } else {
    scene0.traverse((o) => {
      if (o.isMesh) { o.castShadow = true; o.receiveShadow = true; }
    });
    root.add(scene0);
  }

  let hasMesh = false;
  root.traverse((o) => { if (o.isMesh) hasMesh = true; });
  if (!hasMesh) {
    fail('Nothing to preview', 'The file contains no meshes to render.');
    return;
  }

  scene.add(root);

  // Drop the model onto the shadow plane and centre it on the origin.
  const box = new THREE.Box3().setFromObject(root);
  const c = box.getCenter(new THREE.Vector3());
  root.position.x -= c.x;
  root.position.z -= c.z;
  root.position.y -= box.min.y;

  const stats = countStats(root);
  const meta = readMeta(vrm?.meta);
  renderInfo(meta, stats, fileSize, {
    expressions: vrm?.expressionManager ? String(vrm.expressionManager.expressions.length) : null,
    springs: vrm?.springBoneManager ? String(vrm.springBoneManager.joints.size ?? vrm.springBoneManager.joints.length ?? 0) : null,
    lookAt: vrm?.lookAt ? 'Yes' : null,
    generator: gltf.asset?.generator,
  });

  finished = true;
  document.body.classList.add('loaded');
  dom.loading.remove();

  // Show the controls briefly so their existence is discoverable.
  dom.hud.classList.add('pinned');
  setTimeout(() => dom.hud.classList.remove('pinned'), 2600);

  frame('full', { animate: false });
  intro = { t0: performance.now(), dur: 5200 };
  dirty = true;
}

function isWebGLAvailable() {
  try {
    const c = document.createElement('canvas');
    return !!(window.WebGLRenderingContext && (c.getContext('webgl2') || c.getContext('webgl')));
  } catch {
    return false;
  }
}

function fail(title, text) {
  if (finished) return;
  finished = true;
  dom.errorTitle.textContent = title;
  dom.errorText.textContent = text || '';
  dom.error.hidden = false;
  dom.loading.remove();
  document.body.classList.add('failed');
}

// A preview pane stuck on a spinner is worse than a stated failure, so no
// error path may end silently.
const trim = (v) => String(v ?? '').slice(0, 300);
window.addEventListener('error', (e) => fail('Preview failed', trim(e.message)));
window.addEventListener('unhandledrejection', (e) => fail('Preview failed', trim(e.reason?.message ?? e.reason)));

/* -------------------------------------------------------------------- ui */

function cancelIntro() {
  if (!intro || !root) return;
  intro = null;
  const from = root.rotation.y % (Math.PI * 2);
  root.rotation.y = from;
  settle = { t0: performance.now(), dur: 280, from, to: from < Math.PI ? 0 : Math.PI * 2 };
}

function setSpin(on) {
  spinning = on;
  dom.hud.querySelector('[data-act="spin"]')?.classList.toggle('on', on);
  if (on) dirty = true;
}

dom.hud.addEventListener('click', (e) => {
  const btn = e.target.closest('button');
  if (!btn) return;
  switch (btn.dataset.act) {
    case 'spin':
      cancelIntro();
      setSpin(!spinning);
      break;
    case 'frame': {
      const order = ['full', 'upper', 'head'];
      frame(order[(order.indexOf(framing) + 1) % order.length]);
      break;
    }
    case 'reset':
      intro = settle = null;
      if (root) root.rotation.y = 0;
      frame('full');
      break;
    case 'info':
      dom.panel.classList.toggle('open');
      btn.classList.toggle('on', dom.panel.classList.contains('open'));
      break;
  }
});

dom.panel.querySelector('.panel-close').addEventListener('click', () => {
  dom.panel.classList.remove('open');
  dom.hud.querySelector('[data-act="info"]').classList.remove('on');
});

window.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && dom.panel.classList.contains('open')) {
    dom.panel.classList.remove('open');
    dom.hud.querySelector('[data-act="info"]').classList.remove('on');
  }
});

// The host re-sends theme information when Explorer switches light/dark.
window.chrome?.webview?.addEventListener('message', (e) => {
  const msg = e.data;
  if (!msg || msg.type !== 'theme') return;
  document.documentElement.dataset.theme = msg.dark ? 'dark' : 'light';
  if (msg.bg) document.documentElement.style.setProperty('--bg', msg.bg);
  if (msg.fg) document.documentElement.style.setProperty('--fg', msg.fg);
  dirty = true;
});

main();
