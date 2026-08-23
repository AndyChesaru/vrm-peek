import * as esbuild from 'esbuild';
import { cp, mkdir, rm, copyFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const out = path.resolve(here, '../../dist/VrmPeek/web');
const libs = path.resolve(here, 'node_modules/three/examples/jsm/libs');

await rm(out, { recursive: true, force: true });
await mkdir(path.join(out, 'libs'), { recursive: true });

const result = await esbuild.build({
  entryPoints: [path.join(here, 'src/viewer.js')],
  bundle: true,
  minify: true,
  format: 'esm',
  target: ['chrome120'],
  legalComments: 'none',
  outfile: path.join(out, 'viewer.js'),
  metafile: true,
});

await copyFile(path.join(here, 'index.html'), path.join(out, 'viewer.html'));
await cp(path.join(libs, 'draco/gltf'), path.join(out, 'libs/draco'), { recursive: true });
await cp(path.join(libs, 'basis'), path.join(out, 'libs/basis'), { recursive: true });
await rm(path.join(out, 'libs/basis/README.md'), { force: true });

let total = 0;
for (const [f, m] of Object.entries(result.metafile.outputs)) total += m.bytes;
const walk = async (d) => {
  let n = 0;
  for (const e of await readdir(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    n += e.isDirectory() ? await walk(p) : (await import('node:fs')).statSync(p).size;
  }
  return n;
};
console.log(`viewer.js  ${(total / 1024).toFixed(0)} KB`);
console.log(`web total  ${((await walk(out)) / 1024).toFixed(0)} KB  ->  ${out}`);
