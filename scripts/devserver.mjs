// Local static server for testing the viewer outside Explorer:
//   node scripts/devserver.mjs
//   http://localhost:8777/viewer.html?model=/samples/<file>.vrm
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const web = path.join(root, 'dist/VrmPeek/web');
const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm',
  '.vrm': 'model/gltf-binary', '.json': 'application/json',
};

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = decodeURIComponent(url.pathname);
  const file = p.startsWith('/samples/') ? path.join(root, p) : path.join(web, p);
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404).end('not found');
    return;
  }
  const stat = fs.statSync(file);
  res.writeHead(200, {
    'Content-Type': TYPES[path.extname(file)] ?? 'application/octet-stream',
    'Content-Length': stat.size,
    'Cache-Control': 'no-store',
  });
  fs.createReadStream(file).pipe(res);
}).listen(8777, () => console.log('http://localhost:8777/viewer.html?model=/samples/Seed-san.vrm'));
