/**
 * Generate src/mac/Resources/SumatraPDF.icns from the Windows app icon.
 *
 * Usage: bun cmd/gen-mac-icon.ts
 *
 * Source: src/gfx/SumatraPDF-smaller.ico (by Alex, koo.studios at gmail.com; CC BY 3.0, see AUTHORS).
 * The .ico holds a 256px PNG plus hand-tuned 32bpp 16px and 32px bitmaps. 128px and 64px are box-filtered
 * from the 256px image. There is no larger source, so the 512px / 1024px slots (ic09, ic10, ic14) are left
 * out and macOS scales ic08 / ic13 up.
 *
 * Writes the ICNS container directly with PNG payloads, so it needs no iconutil / macOS.
 */

import { deflateSync, inflateSync } from "node:zlib";
import { readFileSync, writeFileSync } from "node:fs";

const srcIco = "src/gfx/SumatraPDF-smaller.ico";
const dstIcns = "src/mac/Resources/SumatraPDF.icns";

const pngSig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

interface Rgba {
  w: number;
  h: number;
  px: Uint8Array; // w * h * 4, straight (not premultiplied) alpha
}

// ICNS chunk type -> pixel size; PNG payloads (types from Apple's IconFamily.h)
const icnsSlots: [string, number][] = [
  ["icp4", 16],
  ["icp5", 32],
  ["ic11", 32], // 16pt @2x
  ["ic12", 64], // 32pt @2x
  ["ic07", 128],
  ["ic13", 256], // 128pt @2x
  ["ic08", 256],
];

const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf: Uint8Array): number {
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function paeth(a: number, b: number, c: number): number {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

// 8-bit RGB / RGBA, non-interlaced PNG only (what the .ico contains)
function decodePng(data: Buffer): Rgba {
  if (!data.subarray(0, 8).equals(pngSig)) throw new Error("not a PNG");
  let pos = 8;
  let w = 0;
  let h = 0;
  let colorType = 0;
  const idat: Buffer[] = [];
  while (pos < data.length) {
    const len = data.readUInt32BE(pos);
    const type = data.toString("latin1", pos + 4, pos + 8);
    const body = data.subarray(pos + 8, pos + 8 + len);
    if (type === "IHDR") {
      w = body.readUInt32BE(0);
      h = body.readUInt32BE(4);
      const bitDepth = body[8];
      colorType = body[9];
      const interlace = body[12];
      if (bitDepth !== 8 || (colorType !== 6 && colorType !== 2) || interlace !== 0) {
        throw new Error(`unsupported PNG: depth ${bitDepth} color ${colorType} interlace ${interlace}`);
      }
    } else if (type === "IDAT") {
      idat.push(body);
    }
    pos += 12 + len;
  }
  const bpp = colorType === 6 ? 4 : 3;
  const raw = inflateSync(Buffer.concat(idat));
  const stride = w * bpp;
  const cur = new Uint8Array(stride);
  const prev = new Uint8Array(stride);
  const px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    const off = y * (stride + 1);
    const filter = raw[off];
    for (let i = 0; i < stride; i++) {
      const x = raw[off + 1 + i];
      const a = i >= bpp ? cur[i - bpp] : 0;
      const b = prev[i];
      const c = i >= bpp ? prev[i - bpp] : 0;
      let v = x;
      if (filter === 1) v = x + a;
      else if (filter === 2) v = x + b;
      else if (filter === 3) v = x + ((a + b) >> 1);
      else if (filter === 4) v = x + paeth(a, b, c);
      cur[i] = v & 0xff;
    }
    for (let x = 0; x < w; x++) {
      const d = (y * w + x) * 4;
      px[d] = cur[x * bpp];
      px[d + 1] = cur[x * bpp + 1];
      px[d + 2] = cur[x * bpp + 2];
      px[d + 3] = bpp === 4 ? cur[x * bpp + 3] : 255;
    }
    prev.set(cur);
  }
  return { w, h, px };
}

function pngChunk(type: string, body: Uint8Array): Buffer {
  const out = Buffer.alloc(12 + body.length);
  out.writeUInt32BE(body.length, 0);
  out.write(type, 4, "latin1");
  out.set(body, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + body.length)), 8 + body.length);
  return out;
}

// per row, pick the filter with the smallest sum of absolute (signed) residuals
function encodePng(img: Rgba): Buffer {
  const stride = img.w * 4;
  const raw = Buffer.alloc(img.h * (stride + 1));
  const zero = new Uint8Array(stride);
  const cand = Array.from({ length: 5 }, () => new Uint8Array(stride));
  for (let y = 0; y < img.h; y++) {
    const cur = img.px.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? img.px.subarray((y - 1) * stride, y * stride) : zero;
    let best = 0;
    let bestSum = Infinity;
    for (let f = 0; f < 5; f++) {
      let sum = 0;
      for (let i = 0; i < stride; i++) {
        const a = i >= 4 ? cur[i - 4] : 0;
        const b = prev[i];
        const c = i >= 4 ? prev[i - 4] : 0;
        let p = 0;
        if (f === 1) p = a;
        else if (f === 2) p = b;
        else if (f === 3) p = (a + b) >> 1;
        else if (f === 4) p = paeth(a, b, c);
        const v = (cur[i] - p) & 0xff;
        cand[f][i] = v;
        sum += v < 128 ? v : 256 - v;
      }
      if (sum < bestSum) {
        bestSum = sum;
        best = f;
      }
    }
    raw[y * (stride + 1)] = best;
    raw.set(cand[best], y * (stride + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(img.w, 0);
  ihdr.writeUInt32BE(img.h, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  return Buffer.concat([
    pngSig,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", deflateSync(raw, { level: 9 })),
    pngChunk("IEND", new Uint8Array(0)),
  ]);
}

// 32bpp BI_RGB DIB (bottom-up BGRA + 1bpp AND mask) as stored in .ico
function decodeDib32(data: Buffer, w: number, h: number): Rgba {
  const hdrSize = data.readUInt32LE(0);
  const bitCount = data.readUInt16LE(14);
  const compression = data.readUInt32LE(16);
  if (bitCount !== 32 || compression !== 0)
    throw new Error(`unsupported DIB: ${bitCount}bpp, compression ${compression}`);
  const px = new Uint8Array(w * h * 4);
  const maskStride = ((w + 31) >> 5) * 4;
  const maskOff = hdrSize + w * h * 4;
  let anyAlpha = false;
  for (let y = 0; y < h; y++) {
    const srcRow = hdrSize + (h - 1 - y) * w * 4;
    for (let x = 0; x < w; x++) {
      const s = srcRow + x * 4;
      const d = (y * w + x) * 4;
      px[d] = data[s + 2];
      px[d + 1] = data[s + 1];
      px[d + 2] = data[s];
      px[d + 3] = data[s + 3];
      if (data[s + 3] !== 0) anyAlpha = true;
    }
  }
  // old-style 32bpp icons leave alpha at 0 and use the AND mask for transparency
  if (!anyAlpha) {
    for (let y = 0; y < h; y++) {
      const maskRow = maskOff + (h - 1 - y) * maskStride;
      for (let x = 0; x < w; x++) {
        const transparent = (data[maskRow + (x >> 3)] >> (7 - (x & 7))) & 1;
        px[(y * w + x) * 4 + 3] = transparent ? 0 : 255;
      }
    }
  }
  return { w, h, px };
}

interface IcoImage {
  size: number;
  bpp: number;
  png?: Buffer; // original PNG bytes, if stored as PNG
  img: Rgba;
}

function readIco(path: string): IcoImage[] {
  const data = readFileSync(path);
  if (data.readUInt16LE(0) !== 0 || data.readUInt16LE(2) !== 1) throw new Error(`${path}: not an .ico`);
  const count = data.readUInt16LE(4);
  const res: IcoImage[] = [];
  for (let i = 0; i < count; i++) {
    const e = 6 + i * 16;
    const size = data[e] || 256;
    const bpp = data.readUInt16LE(e + 6);
    const len = data.readUInt32LE(e + 8);
    const off = data.readUInt32LE(e + 12);
    const body = data.subarray(off, off + len);
    if (body.subarray(0, 8).equals(pngSig)) {
      res.push({ size, bpp, png: Buffer.from(body), img: decodePng(body) });
    } else if (bpp === 32) {
      res.push({ size, bpp, img: decodeDib32(body, size, size) });
    }
  }
  return res;
}

// area average with alpha weighting, so transparent pixels don't darken edges
function downscale(src: Rgba, size: number): Rgba {
  const f = src.w / size;
  if (!Number.isInteger(f) || src.w !== src.h) throw new Error(`can't box-filter ${src.w}px to ${size}px`);
  const px = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      let a = 0;
      let r = 0;
      let g = 0;
      let b = 0;
      for (let sy = y * f; sy < (y + 1) * f; sy++) {
        for (let sx = x * f; sx < (x + 1) * f; sx++) {
          const s = (sy * src.w + sx) * 4;
          const alpha = src.px[s + 3];
          a += alpha;
          r += src.px[s] * alpha;
          g += src.px[s + 1] * alpha;
          b += src.px[s + 2] * alpha;
        }
      }
      const d = (y * size + x) * 4;
      px[d + 3] = Math.round(a / (f * f));
      if (a > 0) {
        px[d] = Math.round(r / a);
        px[d + 1] = Math.round(g / a);
        px[d + 2] = Math.round(b / a);
      }
    }
  }
  return { w: size, h: size, px };
}

// prefer an exact-size image from the .ico (hand-tuned small sizes), else downscale the largest
function pngForSize(ico: IcoImage[], size: number): Buffer {
  const exact = ico.find((e) => e.size === size && (e.png || e.bpp === 32));
  if (exact) return exact.png ?? encodePng(exact.img);
  const largest = ico.reduce((a, b) => (b.size > a.size ? b : a));
  return encodePng(downscale(largest.img, size));
}

function writeIcns(chunks: [string, Buffer][]): Buffer {
  const parts: Buffer[] = [];
  let total = 8;
  for (const [type, data] of chunks) {
    const hdr = Buffer.alloc(8);
    hdr.write(type, 0, "latin1");
    hdr.writeUInt32BE(8 + data.length, 4);
    parts.push(hdr, data);
    total += 8 + data.length;
  }
  const head = Buffer.alloc(8);
  head.write("icns", 0, "latin1");
  head.writeUInt32BE(total, 4);
  return Buffer.concat([head, ...parts]);
}

function main(): void {
  const ico = readIco(srcIco);
  const bySize = new Map<number, Buffer>();
  const chunks: [string, Buffer][] = [];
  for (const [type, size] of icnsSlots) {
    if (!bySize.has(size)) bySize.set(size, pngForSize(ico, size));
    chunks.push([type, bySize.get(size)!]);
  }
  const icns = writeIcns(chunks);
  writeFileSync(dstIcns, icns);
  console.log(`wrote ${dstIcns} (${icns.length} bytes) from ${srcIco}`);
  for (const [type, data] of chunks) {
    const img = decodePng(data);
    console.log(`  ${type} ${img.w}x${img.h} png ${data.length} bytes`);
  }
}

main();
