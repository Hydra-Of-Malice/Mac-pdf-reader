// Generates the image, MOBI and CHM fixtures in tests/mac/fixtures/ that tests/mac/make-fixtures.ts doesn't:
// GIF (static + animated), JPEG 2000, HEIC, AVIF, a CBZ and a folder of mixed image formats, a HUFF/CDIC-compressed
// MOBI with a ToC and filepos links, an AZW4 (Print Replica) and CHMs.
//
// Regenerate: bun tests/mac/make-extra-fixtures.ts
//
// External tools (PATH, plus FIXTURE_TOOLS_DIR if set; when one is missing the committed file is kept):
//   opj_compress (JPEG 2000), heif-enc (HEIC), avifenc (AVIF), chmcmd or chmcmd-3.2.2 (CHM, Free Pascal)
// On Ubuntu 24.04 without root:
//   apt-get download libopenjp2-tools libopenjp2-7 libheif-examples libheif1 libheif-plugin-x265 libx265-199 \
//     libde265-0 libavif-bin libavif16 libaom3 libdav1d7 libsvtav1enc1d1 librav1e0 libyuv0 libgav1-1 \
//     libabsl20220623t64 fp-utils-3.2.2
//   for d in *.deb; do dpkg -x $d ~/tools; done
//   L=~/tools/usr/lib/x86_64-linux-gnu; FIXTURE_TOOLS_DIR=~/tools/usr/bin LD_LIBRARY_PATH=$L \
//     LIBHEIF_PLUGIN_PATH=$L/libheif/plugins bun tests/mac/make-extra-fixtures.ts
// The committed files were made that way (OpenJPEG 2.5.0, libheif 1.17.6 + x265, libavif 1.0.4, chmcmd 3.2.2).

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import {
  bytes,
  concat,
  makeBmp,
  makePalmDb,
  palmdocCompress,
  makePdf,
  makePng,
  makeTga,
  makeTiff,
  makeZip,
  patternPixels,
  type Rgb,
} from "./fixture-lib.ts";

const outDir = join(import.meta.dir, "fixtures");
const repoRoot = join(import.meta.dir, "..", "..");
mkdirSync(outDir, { recursive: true });

const written: string[] = [];
const skipped: string[] = [];

function save(name: string, data: Uint8Array | string) {
  const path = join(outDir, name);
  mkdirSync(join(path, ".."), { recursive: true });
  writeFileSync(path, typeof data === "string" ? bytes(data) : data);
  written.push(name);
}

class Writer {
  buf: number[] = [];
  u8(v: number) {
    this.buf.push(v & 0xff);
  }
  u16le(v: number) {
    this.u8(v);
    this.u8(v >>> 8);
  }
  u16be(v: number) {
    this.u8(v >>> 8);
    this.u8(v);
  }
  u32be(v: number) {
    this.u16be(v >>> 16);
    this.u16be(v);
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
  bytes() {
    return Uint8Array.from(this.buf);
  }
}

const red: Rgb = [220, 40, 40];
const green: Rgb = [40, 180, 60];
const blue: Rgb = [40, 70, 220];

// ---- GIF ----

// 16-colour palette: 0 white, 1 black, 2 red, 3 green, 4 blue, 5 yellow, rest greys; 15 is used as transparent
const gifPalette: Rgb[] = [
  [255, 255, 255],
  [0, 0, 0],
  red,
  green,
  blue,
  [240, 200, 40],
  ...Array.from({ length: 10 }, (_, i): Rgb => [40 + i * 20, 40 + i * 20, 40 + i * 20]),
];
const kGifTransparent = 15;

// LZW with the GIF variable code size (2^minCodeSize literal codes, clear, end of information)
function gifLzw(indices: Uint8Array, minCodeSize: number): Uint8Array {
  const clear = 1 << minCodeSize;
  const eoi = clear + 1;
  let codeSize = minCodeSize + 1;
  let next = eoi + 1;
  const dict = new Map<number, number>();
  const out: number[] = [];
  let acc = 0;
  let nBits = 0;
  const emit = (code: number) => {
    acc |= code << nBits;
    nBits += codeSize;
    while (nBits >= 8) {
      out.push(acc & 0xff);
      acc >>>= 8;
      nBits -= 8;
    }
  };
  emit(clear);
  let prefix = indices[0]!;
  for (let i = 1; i < indices.length; i++) {
    const k = indices[i]!;
    const key = prefix * 256 + k;
    const v = dict.get(key);
    if (v !== undefined) {
      prefix = v;
      continue;
    }
    emit(prefix);
    if (next < 4096) {
      dict.set(key, next++);
      if (next > 1 << codeSize && codeSize < 12) codeSize++;
    } else {
      emit(clear);
      dict.clear();
      next = eoi + 1;
      codeSize = minCodeSize + 1;
    }
    prefix = k;
  }
  emit(prefix);
  emit(eoi);
  if (nBits > 0) out.push(acc & 0xff);
  return Uint8Array.from(out);
}

interface GifFrame {
  x: number;
  y: number;
  w: number;
  h: number;
  px: (x: number, y: number) => number; // palette index, frame-relative
  disposal?: number; // 1 keep, 2 restore to background
  transparent?: boolean;
}

function makeGif(w: number, h: number, frames: GifFrame[]): Uint8Array {
  const bw = new Writer();
  bw.raw(bytes("GIF89a"));
  bw.u16le(w);
  bw.u16le(h);
  bw.u8(0x80 | 0x70 | 3); // global colour table of 16 entries
  bw.u8(0);
  bw.u8(0);
  for (const c of gifPalette) bw.raw(c);
  if (frames.length > 1) {
    bw.raw([0x21, 0xff, 11]);
    bw.raw(bytes("NETSCAPE2.0"));
    bw.raw([3, 1, 0, 0, 0]);
  }
  for (const f of frames) {
    bw.raw([0x21, 0xf9, 4, ((f.disposal ?? 1) << 2) | (f.transparent ? 1 : 0)]);
    bw.u16le(50);
    bw.raw([f.transparent ? kGifTransparent : 0, 0]);
    bw.u8(0x2c);
    bw.u16le(f.x);
    bw.u16le(f.y);
    bw.u16le(f.w);
    bw.u16le(f.h);
    bw.u8(0);
    const idx = new Uint8Array(f.w * f.h);
    for (let y = 0; y < f.h; y++) {
      for (let x = 0; x < f.w; x++) idx[y * f.w + x] = f.px(x, y);
    }
    const minCodeSize = 4;
    bw.u8(minCodeSize);
    const data = gifLzw(idx, minCodeSize);
    for (let i = 0; i < data.length; i += 255) {
      const chunk = data.subarray(i, Math.min(i + 255, data.length));
      bw.u8(chunk.length);
      bw.raw(chunk);
    }
    bw.u8(0);
  }
  bw.u8(0x3b);
  return bw.bytes();
}

// like patternPixels(): background colour with a dark frame and a diagonal band
const gifPattern = (bg: number) => (x: number, y: number) => (x < 3 || y < 3 || (x + y) % 17 < 2 ? 1 : bg);

const staticGif = makeGif(96, 64, [{ x: 0, y: 0, w: 96, h: 64, px: gifPattern(5) }]);
save("sample.gif", staticGif);
// 3 frames: full red, a green patch with transparent holes (kept), a blue patch (restored to background after)
save(
  "animated.gif",
  makeGif(64, 48, [
    { x: 0, y: 0, w: 64, h: 48, px: gifPattern(2) },
    {
      x: 32,
      y: 8,
      w: 24,
      h: 32,
      px: (x, y) => (((x >> 2) + (y >> 2)) % 2 === 0 ? 3 : kGifTransparent),
      transparent: true,
    },
    { x: 8, y: 16, w: 16, h: 16, px: () => 4, disposal: 2 },
  ]),
);

// ---- tools ----

const toolDirs = [...(process.env.FIXTURE_TOOLS_DIR ?? "").split(":"), ...(process.env.PATH ?? "").split(":")].filter(
  Boolean,
);

function findTool(names: string[]): string | null {
  for (const d of toolDirs) {
    for (const name of names) {
      const p = join(d, name);
      if (existsSync(p)) return p;
    }
  }
  return null;
}

function run(args: string[], cwd: string): boolean {
  const r = Bun.spawnSync(args, { cwd, stdin: "ignore", stdout: "pipe", stderr: "pipe" });
  if (r.exitCode !== 0) {
    console.error(
      `  ${args.join(" ")} failed (${r.exitCode}): ${r.stderr.toString().trim()} ${r.stdout.toString().trim()}`,
    );
    return false;
  }
  return true;
}

// runs fn in a scratch dir with the first found name of each tool; skips (keeping the committed file) if one is missing
function withTool(what: string, names: string[], fn: (tool: string, work: string) => void) {
  const tool = findTool(names);
  if (!tool) {
    skipped.push(`${what} (missing ${names.join(" / ")})`);
    return;
  }
  const work = join(tmpdir(), `sumatra-mac-extra-${what.replace(/\W+/g, "-")}`);
  rmSync(work, { recursive: true, force: true });
  mkdirSync(work, { recursive: true });
  try {
    fn(tool, work);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
}

function saveToolOutput(work: string, file: string, name: string) {
  const p = join(work, file);
  if (!existsSync(p)) {
    console.error(`  ${name}: tool wrote no ${file}`);
    return;
  }
  save(name, readFileSync(p));
}

// ---- JPEG 2000, HEIC, AVIF ----

const photoPng = makePng(96, 64, (x, y) => [Math.floor((x * 255) / 95), Math.floor((y * 255) / 63), 120]);

withTool("jp2", ["opj_compress"], (t, work) => {
  writeFileSync(join(work, "in.png"), photoPng);
  if (run([t, "-i", "in.png", "-o", "out.jp2"], work)) saveToolOutput(work, "out.jp2", "sample.jp2");
});
withTool("heic", ["heif-enc"], (t, work) => {
  writeFileSync(join(work, "in.png"), photoPng);
  if (run([t, "-q", "60", "-o", "out.heic", "in.png"], work)) saveToolOutput(work, "out.heic", "sample.heic");
});
withTool("avif", ["avifenc"], (t, work) => {
  writeFileSync(join(work, "in.png"), photoPng);
  if (run([t, "-q", "60", "in.png", "out.avif"], work)) saveToolOutput(work, "out.avif", "sample.avif");
});

// ---- a comic and a folder with one page per image format ----

{
  const repo = (p: string) => new Uint8Array(readFileSync(join(repoRoot, p)));
  const pages = [
    { name: "01.png", data: makePng(48, 64, patternPixels(red)) },
    { name: "02.gif", data: staticGif },
    { name: "03.jpg", data: repo("tests/issue-6236.jpg") },
    { name: "04.webp", data: repo("tests/issue-6245-data/halves.webp") },
    { name: "05.jxl", data: repo("tests/issue-6245-data/keong_macan.jxl") },
    { name: "06.tga", data: makeTga(48, 64, patternPixels(green)) },
    { name: "07.bmp", data: makeBmp(48, 64, patternPixels(blue)) },
    { name: "08.tif", data: makeTiff([{ w: 48, h: 64, px: patternPixels(red) }]) },
  ];
  const jp2 = join(outDir, "sample.jp2");
  if (existsSync(jp2)) pages.push({ name: "09.jp2", data: new Uint8Array(readFileSync(jp2)) });
  save("mixed-images.cbz", makeZip(pages.map((p) => ({ ...p, store: true }))));
  const dir = join(outDir, "image-folder");
  rmSync(dir, { recursive: true, force: true });
  for (const p of pages.slice(0, 3)) save(`image-folder/${p.name}`, p.data);
}

// ---- MOBI: HUFF/CDIC compression, ToC, filepos links ----

// Trivial but valid Mobipocket HUFF/CDIC: every byte is its own 8-bit code. Code c (the top byte of the bit
// stream) decodes to dictionary entry 255 - c, a 1-byte literal. Exercises the decoder's cache table and CDIC lookup.
function huffRecord(): Uint8Array {
  const w = new Writer();
  w.raw(bytes("HUFF"));
  w.u32be(24);
  w.u32be(24); // cache (big-endian)
  w.u32be(24 + 1024); // base table (big-endian)
  w.u32be(24 + 1024 + 256); // little-endian copies
  w.u32be(24 + 1024 + 256 + 1024);
  const cacheEntry = (255 << 8) | 0x80 | 8; // terminal, 8-bit code, max code 255
  for (let i = 0; i < 256; i++) w.u32be(cacheEntry);
  w.pad(256);
  for (let i = 0; i < 256; i++) {
    w.u8(cacheEntry);
    w.u8(cacheEntry >>> 8);
    w.u8(0);
    w.u8(0);
  }
  w.pad(256);
  return w.bytes();
}

function cdicRecord(): Uint8Array {
  const w = new Writer();
  w.raw(bytes("CDIC"));
  w.u32be(16);
  w.u32be(256);
  w.u32be(8);
  for (let i = 0; i < 256; i++) w.u16be(512 + i * 3);
  for (let i = 0; i < 256; i++) {
    w.u16be(0x8000 | 1);
    w.u8(i);
  }
  return w.bytes();
}

const kRecSize = 4096;

interface MobiOpts {
  type: number; // 2 book, 8 Print Replica
  compression: number; // 1 none, 2 PalmDOC, 17480 HUFF/CDIC
  encryption?: number; // PalmDOC header encryption type (2: Mobipocket DRM)
  drm?: boolean; // DRM records in the MOBI header
}

// BOOKMOBI with one text stream split into 4096-byte records; images referenced as recindex 1..n
function makeMobiEx(title: string, text: Uint8Array, images: Uint8Array[], o: MobiOpts): Uint8Array {
  const recs: Uint8Array[] = [];
  for (let off = 0; off < text.length; off += kRecSize) {
    const chunk = text.subarray(off, Math.min(off + kRecSize, text.length));
    if (o.compression === 17480) recs.push(chunk.map((b) => 255 - b));
    else recs.push(o.compression === 2 ? palmdocCompress(chunk) : chunk);
  }
  const huff = o.compression === 17480;
  const huffRecs = huff ? [huffRecord(), cdicRecord()] : [];
  const firstHuff = recs.length + 1;
  const firstImage = images.length > 0 ? firstHuff + huffRecs.length : 0xffffffff;
  const rec0 = new Writer();
  rec0.u16be(o.compression);
  rec0.u16be(0);
  rec0.u32be(text.length);
  rec0.u16be(recs.length);
  rec0.u16be(kRecSize);
  rec0.u16be(o.encryption ?? 0);
  rec0.u16be(0);
  const mobiStart = rec0.length;
  const titleBytes = bytes(title);
  const mobiHdrLen = 232;
  rec0.raw(bytes("MOBI"));
  rec0.u32be(mobiHdrLen);
  rec0.u32be(o.type);
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
  rec0.u32be(huff ? firstHuff : 0);
  rec0.u32be(huffRecs.length);
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.u32be(0); // no EXTH
  rec0.pad(32, 0xff);
  rec0.u32be(o.drm ? 0x200 : 0xffffffff); // drm offset
  rec0.u32be(o.drm ? 1 : 0xffffffff); // drm count
  rec0.u32be(0);
  rec0.u32be(0);
  rec0.pad(62, 0);
  rec0.u16be(0); // extra data flags: no trailing entries
  rec0.u32be(0xffffffff); // indx
  if (rec0.length !== mobiStart + mobiHdrLen) throw new Error(`bad MOBI header length ${rec0.length - mobiStart}`);
  rec0.raw(titleBytes);
  rec0.pad(4 - (rec0.length % 4));
  return makePalmDb(title.replace(/\s+/g, "_"), "BOOKMOBI", [rec0.bytes(), ...recs, ...huffRecs, ...images]);
}

// MOBI links are byte offsets into the text: <a filepos=0000001234>. Placeholders {name} are replaced by the
// 10-digit offset of the matching [name] marker (the marker itself is removed).
function resolveFilepos(tmpl: string): string {
  // placeholders first become 10 characters, the width of the offsets replacing them
  const names: string[] = [];
  const kSlot = "#".repeat(10);
  const fixed = tmpl.replace(/\{([a-z0-9]+)\}/g, (_, name: string) => {
    names.push(name);
    return kSlot;
  });
  const pos = new Map<string, number>();
  let text = "";
  for (const part of fixed.split(/(\[[a-z0-9]+\])/)) {
    const m = /^\[([a-z0-9]+)\]$/.exec(part);
    if (m) pos.set(m[1]!, bytes(text).length);
    else text += part;
  }
  let k = 0;
  return text.replaceAll(kSlot, () => {
    const p = pos.get(names[k++]!);
    if (p === undefined) throw new Error(`no marker [${names[k - 1]}]`);
    return String(p).padStart(10, "0");
  });
}

{
  const filler = (n: number, what: string) =>
    Array.from(
      { length: n },
      (_, i) => `<p>${what} paragraph ${i + 1} with enough words to fill a few lines.</p>`,
    ).join("");
  const html = resolveFilepos(
    `<html><head><guide><reference type="toc" title="Table of Contents" filepos={toc} /></guide></head><body>` +
      `<h1>HUFF/CDIC fixture</h1><p>The quoll hunts at night.</p>` +
      `<mbp:pagebreak/>[toc]<h2>Contents</h2>` +
      `<p><a filepos={ch1}>Chapter One</a></p><blockquote><a filepos={s11}>Section One.One</a></blockquote>` +
      `<p><a filepos={ch2}>Chapter Two</a></p>` +
      `<mbp:pagebreak/>[ch1]<h1>Chapter One</h1>${filler(30, "First")}` +
      `[s11]<h2>Section One.One</h2>${filler(30, "Section")}` +
      `<mbp:pagebreak/>[ch2]<h1>Chapter Two</h1><p>The potoroo digs for fungi.</p>` +
      `<p><img recindex="00001"/></p><p><a filepos={ch1}>Back to chapter one</a></p>` +
      `</body></html>`,
  );
  save(
    "huffcdic.mobi",
    makeMobiEx("HUFF CDIC fixture", bytes(html), [makePng(48, 64, patternPixels(green))], {
      type: 2,
      compression: 17480,
    }),
  );
}

// DRM: with DRM records, and encrypted; both open to a page that says the book can't be shown
{
  const html = bytes(`<html><body><p>Secret text of a protected book.</p></body></html>`);
  save("drm.azw", makeMobiEx("DRM fixture", html, [], { type: 2, compression: 1, drm: true }));
  save(
    "encrypted.mobi",
    makeMobiEx("Encrypted fixture", html, [], { type: 2, compression: 1, encryption: 2, drm: true }),
  );
}

// ---- AZW4 (Kindle Print Replica): a PDF inside a MOBI wrapper ----

// the text stream is a %MOP table: numTables, per-table section counts, then (offset, length) per section;
// the first section of the first table is the PDF
{
  const pdf = makePdf({
    title: "Print Replica fixture",
    pages: [
      { width: 432, height: 648, lines: ["Print Replica page one", "Printed books keep their layout."] },
      { width: 432, height: 648, lines: ["Print Replica page two", "The tarsier has huge eyes."] },
    ],
  });
  const hdrLen = 4 + 4 + 4 + 8;
  const mop = new Writer();
  mop.raw(bytes("%MOP"));
  mop.u32be(1);
  mop.u32be(1);
  mop.u32be(hdrLen);
  mop.u32be(pdf.length);
  const text = concat([mop.bytes(), pdf]);
  save("print-replica.azw4", makeMobiEx("Print Replica fixture", text, [], { type: 8, compression: 2 }));
}

// ---- CHM (chmcmd) ----

interface ChmPage {
  file: string; // "" for a ToC entry without a page
  title: string;
  body: string;
  level: number; // ToC depth, 0 = top
  raw?: Uint8Array; // the whole file instead of a page made from body
  tocLocal?: string; // the ToC's link to the page, if it's not file
}

function chmProject(name: string, title: string, pages: ChmPage[], extra: { file: string; data: Uint8Array }[]) {
  const write = (work: string, file: string, data: Uint8Array | string) => {
    mkdirSync(dirname(join(work, file)), { recursive: true });
    writeFileSync(join(work, file), data);
  };
  return (chmcmd: string, work: string) => {
    for (const p of pages) {
      if (!p.file) continue;
      const css = "../".repeat(p.file.split("/").length - 1) + "style.css";
      const html = `<html><head><title>${p.title}</title><link rel="stylesheet" href="${css}"></head><body>${p.body}</body></html>\n`;
      write(work, p.file, p.raw ?? html);
    }
    write(work, "style.css", "body { font-family: serif; } h1 { color: #336; }\n");
    for (const e of extra) write(work, e.file, e.data);
    // ToC: nested <UL> per level
    let toc = `<!DOCTYPE HTML PUBLIC "-//IETF//DTD HTML//EN">\n<HTML><HEAD></HEAD><BODY>\n<UL>\n`;
    let level = 0;
    for (const p of pages) {
      while (level < p.level) {
        toc += "<UL>\n";
        level++;
      }
      while (level > p.level) {
        toc += "</UL>\n";
        level--;
      }
      const local = p.tocLocal ?? p.file;
      const localParam = local ? `<param name="Local" value="${local}">` : "";
      toc += `<LI><OBJECT type="text/sitemap"><param name="Name" value="${p.title}">${localParam}</OBJECT>\n`;
    }
    while (level-- > 0) toc += "</UL>\n";
    toc += "</UL>\n</BODY></HTML>\n";
    writeFileSync(join(work, "toc.hhc"), toc);
    const files = [...pages.filter((p) => p.file).map((p) => p.file), "style.css", ...extra.map((e) => e.file)];
    const hhp =
      `[OPTIONS]\nCompatibility=1.1 or later\nCompiled file=${name}\nContents file=toc.hhc\n` +
      `Default topic=${pages[0]!.file}\nDisplay compile progress=No\nLanguage=0x409 English (United States)\n` +
      `Title=${title}\n\n[FILES]\n${files.join("\n")}\n`;
    writeFileSync(join(work, "project.hhp"), hhp);
    if (run([chmcmd, "--no-html-scan", "project.hhp"], work)) saveToolOutput(work, name, name);
  };
}

const chmTools = ["chmcmd", "chmcmd-3.2.2"];
withTool(
  "chm",
  chmTools,
  chmProject(
    "sample.chm",
    "CHM mac fixture",
    [
      {
        file: "index.html",
        title: "CHM Home",
        level: 0,
        body:
          `<h1>CHM Home</h1><p>The kinkajou climbs trees.</p><p><img src="img/pic.png" alt="pattern"></p>` +
          `<p><a href="topics/two.html">Go to topic two</a></p>` +
          `<p><a href="https://www.sumatrapdfreader.org/">Visit the website</a></p>`,
      },
      {
        file: "topics/one.html",
        title: "Topic One",
        level: 1,
        body: `<h1>Topic One</h1><p>The binturong smells of popcorn.</p><p><a href="../index.html">Home</a></p>`,
      },
      {
        file: "topics/two.html",
        title: "Topic Two",
        level: 1,
        body: `<h1>Topic Two</h1><p>The fossa hunts lemurs.</p><p><img src="../img/pic.png" alt="pattern"></p>`,
      },
    ],
    [{ file: "img/pic.png", data: makePng(48, 64, patternPixels(blue)) }],
  ),
);

// pages that trip naive converters: a windows-1253 page, an <?xml encoding?> page, a '%' in a name, an empty page
// and an empty stylesheet, ToC links with backslashes / other case, ToC entries without a page, a 100-level ToC
{
  const greek = concat([
    bytes(
      `<html><head><meta http-equiv="Content-Type" content="text/html; charset=windows-1253"><title>Greek</title></head><body><h1>Greek page</h1><p>`,
    ),
    Uint8Array.from([0xe1, 0xeb, 0xe5, 0xf0, 0xef, 0xfd]), // "αλεπού" (fox) in windows-1253
    bytes(` means fox.</p></body></html>\n`),
  ]);
  const xmlPage =
    `<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>XML</title></head>` +
    `<body><h1>XML page</h1><p>Un café crème, s'il vous plaît.</p></body></html>\n`;
  const deep: ChmPage[] = Array.from({ length: 100 }, (_, i) => ({
    file: "",
    title: `Level ${i + 1}`,
    body: "",
    level: i,
  }));
  const pages: ChmPage[] = [
    {
      file: "index.html",
      title: "Tricky Home",
      level: 0,
      body: `<h1>Tricky CHM</h1><p>The tricky caracal jumps.</p><p><a href="Sub\\Greek.htm">Greek page</a></p>`,
    },
    { file: "sub/greek.htm", title: "Greek", level: 0, body: "", raw: greek, tocLocal: "Sub\\Greek.HTM" },
    { file: "xml.htm", title: "XML page", level: 0, body: "", raw: bytes(xmlPage), tocLocal: "XML.HTM#top" },
    {
      file: "100%.htm",
      title: "Percent page",
      level: 0,
      body: `<h1>Percent</h1><p>The percentword is here.</p><link rel="stylesheet" href="empty.css">`,
    },
    { file: "empty.htm", title: "Empty page", level: 0, body: "", raw: new Uint8Array(0) },
    ...deep,
  ];
  withTool(
    "tricky chm",
    chmTools,
    chmProject("tricky.chm", "Tricky CHM fixture", pages, [{ file: "empty.css", data: new Uint8Array(0) }]),
  );
}

// many pages, for load time and memory: 25 parts x 40 chapters of ~3 KB each
{
  const pages: ChmPage[] = [];
  const nParts = 25;
  const nChapters = 40;
  const para = (i: number) =>
    `<p>Paragraph ${i}: large CHM fixture text that compresses well but still has to be laid out page by page.</p>`;
  for (let p = 1; p <= nParts; p++) {
    pages.push({ file: `part${p}/index.html`, title: `Part ${p}`, level: 0, body: `<h1>Part ${p}</h1>` });
    for (let c = 1; c <= nChapters; c++) {
      const last = p === nParts && c === nChapters;
      const next = c < nChapters ? `ch${c + 1}.html` : `../part${p + 1}/index.html`;
      pages.push({
        file: `part${p}/ch${c}.html`,
        title: `Chapter ${p}.${c}`,
        level: 1,
        body:
          `<h1>Chapter ${p}.${c}</h1>` +
          Array.from({ length: 25 }, (_, i) => para(i + 1)).join("") +
          (last ? `<p>The last page names the ocelot.</p>` : `<p><a href="${next}">Next</a></p>`),
      });
    }
  }
  withTool("large chm", chmTools, chmProject("large.chm", "Large CHM fixture", pages, []));
}

console.log(`wrote ${written.length} fixtures to ${outDir}`);
for (const n of written) console.log(`  ${n}`);
if (skipped.length > 0) {
  console.log("skipped (kept committed file):");
  for (const s of skipped) console.log(`  ${s}`);
}
