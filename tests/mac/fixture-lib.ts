// Tiny writers for the synthetic document fixtures used by the macOS engine tests.
// Used by tests/mac/make-fixtures.ts (committed fixtures) and tests/mac/run-engine-tests.ts
// (large PDF generated at test time). No dependencies beyond node:zlib.

import { deflateRawSync, deflateSync } from "node:zlib";

const enc = new TextEncoder();

export function bytes(s: string): Uint8Array {
  return enc.encode(s);
}

export function concat(parts: Uint8Array[]): Uint8Array {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

// deterministic PRNG so regenerated fixtures are byte-identical
export function prng(seed: number): () => number {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = s;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const crcTable = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = crcTable[(c ^ data[i]!) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

class ByteWriter {
  buf: number[] = [];
  u8(v: number) {
    this.buf.push(v & 0xff);
  }
  u16le(v: number) {
    this.u8(v);
    this.u8(v >>> 8);
  }
  u32le(v: number) {
    this.u16le(v & 0xffff);
    this.u16le(v >>> 16);
  }
  u16be(v: number) {
    this.u8(v >>> 8);
    this.u8(v);
  }
  u32be(v: number) {
    this.u16be(v >>> 16);
    this.u16be(v & 0xffff);
  }
  raw(d: Uint8Array | number[]) {
    for (const b of d) this.buf.push(b);
  }
  pad(n: number, v = 0) {
    for (let i = 0; i < n; i++) this.buf.push(v);
  }
  get length() {
    return this.buf.length;
  }
  bytes(): Uint8Array {
    return Uint8Array.from(this.buf);
  }
}

// ---- zip ----

export interface ZipEntry {
  name: string;
  data: Uint8Array | string;
  store?: boolean; // default: deflate
}

// Minimal zip writer (store or deflate, no zip64). Fixed timestamp for reproducibility.
export function makeZip(entries: ZipEntry[]): Uint8Array {
  const local: Uint8Array[] = [];
  const central: Uint8Array[] = [];
  let offset = 0;
  const dosTime = 0;
  const dosDate = ((2026 - 1980) << 9) | (1 << 5) | 1;
  for (const e of entries) {
    const data = typeof e.data === "string" ? bytes(e.data) : e.data;
    const name = bytes(e.name);
    const store = e.store ?? false;
    const comp = store ? data : new Uint8Array(deflateRawSync(data));
    const crc = crc32(data);
    const method = store ? 0 : 8;
    const lh = new ByteWriter();
    lh.u32le(0x04034b50);
    lh.u16le(20);
    lh.u16le(0);
    lh.u16le(method);
    lh.u16le(dosTime);
    lh.u16le(dosDate);
    lh.u32le(crc);
    lh.u32le(comp.length);
    lh.u32le(data.length);
    lh.u16le(name.length);
    lh.u16le(0);
    lh.raw(name);
    const lhb = lh.bytes();
    local.push(lhb, comp);

    const ch = new ByteWriter();
    ch.u32le(0x02014b50);
    ch.u16le(20);
    ch.u16le(20);
    ch.u16le(0);
    ch.u16le(method);
    ch.u16le(dosTime);
    ch.u16le(dosDate);
    ch.u32le(crc);
    ch.u32le(comp.length);
    ch.u32le(data.length);
    ch.u16le(name.length);
    ch.u16le(0);
    ch.u16le(0);
    ch.u16le(0);
    ch.u16le(0);
    ch.u32le(0);
    ch.u32le(offset);
    ch.raw(name);
    central.push(ch.bytes());
    offset += lhb.length + comp.length;
  }
  const cd = concat(central);
  const end = new ByteWriter();
  end.u32le(0x06054b50);
  end.u16le(0);
  end.u16le(0);
  end.u16le(entries.length);
  end.u16le(entries.length);
  end.u32le(cd.length);
  end.u32le(offset);
  end.u16le(0);
  return concat([...local, cd, end.bytes()]);
}

// ---- tar (ustar) ----

export function makeTar(entries: { name: string; data: Uint8Array }[]): Uint8Array {
  const parts: Uint8Array[] = [];
  for (const e of entries) {
    const h = new Uint8Array(512);
    const put = (s: string, off: number, len: number) => {
      const b = bytes(s);
      h.set(b.subarray(0, len), off);
    };
    const oct = (v: number, len: number) => v.toString(8).padStart(len - 1, "0");
    put(e.name, 0, 100);
    put(oct(0o644, 8), 100, 8);
    put(oct(0, 8), 108, 8);
    put(oct(0, 8), 116, 8);
    put(oct(e.data.length, 12), 124, 12);
    put(oct(1767225600, 12), 136, 12); // 2026-01-01
    h.fill(0x20, 148, 156);
    put("0", 156, 1);
    put("ustar\u000000", 257, 8);
    let sum = 0;
    for (const b of h) sum += b;
    put(oct(sum, 7) + "\u0000", 148, 8);
    parts.push(h, e.data);
    const rem = e.data.length % 512;
    if (rem) parts.push(new Uint8Array(512 - rem));
  }
  parts.push(new Uint8Array(1024));
  return concat(parts);
}

// ---- RAR 4.x ----

// RAR 1.5-4.x archive with every file in "store" mode (method 0x30). rar 7.x can no longer create
// RAR4 archives, and unrar / libarchive still read them, so we write the few headers ourselves.
export function makeRar4Store(entries: { name: string; data: Uint8Array }[]): Uint8Array {
  const block = (type: number, flags: number, body: Uint8Array): Uint8Array => {
    const h = new ByteWriter();
    h.u8(type);
    h.u16le(flags);
    h.u16le(7 + body.length);
    h.raw(body);
    const hb = h.bytes();
    const out = new ByteWriter();
    out.u16le(crc32(hb) & 0xffff);
    out.raw(hb);
    return out.bytes();
  };
  const parts: Uint8Array[] = [Uint8Array.from([0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00])];
  parts.push(block(0x73, 0, new Uint8Array(6)));
  const dosTime = (((2026 - 1980) << 9) | (1 << 5) | 1) * 65536;
  for (const e of entries) {
    const name = bytes(e.name);
    const b = new ByteWriter();
    b.u32le(e.data.length); // packed size
    b.u32le(e.data.length); // unpacked size
    b.u8(3); // host OS: Unix
    b.u32le(crc32(e.data));
    b.u32le(dosTime);
    b.u8(20); // version needed: 2.0
    b.u8(0x30); // method: store
    b.u16le(name.length);
    b.u32le(0o100644);
    b.raw(name);
    parts.push(block(0x74, 0x8000, b.bytes()), e.data);
  }
  parts.push(block(0x7b, 0x4000, new Uint8Array(0)));
  return concat(parts);
}

// ---- raster images ----

export type Rgb = [number, number, number];
export type PixelFn = (x: number, y: number) => Rgb;

// solid background with a darker frame and a diagonal band, so pages are never uniform
export function patternPixels(bg: Rgb): PixelFn {
  return (x, y) => {
    if (x < 4 || y < 4 || (x + y) % 23 < 3) return [bg[0] >> 2, bg[1] >> 2, bg[2] >> 2];
    return bg;
  };
}

export function makePng(w: number, h: number, px: PixelFn): Uint8Array {
  const raw = new Uint8Array(h * (1 + w * 3));
  let o = 0;
  for (let y = 0; y < h; y++) {
    raw[o++] = 0;
    for (let x = 0; x < w; x++) {
      const [r, g, b] = px(x, y);
      raw[o++] = r;
      raw[o++] = g;
      raw[o++] = b;
    }
  }
  const chunk = (type: string, data: Uint8Array) => {
    const w = new ByteWriter();
    w.u32be(data.length);
    const td = concat([bytes(type), data]);
    w.raw(td);
    w.u32be(crc32(td));
    return w.bytes();
  };
  const ihdr = new ByteWriter();
  ihdr.u32be(w);
  ihdr.u32be(h);
  ihdr.raw([8, 2, 0, 0, 0]);
  return concat([
    Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr.bytes()),
    chunk("IDAT", new Uint8Array(deflateSync(raw))),
    chunk("IEND", new Uint8Array(0)),
  ]);
}

export function makeBmp(w: number, h: number, px: PixelFn): Uint8Array {
  const rowSize = (w * 3 + 3) & ~3;
  const bw = new ByteWriter();
  bw.raw(bytes("BM"));
  bw.u32le(54 + rowSize * h);
  bw.u32le(0);
  bw.u32le(54);
  bw.u32le(40);
  bw.u32le(w);
  bw.u32le(h);
  bw.u16le(1);
  bw.u16le(24);
  bw.u32le(0);
  bw.u32le(rowSize * h);
  bw.u32le(2835);
  bw.u32le(2835);
  bw.u32le(0);
  bw.u32le(0);
  for (let y = h - 1; y >= 0; y--) {
    for (let x = 0; x < w; x++) {
      const [r, g, b] = px(x, y);
      bw.raw([b, g, r]);
    }
    bw.pad(rowSize - w * 3);
  }
  return bw.bytes();
}

export function makeTga(w: number, h: number, px: PixelFn): Uint8Array {
  const bw = new ByteWriter();
  bw.raw([0, 0, 2, 0, 0, 0, 0, 0]);
  bw.u16le(0);
  bw.u16le(0);
  bw.u16le(w);
  bw.u16le(h);
  bw.u8(24);
  bw.u8(0); // bottom-left origin
  for (let y = h - 1; y >= 0; y--) {
    for (let x = 0; x < w; x++) {
      const [r, g, b] = px(x, y);
      bw.raw([b, g, r]);
    }
  }
  return bw.bytes();
}

// multi-page baseline TIFF, uncompressed RGB, little-endian
export function makeTiff(pages: { w: number; h: number; px: PixelFn }[]): Uint8Array {
  const bw = new ByteWriter();
  bw.raw(bytes("II"));
  bw.u16le(42);
  bw.u32le(8);
  for (let i = 0; i < pages.length; i++) {
    const { w, h, px } = pages[i]!;
    const nEntries = 10;
    const ifdStart = bw.length;
    const ifdSize = 2 + nEntries * 12 + 4;
    const bpsOff = ifdStart + ifdSize;
    const dataOff = bpsOff + 6;
    const dataLen = w * h * 3;
    const nextIfd = i + 1 < pages.length ? dataOff + dataLen + (dataLen & 1) : 0;
    const entry = (tag: number, type: number, count: number, value: number) => {
      bw.u16le(tag);
      bw.u16le(type);
      bw.u32le(count);
      if (type === 3 && count === 1) {
        bw.u16le(value);
        bw.u16le(0);
      } else {
        bw.u32le(value);
      }
    };
    bw.u16le(nEntries);
    entry(256, 4, 1, w);
    entry(257, 4, 1, h);
    entry(258, 3, 3, bpsOff);
    entry(259, 3, 1, 1);
    entry(262, 3, 1, 2);
    entry(273, 4, 1, dataOff);
    entry(277, 3, 1, 3);
    entry(278, 4, 1, h);
    entry(279, 4, 1, dataLen);
    entry(284, 3, 1, 1);
    bw.u32le(nextIfd);
    bw.u16le(8);
    bw.u16le(8);
    bw.u16le(8);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) bw.raw(px(x, y));
    }
    if (dataLen & 1) bw.u8(0);
  }
  return bw.bytes();
}

export function makePpm(w: number, h: number, px: PixelFn): Uint8Array {
  const body = new Uint8Array(w * h * 3);
  let o = 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const [r, g, b] = px(x, y);
      body[o++] = r;
      body[o++] = g;
      body[o++] = b;
    }
  }
  return concat([bytes(`P6\n${w} ${h}\n255\n`), body]);
}

// 1-bit bitmap (P4), black = true
export function makePbm(w: number, h: number, black: (x: number, y: number) => boolean): Uint8Array {
  const rowBytes = (w + 7) >> 3;
  const body = new Uint8Array(rowBytes * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (black(x, y)) body[y * rowBytes + (x >> 3)]! |= 0x80 >> (x & 7);
    }
  }
  return concat([bytes(`P4\n${w} ${h}\n`), body]);
}

// ---- PDF ----

export function pdfStr(s: string): string {
  return "(" + s.replace(/\\/g, "\\\\").replace(/\(/g, "\\(").replace(/\)/g, "\\)") + ")";
}

export class PdfWriter {
  objs: string[] = [];

  alloc(): number {
    this.objs.push("");
    return this.objs.length;
  }

  set(num: number, body: string) {
    this.objs[num - 1] = body;
  }

  add(body: string): number {
    const n = this.alloc();
    this.set(n, body);
    return n;
  }

  stream(dict: string, data: string): string {
    return `<< ${dict} /Length ${bytes(data).length} >>\nstream\n${data}\nendstream`;
  }

  build(root: number, info?: number): Uint8Array {
    const parts: string[] = ["%PDF-1.7\n%âãÏÓ\n"];
    // header has 4 non-ASCII chars that are 2 bytes each in UTF-8; track real byte offsets
    const offsets: number[] = [];
    let pos = bytes(parts[0]!).length;
    for (let i = 0; i < this.objs.length; i++) {
      offsets.push(pos);
      const s = `${i + 1} 0 obj\n${this.objs[i]}\nendobj\n`;
      parts.push(s);
      pos += bytes(s).length;
    }
    let xref = `xref\n0 ${this.objs.length + 1}\n0000000000 65535 f \n`;
    for (const o of offsets) xref += `${String(o).padStart(10, "0")} 00000 n \n`;
    const infoRef = info ? ` /Info ${info} 0 R` : "";
    xref += `trailer\n<< /Size ${this.objs.length + 1} /Root ${root} 0 R${infoRef} >>\nstartxref\n${pos}\n%%EOF\n`;
    parts.push(xref);
    return bytes(parts.join(""));
  }
}

export interface PdfPageSpec {
  width: number;
  height: number;
  lines: string[]; // drawn top-down at 24pt then 14pt
}

// text page content: first line big, rest normal; returns content stream
function pageContent(p: PdfPageSpec): string {
  const ops: string[] = [];
  let y = p.height - 72;
  p.lines.forEach((line, i) => {
    const size = i === 0 ? 24 : 14;
    ops.push(`BT /F1 ${size} Tf 72 ${y} Td ${pdfStr(line)} Tj ET`);
    y -= size + 12;
  });
  return ops.join("\n");
}

export interface LinkSpec {
  page: number; // 1-based
  rect: [number, number, number, number]; // PDF user space x1 y1 x2 y2
  toPage?: number;
  uri?: string;
  label: string; // drawn inside the rect
}

export interface OutlineSpec {
  title: string;
  page: number;
  children?: OutlineSpec[];
}

export interface PdfDocSpec {
  pages: PdfPageSpec[];
  links?: LinkSpec[];
  outline?: OutlineSpec[];
  title?: string;
  author?: string;
}

export function makePdf(spec: PdfDocSpec): Uint8Array {
  const w = new PdfWriter();
  const catalog = w.alloc();
  const pagesObj = w.alloc();
  const font = w.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  const pageObjs = spec.pages.map(() => w.alloc());

  const annots = new Map<number, number[]>();
  const extraContent = new Map<number, string[]>();
  for (const l of spec.links ?? []) {
    const [x1, y1, x2, y2] = l.rect;
    const action = l.uri
      ? `/A << /S /URI /URI ${pdfStr(l.uri)} >>`
      : `/Dest [${pageObjs[l.toPage! - 1]} 0 R /XYZ 0 ${spec.pages[l.toPage! - 1]!.height} 0]`;
    const a = w.add(`<< /Type /Annot /Subtype /Link /Rect [${x1} ${y1} ${x2} ${y2}] /Border [0 0 1] ${action} >>`);
    if (!annots.has(l.page)) annots.set(l.page, []);
    annots.get(l.page)!.push(a);
    if (!extraContent.has(l.page)) extraContent.set(l.page, []);
    extraContent.get(l.page)!.push(`0 0 1 rg BT /F1 14 Tf ${x1 + 4} ${y1 + 6} Td ${pdfStr(l.label)} Tj ET 0 g`);
  }

  spec.pages.forEach((p, i) => {
    const content = [pageContent(p), ...(extraContent.get(i + 1) ?? [])].join("\n");
    const c = w.add(w.stream("", content));
    const pa = annots.get(i + 1);
    const annotRef = pa ? ` /Annots [${pa.map((a) => `${a} 0 R`).join(" ")}]` : "";
    w.set(
      pageObjs[i]!,
      `<< /Type /Page /Parent ${pagesObj} 0 R /MediaBox [0 0 ${p.width} ${p.height}] ` +
        `/Resources << /Font << /F1 ${font} 0 R >> >> /Contents ${c} 0 R${annotRef} >>`,
    );
  });
  w.set(pagesObj, `<< /Type /Pages /Kids [${pageObjs.map((p) => `${p} 0 R`).join(" ")}] /Count ${pageObjs.length} >>`);

  let outlineRef = "";
  if (spec.outline && spec.outline.length > 0) {
    const root = w.alloc();
    const build = (items: OutlineSpec[], parent: number): { first: number; last: number; count: number } => {
      const nums = items.map(() => w.alloc());
      let count = items.length;
      items.forEach((it, i) => {
        let kids = "";
        if (it.children && it.children.length > 0) {
          const k = build(it.children, nums[i]!);
          kids = ` /First ${k.first} 0 R /Last ${k.last} 0 R /Count ${k.count}`;
          count += k.count;
        }
        const prev = i > 0 ? ` /Prev ${nums[i - 1]} 0 R` : "";
        const next = i + 1 < items.length ? ` /Next ${nums[i + 1]} 0 R` : "";
        w.set(
          nums[i]!,
          `<< /Title ${pdfStr(it.title)} /Parent ${parent} 0 R${prev}${next}${kids} ` +
            `/Dest [${pageObjs[it.page - 1]} 0 R /Fit] >>`,
        );
      });
      return { first: nums[0]!, last: nums[nums.length - 1]!, count };
    };
    const top = build(spec.outline, root);
    w.set(root, `<< /Type /Outlines /First ${top.first} 0 R /Last ${top.last} 0 R /Count ${top.count} >>`);
    outlineRef = ` /Outlines ${root} 0 R /PageMode /UseOutlines`;
  }
  w.set(catalog, `<< /Type /Catalog /Pages ${pagesObj} 0 R${outlineRef} >>`);

  let info: number | undefined;
  if (spec.title || spec.author) {
    const t = spec.title ? ` /Title ${pdfStr(spec.title)}` : "";
    const a = spec.author ? ` /Author ${pdfStr(spec.author)}` : "";
    info = w.add(`<<${t}${a} /Producer (SumatraPDF tests/mac/make-fixtures.ts) >>`);
  }
  return w.build(catalog, info);
}

// Large text PDF for stress tests. Generated at test time into tests/tmp/, not committed.
export function makeLargePdf(nPages: number): Uint8Array {
  const pages: PdfPageSpec[] = [];
  for (let i = 1; i <= nPages; i++) {
    pages.push({
      width: 612,
      height: 792,
      lines: [`Stress page ${i}`, `Marker word pangolin${i}`, "The quick brown fox jumps over the lazy dog."],
    });
  }
  return makePdf({ pages, title: `Large ${nPages} page fixture` });
}

// ---- PalmDB / MOBI ----

// PalmDOC LZ77 compression: literal runs + back-references (distance <= 2047, length 3..10).
export function palmdocCompress(src: Uint8Array): Uint8Array {
  const out: number[] = [];
  let i = 0;
  while (i < src.length) {
    let bestLen = 0;
    let bestDist = 0;
    if (i + 3 <= src.length) {
      const maxDist = Math.min(i, 2047);
      for (let d = 1; d <= maxDist; d++) {
        let l = 0;
        while (l < 10 && i + l < src.length && src[i + l] === src[i + l - d]) l++;
        if (l > bestLen) {
          bestLen = l;
          bestDist = d;
          if (l === 10) break;
        }
      }
    }
    if (bestLen >= 3) {
      const v = 0x8000 | (bestDist << 3) | (bestLen - 3);
      out.push(v >> 8, v & 0xff);
      i += bestLen;
      continue;
    }
    const c = src[i]!;
    if (c === 0 || (c >= 0x09 && c <= 0x7f)) {
      out.push(c);
      i++;
      continue;
    }
    // literal run of up to 8 bytes that need escaping
    let n = 1;
    while (n < 8 && i + n < src.length) {
      const c2 = src[i + n]!;
      if (c2 === 0 || (c2 >= 0x09 && c2 <= 0x7f)) break;
      n++;
    }
    out.push(n);
    for (let k = 0; k < n; k++) out.push(src[i + k]!);
    i += n;
  }
  return Uint8Array.from(out);
}

export function makePalmDb(name: string, typeCreator: string, records: Uint8Array[]): Uint8Array {
  const hdr = new ByteWriter();
  const nameBytes = bytes(name).subarray(0, 31);
  hdr.raw(nameBytes);
  hdr.pad(32 - nameBytes.length);
  hdr.u16be(0);
  hdr.u16be(0);
  const t = 0x7c25b080; // fixed palm timestamp
  hdr.u32be(t);
  hdr.u32be(t);
  hdr.u32be(0);
  hdr.u32be(0);
  hdr.u32be(0);
  hdr.u32be(0);
  hdr.raw(bytes(typeCreator));
  hdr.u32be(records.length * 2 - 1);
  hdr.u32be(0);
  hdr.u16be(records.length);
  let off = 78 + records.length * 8 + 2;
  records.forEach((r, i) => {
    hdr.u32be(off);
    hdr.u8(0);
    hdr.u8(0);
    hdr.u16be(i * 2);
    off += r.length;
  });
  hdr.u16be(0);
  return concat([hdr.bytes(), ...records]);
}

const kPalmRecSize = 4096;

function textRecords(text: Uint8Array, compress: boolean): Uint8Array[] {
  const recs: Uint8Array[] = [];
  for (let off = 0; off < text.length; off += kPalmRecSize) {
    const chunk = text.subarray(off, Math.min(off + kPalmRecSize, text.length));
    recs.push(compress ? palmdocCompress(chunk) : chunk);
  }
  return recs;
}

function palmDocHeader(compress: boolean, textLen: number, nRecs: number): ByteWriter {
  const w = new ByteWriter();
  w.u16be(compress ? 2 : 1);
  w.u16be(0);
  w.u32be(textLen);
  w.u16be(nRecs);
  w.u16be(kPalmRecSize);
  w.u16be(0);
  w.u16be(0);
  return w;
}

// Mobipocket book (BOOKMOBI), UTF-8 HTML text, optional image records referenced as recindex 1..n.
export function makeMobi(title: string, html: string, images: Uint8Array[], compress: boolean): Uint8Array {
  const text = bytes(html);
  const recs = textRecords(text, compress);
  const rec0 = palmDocHeader(compress, text.length, recs.length);
  const firstImage = images.length > 0 ? recs.length + 1 : 0xffffffff;
  const mobiStart = rec0.length;
  const titleBytes = bytes(title);
  const mobiHdrLen = 232;
  rec0.raw(bytes("MOBI"));
  rec0.u32be(mobiHdrLen);
  rec0.u32be(2); // book
  rec0.u32be(65001); // utf-8
  rec0.u32be(0x5ec0de);
  rec0.u32be(6);
  for (let i = 0; i < 10; i++) rec0.u32be(0xffffffff);
  rec0.u32be(recs.length + 1); // first non-book record
  rec0.u32be(16 + mobiHdrLen); // full name offset
  rec0.u32be(titleBytes.length);
  rec0.u32be(9); // english
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.u32be(6);
  rec0.u32be(firstImage);
  rec0.u32be(0); // huffman
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.u32be(0); // no EXTH
  rec0.pad(32, 0xff);
  rec0.u32be(0xffffffff); // drm offset
  rec0.u32be(0xffffffff); // drm count
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.pad(62, 0);
  rec0.u16be(0); // extra data flags: no trailing entries
  rec0.u32be(0xffffffff); // indx
  if (rec0.length !== mobiStart + mobiHdrLen) throw new Error(`bad MOBI header length ${rec0.length - mobiStart}`);
  rec0.raw(titleBytes);
  rec0.pad(4 - (rec0.length % 4));
  return makePalmDb(title.replace(/\s+/g, "_"), "BOOKMOBI", [rec0.bytes(), ...recs, ...images]);
}

// PalmDoc (TEXtREAd), plain text
export function makePalmDoc(title: string, text: string, compress: boolean): Uint8Array {
  const t = bytes(text);
  const recs = textRecords(t, compress);
  const rec0 = palmDocHeader(compress, t.length, recs.length);
  return makePalmDb(title.replace(/\s+/g, "_"), "TEXtREAd", [rec0.bytes(), ...recs]);
}

// ---- LIT (Microsoft Reader) ----

// An element of LIT's tokenized markup: text, or a tag with attributes and children.
export type LitNode = string | { tag: string; attrs?: Record<string, string>; children?: LitNode[] };

export interface LitItem {
  id: string; // internal name
  file: string; // original path, also its path in the reconstructed EPUB
  mime: string;
  spine?: LitNode[]; // spine (HTML) items: their tokenized markup
  data?: Uint8Array; // other items (images)
}

// UTF-8 of one code point, as LitUtf8Char() decodes it (codes like 0x8000 need 3 bytes)
function litChar(c: number): number[] {
  if (c < 0x80) return [c];
  if (c < 0x800) return [0xc0 | (c >> 6), 0x80 | (c & 0x3f)];
  return [0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f)];
}

function litChars(s: string): number[] {
  const out: number[] = [];
  for (const ch of s) out.push(...litChar(ch.codePointAt(0)!));
  return out;
}

const kLitCustom = 0x8000; // tag / attribute given by name instead of a table code
const kLitOpening = 1;
const kLitClosing = 2;

// Tokenized markup (calibre's UnBinary format) using only named tags and attributes, so no tag tables are needed.
function litTokens(nodes: LitNode[]): number[] {
  const out: number[] = [];
  for (const n of nodes) {
    if (typeof n === "string") {
      out.push(...litChars(n));
      continue;
    }
    const kids = n.children ?? [];
    const flags = kids.length > 0 ? kLitOpening : kLitOpening | kLitClosing;
    out.push(0, flags, ...litChar(kLitCustom), ...litChar(n.tag.length + 1), ...litChars(n.tag));
    for (const [name, value] of Object.entries(n.attrs ?? {})) {
      out.push(...litChar(kLitCustom), ...litChar(name.length + 1), ...litChars(name));
      out.push(...litChar([...value].length + 1), ...litChars(value));
    }
    out.push(0);
    if (kids.length > 0) out.push(...litTokens(kids), 0, kLitClosing, 1);
  }
  return out;
}

// big-endian 7-bit groups, high bit = more follow (LitEncInt)
function litEncInt(v: number): number[] {
  const groups: number[] = [];
  do {
    groups.unshift(v & 0x7f);
    v = Math.floor(v / 128);
  } while (v > 0);
  return groups.map((g, i) => (i < groups.length - 1 ? g | 0x80 : g));
}

function litSizedString(s: string): number[] {
  return [...litChar([...s].length), ...litChars(s)];
}

// Uncompressed, DRM-free .lit: every file lives in section 0 (no LZX), which LitDoc.cpp reads like any other.
export function makeLit(title: string, items: LitItem[]): Uint8Array {
  const files: { name: string; data: Uint8Array }[] = [];
  const nameList = new ByteWriter();
  nameList.u16le(0);
  nameList.u16le(1);
  const secName = "Uncompressed";
  nameList.u16le(secName.length);
  for (const ch of secName) nameList.u16le(ch.charCodeAt(0));
  nameList.u16le(0);
  files.push({ name: "::DataSpace/NameList", data: nameList.bytes() });

  // /manifest: root name, then 4 groups (spine, other html, css, images)
  const man: number[] = [4, ...bytes("root")];
  const groups = [items.filter((it) => it.spine), [], [], items.filter((it) => !it.spine)];
  for (const g of groups) {
    const w = new ByteWriter();
    w.u32le(g.length);
    man.push(...w.bytes());
    for (const it of g) {
      man.push(0, 0, 0, 0, ...litSizedString(it.id), ...litSizedString(it.file), ...litSizedString(it.mime), 0);
    }
  }
  files.push({ name: "/manifest", data: Uint8Array.from(man) });

  const opf: LitNode = {
    tag: "package",
    attrs: { xmlns: "http://www.idpf.org/2007/opf", version: "2.0", "unique-identifier": "id" },
    children: [
      {
        tag: "metadata",
        attrs: { "xmlns:dc": "http://purl.org/dc/elements/1.1/" },
        children: [
          { tag: "dc:identifier", attrs: { id: "id" }, children: ["lit-fixture"] },
          { tag: "dc:title", children: [title] },
        ],
      },
      {
        tag: "manifest",
        children: items.map((it) => ({ tag: "item", attrs: { id: it.id, href: it.file, "media-type": it.mime } })),
      },
      {
        tag: "spine",
        children: items.filter((it) => it.spine).map((it) => ({ tag: "itemref", attrs: { idref: it.id } })),
      },
    ],
  };
  files.push({ name: "/meta", data: Uint8Array.from(litTokens([opf])) });
  for (const it of items) {
    if (it.spine) files.push({ name: `/data/${it.id}/content`, data: Uint8Array.from(litTokens(it.spine)) });
    else files.push({ name: `/data/${it.id}`, data: it.data! });
  }

  // directory: one AOLL chunk listing every file as (section 0, offset, size)
  const entries: number[] = [];
  let off = 0;
  for (const f of files) {
    const name = bytes(f.name);
    entries.push(...litEncInt(name.length), ...name, ...litEncInt(0), ...litEncInt(off), ...litEncInt(f.data.length));
    off += f.data.length;
  }
  const chunkSize = Math.max(256, 48 + entries.length + 2 + 16);
  const chunk = new Uint8Array(chunkSize);
  chunk.set(bytes("AOLL"), 0);
  new DataView(chunk.buffer).setUint32(4, chunkSize - 2 - (48 + entries.length), true);
  chunk.set(entries, 48);
  new DataView(chunk.buffer).setUint16(chunkSize - 2, files.length, true);
  const dirHdr = new ByteWriter();
  dirHdr.raw(bytes("IFCM"));
  dirHdr.u32le(1);
  dirHdr.u32le(chunkSize);
  dirHdr.pad(12);
  dirHdr.u32le(1); // chunk count
  dirHdr.pad(4);
  const dir = concat([dirHdr.bytes(), chunk]);

  const hdrLen = 0x28;
  const nPieces = 5;
  const secHdrLen = 56;
  const dirOff = hdrLen + nPieces * 16 + secHdrLen;
  const contentOff = dirOff + dir.length;
  const w = new ByteWriter();
  w.raw(bytes("ITOLITLS"));
  w.u32le(1);
  w.u32le(hdrLen);
  w.u32le(nPieces);
  w.u32le(secHdrLen);
  w.pad(hdrLen - 24);
  for (let i = 0; i < nPieces; i++) {
    // piece 1 is the directory: u64 offset, u64 length
    w.u32le(i === 1 ? dirOff : 0);
    w.u32le(0);
    w.u32le(i === 1 ? dir.length : 0);
    w.u32le(0);
  }
  // secondary header: an ITSF block holding the content offset
  w.u32le(0);
  w.u32le(8);
  w.raw(bytes("ITSF"));
  w.u32le(4);
  w.pad(8);
  w.u32le(contentOff);
  w.u32le(0);
  w.pad(24);
  if (w.length !== dirOff) throw new Error(`bad LIT header size ${w.length}`);
  return concat([w.bytes(), dir, ...files.map((f) => f.data)]);
}

// Uncompressed MOBI of nChapters chapters (~4 KB each, separated by <mbp:pagebreak/>), generated at test time:
// big enough to be split into parts. The last chapter holds the word "serval".
export function makeLargeMobi(nChapters: number): Uint8Array {
  const parts: string[] = ["<html><head></head><body>"];
  for (let c = 1; c <= nChapters; c++) {
    parts.push(`<h1>Chapter ${c}</h1>`);
    for (let p = 1; p <= 40; p++) {
      parts.push(`<p>Chapter ${c} paragraph ${p}: the quick brown fox jumps over the lazy dog.</p>`);
    }
    if (c === nChapters) parts.push("<p>The last word is serval.</p>");
    parts.push("<mbp:pagebreak/>");
  }
  parts.push("</body></html>");
  return makeMobi(`Large ${nChapters} chapter fixture`, parts.join(""), [], false);
}
