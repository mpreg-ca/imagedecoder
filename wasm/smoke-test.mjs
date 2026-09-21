// Decodes each file whole and streamed in small pushes through the wasm
// module, and fails if either throws or they disagree.
//
//   node wasm/smoke-test.mjs <imagedecoder.js, or the dir holding it> <image>...

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { join, resolve, basename } from 'node:path';

const [modulePath, ...files] = process.argv.slice(2);
if (!modulePath || files.length === 0) {
  console.error('usage: node smoke-test.mjs <module .js or dir> <image>...');
  process.exit(2);
}
const moduleFile = modulePath.endsWith('.js')
  ? resolve(modulePath)
  : join(resolve(modulePath), 'imagedecoder.js');
const { default: create } = await import(pathToFileURL(moduleFile).href);
const M = await create();

const equal = (a, b) =>
  a.length === b.length && a.every((v, i) => v === b[i]);

let failed = 0;
for (const path of files) {
  const bytes = new Uint8Array(readFileSync(path));
  const name = basename(path);
  try {
    const whole = M.ImageDecoder.open(bytes, true);
    const first = whole.decodeNext();
    let frames = 1;
    while (whole.hasNext) {
      whole.decodeNext();
      frames++;
    }
    const expected = first.width * first.height * (first.halfFloat ? 8 : 4);
    if (first.pixels.length !== expected) {
      throw new Error(`${first.pixels.length} bytes for ${first.width}x${first.height}`);
    }
    const format = whole.format;
    whole.delete();

    const streamed = M.ImageDecoder.open(bytes.subarray(0, 1), false);
    let at = 1;
    let frame = null;
    while (!frame) {
      try {
        const f = streamed.decodeNext();
        if (!f.partial) frame = f;
      } catch (e) {
        if (!/Need more data/.test(e.message) || at >= bytes.length) throw e;
        const n = Math.min(509, bytes.length - at);
        streamed.pushData(bytes.subarray(at, at + n));
        at += n;
        if (at >= bytes.length) streamed.markComplete();
      }
    }
    streamed.delete();
    if (!equal(frame.pixels, first.pixels)) {
      throw new Error('streamed decode differs from whole');
    }
    console.log(`ok   ${name}: ${format} ${first.width}x${first.height}, ${frames} frame(s)`);
  } catch (e) {
    console.log(`FAIL ${name}: ${e.message}`);
    failed++;
  }
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
