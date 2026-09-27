// Generates the synthetic fixtures in tests/mac/fixtures/ used by tests/mac/run-engine-tests.ts.
//
// Regenerate: bun tests/mac/make-fixtures.ts [--force]
//
// Most files are written by the tiny writers in tests/mac/fixture-lib.ts, deterministically; a file is only
// rewritten when its bytes change. A few need external tools (looked up on PATH, plus FIXTURE_TOOLS_DIR if set),
// whose output isn't reproducible (salts, stamps): an existing tool-made file is kept unless --force is passed,
// and when a tool is missing the committed file is kept:
//   - qpdf (encrypted PDFs), rar (RAR5 CBR), 7z (CB7, password CBZs), c44/cjb2/djvm/djvused (DjVu)
// sample-rar4.cbr is written by makeRar4Store() (RAR 4.x, store method): rar 7.x can't create RAR4.
// On Ubuntu without root the tools can be unpacked from the distro archive:
//   apt-get download qpdf libqpdf29t64 rar 7zip djvulibre-bin libdjvulibre21
//   for d in *.deb; do dpkg -x $d ~/tools; done
//   FIXTURE_TOOLS_DIR=~/tools/usr/lib/7zip:~/tools/usr/bin LD_LIBRARY_PATH=~/tools/usr/lib/x86_64-linux-gnu \
//     bun tests/mac/make-fixtures.ts
// The fixtures committed in 2026-09 were made that way on Ubuntu 24.04 (qpdf 11.9.0, rar 7.00, 7-Zip 23.01,
// DjVuLibre 3.5.28).

import { existsSync, mkdirSync, readFileSync, rmSync, utimesSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { deflateSync } from "node:zlib";
import {
  bytes,
  concat,
  crc32,
  makeBmp,
  makeLit,
  makeMobi,
  makeRar4Store,
  makePalmDoc,
  makePbm,
  makePdf,
  makePng,
  makePpm,
  makeTar,
  makeTga,
  makeTiff,
  makeZip,
  patternPixels,
  PdfWriter,
  pdfStr,
  prng,
  type LitNode,
  type Rgb,
} from "./fixture-lib.ts";

const outDir = join(import.meta.dir, "fixtures");
const repoRoot = join(import.meta.dir, "..", "..");
mkdirSync(outDir, { recursive: true });

const written: string[] = [];
const skipped: string[] = [];

const force = process.argv.includes("--force");
const unchanged: string[] = [];
const kept: string[] = [];

// writes a fixture only when its bytes change, so regenerating doesn't churn the repo
function save(name: string, data: Uint8Array | string) {
  const d = typeof data === "string" ? bytes(data) : data;
  const path = join(outDir, name);
  if (existsSync(path) && Buffer.compare(readFileSync(path), Buffer.from(d)) === 0) {
    unchanged.push(name);
    return;
  }
  writeFileSync(path, d);
  written.push(name);
}

// external tools salt or stamp their output (AES salts, archive headers), so a committed
// tool-made fixture is only rebuilt with --force
function keepToolOutput(name: string): boolean {
  if (force || !existsSync(join(outDir, name))) return false;
  kept.push(name);
  return true;
}

const red: Rgb = [220, 40, 40];
const green: Rgb = [40, 180, 60];
const blue: Rgb = [40, 70, 220];
const smallPng = (c: Rgb) => makePng(48, 64, patternPixels(c));

// ---- PDF ----

const textPdf = makePdf({
  title: "SumatraPDF mac fixture",
  author: "tests/mac",
  pages: [
    { width: 612, height: 792, lines: ["Page one", "This document tests text, outline and links."] },
    { width: 612, height: 792, lines: ["Page two", "The quokka is a small marsupial."] },
    { width: 792, height: 612, lines: ["Page three (landscape)", "Zebrafinch appears only here."] },
  ],
  links: [
    { page: 1, rect: [72, 600, 300, 630], toPage: 3, label: "Go to page three" },
    { page: 1, rect: [72, 540, 300, 570], uri: "https://www.sumatrapdfreader.org/", label: "Visit the website" },
  ],
  outline: [
    { title: "Chapter One", page: 1, children: [{ title: "Section Two", page: 2 }] },
    { title: "Chapter Three", page: 3 },
  ],
});
save("text.pdf", textPdf);
save("truncated.pdf", textPdf.subarray(0, Math.floor(textPdf.length * 0.55)));
save("header-only.pdf", "%PDF-1.7\n");
save("empty.pdf", new Uint8Array(0));
{
  const rnd = prng(0x5eed);
  const g = new Uint8Array(4096);
  for (let i = 0; i < g.length; i++) g[i] = Math.floor(rnd() * 256);
  save("garbage.pdf", g);
}

// Fonts named but not embedded. On macOS they're looked up through CoreText
// (src/mupdf/mupdf_load_system_font_mac.c), most of these as a face inside a .ttc collection; elsewhere mupdf
// substitutes its built-in fonts. Every other font has a /FontDescriptor: mupdf takes a different path for each.
{
  const w = new PdfWriter();
  const catalog = w.alloc();
  const pages = w.alloc();
  const page = w.alloc();
  const names = [
    "HelveticaNeue",
    "AvenirNext-Bold",
    "Menlo-Regular",
    "Futura-Medium",
    "Arial,BoldItalic",
    "GillSans-Italic",
    "Optima-Regular",
    "Palatino-Roman",
    "NoSuchFont-Bold",
  ];
  const fonts = names.map((name, i) => {
    const desc =
      i % 2 === 0
        ? ""
        : ` /FontDescriptor ${w.add(
            `<< /Type /FontDescriptor /FontName /${name} /Flags 32 /FontBBox [0 -200 1000 900] /ItalicAngle 0 ` +
              `/Ascent 900 /Descent -200 /CapHeight 700 /StemV 80 >>`,
          )} 0 R`;
    return w.add(`<< /Type /Font /Subtype /TrueType /BaseFont /${name} /Encoding /WinAnsiEncoding${desc} >>`);
  });
  const text = names.map((name, i) => `BT /F${i} 16 Tf 72 ${720 - i * 30} Td ${pdfStr(`The okapi in ${name}`)} Tj ET`);
  const content = w.add(w.stream("", text.join("\n")));
  const fontRes = fonts.map((f, i) => `/F${i} ${f} 0 R`).join(" ");
  w.set(
    page,
    `<< /Type /Page /Parent ${pages} 0 R /MediaBox [0 0 612 792] /Resources << /Font << ${fontRes} >> >> ` +
      `/Contents ${content} 0 R >>`,
  );
  w.set(pages, `<< /Type /Pages /Kids [${page} 0 R] /Count 1 >>`);
  w.set(catalog, `<< /Type /Catalog /Pages ${pages} 0 R >>`);
  save("system-fonts.pdf", w.build(catalog));
}

// A page with a photo-like image (sky, sun, hills, a house; noisy like a photo) above text, for dark mode: the
// photo must keep its colors (manifest: its region) while the page and the text are recolored.
{
  const iw = 160;
  const ih = 100;
  const rnd = prng(0xf070);
  const px = new Uint8Array(iw * ih * 3);
  const clamp = (v: number) => Math.max(0, Math.min(255, Math.round(v)));
  for (let y = 0; y < ih; y++) {
    for (let x = 0; x < iw; x++) {
      const noise = (rnd() - 0.5) * 30;
      const hill = 58 + 9 * Math.sin(x / 14) + 5 * Math.sin(x / 5);
      let c: number[];
      if (Math.hypot(x - 122, y - 22) < 11) {
        c = [255, 226, 120];
      } else if (x >= 28 && x < 52 && y >= 52 && y < 80) {
        c = y < 62 ? [150, 40, 35] : [205, 180, 140];
      } else if (y < hill) {
        const t = y / hill;
        c = [60 + 110 * t, 120 + 80 * t, 215 + 25 * t];
      } else {
        const t = (y - hill) / (ih - hill);
        c = [55 - 25 * t + 20 * Math.sin(x / 7), 150 - 60 * t, 45 - 15 * t];
      }
      const i = (y * iw + x) * 3;
      px[i] = clamp(c[0]! + noise);
      px[i + 1] = clamp(c[1]! + noise);
      px[i + 2] = clamp(c[2]! + noise);
    }
  }
  const hex = Buffer.from(deflateSync(px)).toString("hex") + ">";
  const w = new PdfWriter();
  const catalog = w.alloc();
  const pages = w.alloc();
  const page = w.alloc();
  const font = w.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  const img = w.add(
    w.stream(
      `/Type /XObject /Subtype /Image /Width ${iw} /Height ${ih} /ColorSpace /DeviceRGB /BitsPerComponent 8 ` +
        `/Filter [/ASCIIHexDecode /FlateDecode]`,
      hex,
    ),
  );
  // the photo covers x 72..540, y 420..720 (PDF units, origin bottom left)
  const text = [
    "q 468 0 0 300 72 420 cm /Im0 Do Q",
    `BT /F1 24 Tf 72 380 Td ${pdfStr("Photo page")} Tj ET`,
    `BT /F1 14 Tf 72 350 Td ${pdfStr("The heron waits in the reeds while the sun sets over the hills.")} Tj ET`,
    `BT /F1 14 Tf 72 326 Td ${pdfStr("Dark mode keeps the photo's colors and recolors this text.")} Tj ET`,
  ];
  const content = w.add(w.stream("", text.join("\n")));
  w.set(
    page,
    `<< /Type /Page /Parent ${pages} 0 R /MediaBox [0 0 612 792] ` +
      `/Resources << /Font << /F1 ${font} 0 R >> /XObject << /Im0 ${img} 0 R >> >> /Contents ${content} 0 R >>`,
  );
  w.set(pages, `<< /Type /Pages /Kids [${page} 0 R] /Count 1 >>`);
  w.set(catalog, `<< /Type /Catalog /Pages ${pages} 0 R >>`);
  save("photo.pdf", w.build(catalog));
}

// ---- EPUB ----

function makeEpub(): Uint8Array {
  const xhtml = (title: string, body: string) =>
    `<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title>${title}</title></head><body>${body}</body></html>\n`;
  const filler = Array.from(
    { length: 12 },
    (_, i) => `<p>Filler paragraph ${i + 1} keeps the chapter long enough.</p>`,
  );
  return makeZip([
    { name: "mimetype", data: "application/epub+zip", store: true },
    {
      name: "META-INF/container.xml",
      data: `<?xml version="1.0"?>\n<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>\n`,
    },
    {
      name: "OEBPS/content.opf",
      data: `<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="id">sumatra-mac-fixture-epub</dc:identifier>
  <dc:title>EPUB mac fixture</dc:title>
  <dc:creator>tests/mac</dc:creator>
  <dc:language>en</dc:language>
 </metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>
  <item id="img" href="img.png" media-type="image/png"/>
 </manifest>
 <spine toc="ncx"><itemref idref="ch1"/><itemref idref="ch2"/></spine>
</package>
`,
    },
    {
      name: "OEBPS/toc.ncx",
      data: `<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>EPUB mac fixture</text></docTitle>
<navMap>
 <navPoint id="n1" playOrder="1"><navLabel><text>Chapter One</text></navLabel><content src="ch1.xhtml"/></navPoint>
 <navPoint id="n2" playOrder="2"><navLabel><text>Chapter Two</text></navLabel><content src="ch2.xhtml#s2"/></navPoint>
</navMap></ncx>
`,
    },
    {
      name: "OEBPS/nav.xhtml",
      data: xhtml(
        "Contents",
        `<nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><ol><li><a href="ch1.xhtml">Chapter One</a></li><li><a href="ch2.xhtml#s2">Chapter Two</a></li></ol></nav>`,
      ),
    },
    {
      name: "OEBPS/ch1.xhtml",
      data: xhtml(
        "Chapter One",
        `<h1>Chapter One</h1><p>The platypus lays eggs.</p>` +
          `<p><a href="ch2.xhtml#s2">Jump to chapter two</a></p>` +
          `<p><a href="https://www.sumatrapdfreader.org/">Visit the website</a></p>` +
          `<p><img src="img.png" alt="red pattern"/></p>` +
          filler.join(""),
      ),
    },
    {
      name: "OEBPS/ch2.xhtml",
      data: xhtml("Chapter Two", `<h1 id="s2">Chapter Two</h1><p>The kiwi cannot fly.</p>` + filler.join("")),
    },
    { name: "OEBPS/img.png", data: smallPng(red), store: true },
  ]);
}
const epub = makeEpub();
save("sample.epub", epub);
save("corrupt.epub", epub.subarray(0, Math.floor(epub.length * 0.6)));
save("empty.epub", new Uint8Array(0));

// Several multi-page chapters: MuPDF lays out only the first one at open, so the page count grows afterwards
// (tests the bridge's page-count updates). The last chapter's word must be found past the initial count.
function makeChaptersEpub(): Uint8Array {
  const words = ["gazelle", "ibex", "lemur", "narwhal"];
  const filler = Array.from(
    { length: 45 },
    (_, i) => `<p>Paragraph ${i + 1} of this chapter fills up another line.</p>`,
  );
  // a link into the last chapter: its target page only exists once that chapter is laid out
  const jump = `<p><a href="ch${words.length}.xhtml#c${words.length}">Jump to the last chapter</a></p>`;
  const chapter = (i: number) =>
    `<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml">` +
    `<head><title>Chapter ${i + 1}</title></head><body><h1 id="c${i + 1}">Chapter ${i + 1}</h1>` +
    `<p>The ${words[i]} appears in chapter ${i + 1}.</p>${i === 0 ? jump : ""}${filler.join("")}</body></html>\n`;
  const ids = words.map((_, i) => `ch${i + 1}`);
  return makeZip([
    { name: "mimetype", data: "application/epub+zip", store: true },
    {
      name: "META-INF/container.xml",
      data: `<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>
`,
    },
    {
      name: "content.opf",
      data:
        `<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">` +
        `<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">chapters</dc:identifier><dc:title>Chapters fixture</dc:title></metadata>` +
        `<manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>` +
        ids.map((id) => `<item id="${id}" href="${id}.xhtml" media-type="application/xhtml+xml"/>`).join("") +
        `</manifest><spine toc="ncx">${ids.map((id) => `<itemref idref="${id}"/>`).join("")}</spine></package>
`,
    },
    {
      name: "toc.ncx",
      data:
        `<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>Chapters</text></docTitle><navMap>` +
        ids
          .map(
            (id, i) =>
              `<navPoint id="n${i + 1}" playOrder="${i + 1}"><navLabel><text>Chapter ${i + 1}</text></navLabel><content src="${id}.xhtml#c${i + 1}"/></navPoint>`,
          )
          .join("") +
        `</navMap></ncx>
`,
    },
    ...ids.map((id, i) => ({ name: `${id}.xhtml`, data: chapter(i) })),
  ]);
}
save("chapters.epub", makeChaptersEpub());

// ---- LIT ----

{
  const page = (title: string, body: LitNode[]): LitNode[] => [
    {
      tag: "html",
      attrs: { xmlns: "http://www.w3.org/1999/xhtml" },
      children: [
        { tag: "head", children: [{ tag: "title", children: [title] }] },
        { tag: "body", children: body },
      ],
    },
  ];
  save(
    "sample.lit",
    makeLit("LIT mac fixture", [
      {
        id: "ch1",
        file: "ch1.html",
        mime: "application/xhtml+xml",
        spine: page("LIT chapter one", [
          { tag: "h1", children: ["LIT Chapter One"] },
          { tag: "p", children: ["The aardvark digs at night."] },
          { tag: "p", children: [{ tag: "a", attrs: { href: "ch2.html#c2" }, children: ["Go to chapter two"] }] },
          { tag: "p", children: [{ tag: "img", attrs: { src: "img.png", alt: "pattern" } }] },
        ]),
      },
      {
        id: "ch2",
        file: "ch2.html",
        mime: "application/xhtml+xml",
        spine: page("LIT chapter two", [
          { tag: "h1", attrs: { id: "c2" }, children: ["LIT Chapter Two"] },
          { tag: "p", children: ["The okapi lives in the forest."] },
        ]),
      },
      { id: "img", file: "img.png", mime: "image/png", data: smallPng(green) },
    ]),
  );
}

// ---- FB2 / FBZ ----

const fb2 = `<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
 <description>
  <title-info>
   <genre>prose</genre>
   <author><first-name>Tests</first-name><last-name>Mac</last-name></author>
   <book-title>FB2 mac fixture</book-title>
   <lang>en</lang>
  </title-info>
 </description>
 <body>
  <section><title><p>Chapter One</p></title>
   <p>The echidna is a spiny anteater.</p>
   <image l:href="#pic1"/>
  </section>
  <section><title><p>Chapter Two</p></title>
   <p>The dingo is a wild dog.</p>
  </section>
 </body>
 <binary id="pic1" content-type="image/png">${Buffer.from(smallPng(green)).toString("base64")}</binary>
</FictionBook>
`;
save("sample.fb2", fb2);
save("sample.fbz", makeZip([{ name: "sample.fb2", data: fb2 }]));

// ---- MOBI / AZW / PalmDoc ----

{
  const para = (s: string) => `<p>${s}</p>`;
  const html =
    `<html><head><guide></guide></head><body>` +
    `<h1>Mobi Chapter One</h1>` +
    para("The numbat eats termites.") +
    `<p><img recindex="00001"/></p>` +
    `<mbp:pagebreak/>` +
    `<h1>Mobi Chapter Two</h1>` +
    Array.from({ length: 150 }, (_, i) =>
      para(`Line ${i + 1}: a long paragraph so the text spans several records.`),
    ).join("") +
    `</body></html>`;
  save("sample.mobi", makeMobi("MOBI mac fixture", html, [smallPng(blue)], true));
  const html2 = `<html><body><h1>Uncompressed</h1><p>The bilby has big ears.</p></body></html>`;
  save("uncompressed.azw", makeMobi("AZW mac fixture", html2, [], false));
  const bad = makeMobi("corrupt", html2, [], false);
  const rnd = prng(42);
  for (let i = 90; i < bad.length; i++) bad[i] = Math.floor(rnd() * 256);
  save("corrupt.mobi", bad);
}
save(
  "sample.pdb",
  makePalmDoc(
    "PalmDoc mac fixture",
    "PalmDoc fixture\n\nThe wallaby hops.\n" + "Some more text to compress. ".repeat(40),
    true,
  ),
);

// ---- XPS ----

function makeXps(): Uint8Array {
  const ns = "http://schemas.microsoft.com/xps/2005/06";
  const font = readFileSync(join(repoRoot, "ext/mupdf/resources/fonts/urw/NimbusSans-Regular.cff"));
  const page = (n: number, word: string, extra: string) =>
    `<FixedPage xmlns="${ns}" Width="816" Height="1056" xml:lang="en-US">
 <Path Data="M 0,0 L 816,0 816,1056 0,1056 Z" Fill="#FFFFFFFF"/>
 <Path Data="M 96,300 L 720,300 720,340 96,340 Z" Fill="#FF3366CC"/>
 <Glyphs FontUri="/Resources/NimbusSans-Regular.cff" FontRenderingEmSize="32" OriginX="96" OriginY="140" Fill="#FF000000" UnicodeString="XPS page ${n}"/>
 <Glyphs FontUri="/Resources/NimbusSans-Regular.cff" FontRenderingEmSize="20" OriginX="96" OriginY="200" Fill="#FF000000" UnicodeString="${word}"/>
${extra}</FixedPage>
`;
  const link = (uri: string, y: number, label: string) =>
    ` <Path FixedPage.NavigateUri="${uri}" Data="M 96,${y} L 500,${y} 500,${y + 36} 96,${y + 36} Z" Fill="#FFDDEEFF"/>
 <Glyphs FontUri="/Resources/NimbusSans-Regular.cff" FontRenderingEmSize="18" OriginX="100" OriginY="${y + 26}" Fill="#FF0000CC" UnicodeString="${label}"/>
`;
  return makeZip([
    {
      name: "[Content_Types].xml",
      data: `<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="fdseq" ContentType="application/vnd.ms-package.xps-fixeddocumentsequence+xml"/>
 <Default Extension="fdoc" ContentType="application/vnd.ms-package.xps-fixeddocument+xml"/>
 <Default Extension="fpage" ContentType="application/vnd.ms-package.xps-fixedpage+xml"/>
 <Default Extension="struct" ContentType="application/vnd.ms-package.xps-documentstructure+xml"/>
 <Default Extension="cff" ContentType="application/vnd.ms-opentype"/>
</Types>
`,
    },
    {
      name: "_rels/.rels",
      data: `<?xml version="1.0" encoding="utf-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Id="R0" Type="http://schemas.microsoft.com/xps/2005/06/fixedrepresentation" Target="/FixedDocumentSequence.fdseq"/>
</Relationships>
`,
    },
    {
      name: "FixedDocumentSequence.fdseq",
      data: `<FixedDocumentSequence xmlns="${ns}"><DocumentReference Source="/Documents/1/FixedDocument.fdoc"/></FixedDocumentSequence>\n`,
    },
    {
      name: "Documents/1/FixedDocument.fdoc",
      data: `<FixedDocument xmlns="${ns}">
 <PageContent Source="Pages/1.fpage"><PageContent.LinkTargets><LinkTarget Name="p1"/></PageContent.LinkTargets></PageContent>
 <PageContent Source="Pages/2.fpage"><PageContent.LinkTargets><LinkTarget Name="p2"/></PageContent.LinkTargets></PageContent>
</FixedDocument>
`,
    },
    {
      name: "Documents/1/_rels/FixedDocument.fdoc.rels",
      data: `<?xml version="1.0" encoding="utf-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Id="R1" Type="http://schemas.microsoft.com/xps/2005/06/documentstructure" Target="Structure/DocStructure.struct"/>
</Relationships>
`,
    },
    {
      name: "Documents/1/Structure/DocStructure.struct",
      data: `<DocumentStructure xmlns="http://schemas.microsoft.com/xps/2005/06/documentstructure">
 <DocumentStructure.Outline><DocumentOutline xml:lang="en-US">
  <OutlineEntry OutlineLevel="1" OutlineTarget="../FixedDocument.fdoc#p1" Description="First XPS page"/>
  <OutlineEntry OutlineLevel="1" OutlineTarget="../FixedDocument.fdoc#p2" Description="Second XPS page"/>
 </DocumentOutline></DocumentStructure.Outline>
</DocumentStructure>
`,
    },
    {
      name: "Documents/1/Pages/1.fpage",
      data: page(
        1,
        "The kookaburra laughs.",
        link("#p2", 400, "Go to page two") + link("https://www.sumatrapdfreader.org/", 460, "Visit the website"),
      ),
    },
    { name: "Documents/1/Pages/2.fpage", data: page(2, "The cassowary is shy.", "") },
    { name: "Resources/NimbusSans-Regular.cff", data: new Uint8Array(font) },
  ]);
}
save("sample.xps", makeXps());
save("corrupt.xps", concat([bytes("PK\x03\x04"), new Uint8Array(200).fill(0x41)]));

// ---- images ----

save("sample.png", makePng(120, 160, patternPixels(red)));
save("sample.bmp", makeBmp(64, 80, patternPixels(green)));
save("sample.tga", makeTga(64, 80, patternPixels(blue)));
save(
  "sample.tif",
  makeTiff([
    { w: 60, h: 80, px: patternPixels(red) },
    { w: 60, h: 80, px: patternPixels(blue) },
  ]),
);

// ---- comic book archives ----

const comicPages = [
  { name: "page01.png", data: smallPng(red) },
  { name: "page02.png", data: smallPng(green) },
  { name: "page03.png", data: smallPng(blue) },
];
save("sample.cbz", makeZip(comicPages.map((p) => ({ ...p, store: true }))));
save("sample.cbt", makeTar(comicPages));
save("corrupt.cbz", concat([bytes("PK\x03\x04"), new Uint8Array(300).fill(0x5a)]));
save("empty.cbz", new Uint8Array(0));
save("sample-rar4.cbr", makeRar4Store(comicPages));

// pages whose PNG headers claim 48 x 2130706432 px: summing their heights must not overflow the layout
function hugeClaimPng(): Uint8Array {
  const png = smallPng(red).slice();
  const dv = new DataView(png.buffer);
  dv.setUint32(20, 0x7f000000); // IHDR height
  dv.setUint32(29, crc32(png.subarray(12, 29))); // IHDR crc (type + data)
  return png;
}
save(
  "huge-pages.cbz",
  makeZip(
    Array.from({ length: 200 }, (_, i) => ({
      name: `p${String(i).padStart(3, "0")}.png`,
      data: hugeClaimPng(),
      store: true,
    })),
  ),
);
save("corrupt.cbr", concat([bytes("Rar!\x1a\x07\x00"), new Uint8Array(300).fill(0x33)]));

// ---- text-ish formats rendered by mupdf ----

save("sample.txt", "Plain text fixture\n\nThe bandicoot digs.\n");
save("sample.md", "# Markdown fixture\n\nThe **cassowary** runs.\n\n## Second heading\n\n- item one\n- item two\n");
save(
  "sample.html",
  `<!DOCTYPE html><html><head><title>HTML fixture</title></head><body><h1>HTML fixture</h1><p>The dugong grazes.</p><p><a href="https://www.sumatrapdfreader.org/">Visit</a></p></body></html>\n`,
);
// Characters the default fonts lack, in every style: each needs a fallback font per script, which on macOS comes
// from CoreText (src/mupdf/mupdf_load_system_font_mac.c; most of them are faces inside a .ttc), plus families
// named by the stylesheet
{
  const mixed = "Ελληνικά Русский עברית العربية हिन्दी ไทย 中文 日本語 ひらがな 한국어 ✓ → ☃ � \u{1F600}";
  const styles = ["", "font-family: sans-serif", "font-family: serif", "font-family: monospace"];
  const families = ["Helvetica Neue", "Menlo", "Avenir Next", "Optima", "No Such Font"];
  const paras = [
    ...styles.flatMap((s) =>
      ["", "b", "i"].map((tag) => {
        const t = tag ? `<${tag}>${mixed}</${tag}>` : mixed;
        return `<p style="${s}">${t}</p>`;
      }),
    ),
    ...families.map((f) => `<p style="font-family: '${f}'"><b>${f}</b>: the okapi ${mixed}</p>`),
  ];
  save(
    "scripts.html",
    `<!DOCTYPE html><html><head><meta charset="utf-8"><title>Scripts fixture</title></head><body>` +
      `<h1>Scripts fixture</h1><p>The okapi hides.</p>${paras.join("")}</body></html>\n`,
  );
}
save(
  "sample.svg",
  `<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300"><rect width="400" height="300" fill="#fff"/><circle cx="200" cy="150" r="100" fill="#c33"/><text x="40" y="40" font-size="24">koala</text></svg>\n`,
);
save(
  "sample.ps",
  "%!PS-Adobe-3.0\n/Helvetica findfont 24 scalefont setfont 72 700 moveto (PostScript fixture) show showpage\n",
);

// ---- fixtures made with external tools ----

const toolDirs = [...(process.env.FIXTURE_TOOLS_DIR ?? "").split(":"), ...(process.env.PATH ?? "").split(":")].filter(
  Boolean,
);

function findTool(name: string): string | null {
  for (const d of toolDirs) {
    const p = join(d, name);
    if (existsSync(p)) return p;
  }
  return null;
}

function run(args: string[], cwd: string, input?: string): boolean {
  const r = Bun.spawnSync(args, {
    cwd,
    stdin: input !== undefined ? bytes(input) : "ignore",
    stdout: "pipe",
    stderr: "pipe",
  });
  if (r.exitCode !== 0) {
    console.error(
      `  ${args.join(" ")} failed (${r.exitCode}): ${r.stderr.toString().trim()} ${r.stdout.toString().trim()}`,
    );
    return false;
  }
  return true;
}

function withTools(what: string, names: string[], fn: (tools: Record<string, string>, work: string) => void) {
  const tools: Record<string, string> = {};
  for (const n of names) {
    const p = findTool(n);
    if (!p) {
      skipped.push(`${what} (missing ${n})`);
      return;
    }
    tools[n] = p;
  }
  const work = join(tmpdir(), `sumatra-mac-fixtures-${what.replace(/\W+/g, "-")}`);
  rmSync(work, { recursive: true, force: true });
  mkdirSync(work, { recursive: true });
  try {
    fn(tools, work);
  } finally {
    rmSync(work, { recursive: true, force: true });
  }
}

const fixedTime = new Date("2026-01-01T00:00:00Z");

function writeComicPages(work: string): string[] {
  const names: string[] = [];
  for (const p of comicPages) {
    const f = join(work, p.name);
    writeFileSync(f, p.data);
    utimesSync(f, fixedTime, fixedTime);
    names.push(p.name);
  }
  return names;
}

withTools("encrypted pdf", ["qpdf"], (t, work) => {
  const src = join(work, "in.pdf");
  writeFileSync(src, textPdf);
  const variants: [string, string][] = [
    ["encrypted-aes128.pdf", "128"],
    ["encrypted-aes256.pdf", "256"],
  ];
  for (const [name, bits] of variants) {
    if (keepToolOutput(name)) continue;
    const aes = bits === "128" ? ["--use-aes=y"] : [];
    const dst = join(work, name);
    const args = [
      t.qpdf!,
      "--static-id",
      "--static-aes-iv",
      "--encrypt",
      "sumatra",
      "owner-pw",
      bits,
      ...aes,
      "--",
      src,
      dst,
    ];
    if (run(args, work)) save(name, readFileSync(dst));
  }
});

withTools("cbr", ["rar"], (t, work) => {
  const names = writeComicPages(work);
  const variants: [string, string[]][] = [
    ["sample-rar5.cbr", ["-ma5"]],
    ["password.cbr", ["-ma5", "-psumatra"]],
  ];
  for (const [name, opts] of variants) {
    if (keepToolOutput(name)) continue;
    const dst = join(work, name);
    if (run([t.rar!, "a", "-idq", "-ep", "-tl", ...opts, dst, ...names], work)) save(name, readFileSync(dst));
  }
});

withTools("7z", ["7z"], (t, work) => {
  const names = writeComicPages(work);
  const variants: [string, string[]][] = [
    ["sample.cb7", ["-t7z", "-mtm=off", "-mtc=off", "-mta=off"]],
    // password-protected comic zips: traditional PKWARE encryption, and WinZip AES
    ["password-zipcrypto.cbz", ["-tzip", "-psumatra", "-mem=ZipCrypto"]],
    ["password-aes.cbz", ["-tzip", "-psumatra", "-mem=AES256"]],
  ];
  for (const [name, opts] of variants) {
    if (keepToolOutput(name)) continue;
    const dst = join(work, name);
    if (run([t["7z"]!, "a", "-bd", ...opts, dst, ...names], work)) save(name, readFileSync(dst));
  }
});

withTools("djvu", ["c44", "cjb2", "djvm", "djvused"], (t, work) => {
  if (keepToolOutput("sample.djvu")) return;
  const w = 600;
  const h = 800;
  // page 1: color photo-like image (IW44), page 2: bitonal (JB2)
  const gradient = (x: number, y: number): Rgb => [Math.floor((x * 255) / w), Math.floor((y * 255) / h), 140];
  writeFileSync(join(work, "p1.ppm"), makePpm(w, h, gradient));
  writeFileSync(
    join(work, "p2.pbm"),
    makePbm(w, h, (x, y) => x < 6 || y < 6 || x >= w - 6 || y >= h - 6 || (y > 100 && y < 130 && x > 60 && x < 540)),
  );
  const ok =
    run([t.c44!, "-dpi", "100", "p1.ppm", "p1.djvu"], work) &&
    run([t.cjb2!, "-dpi", "100", "p2.pbm", "p2.djvu"], work) &&
    run([t.djvm!, "-c", "sample.djvu", "p1.djvu", "p2.djvu"], work);
  if (!ok) return;
  // hidden text layer (for search) + outline; DjVu coordinates have a bottom-left origin
  const script = `select 1
set-txt
(page 0 0 ${w} ${h} (line 60 680 540 720 (word 60 680 260 720 "Wombat") (word 280 680 540 720 "burrows")))
.
select 2
set-txt
(page 0 0 ${w} ${h} (line 60 670 540 700 (word 60 670 300 700 "Second") (word 320 670 540 700 "page")))
.
select
set-outline
(bookmarks ("First DjVu page" "#1") ("Second DjVu page" "#2"))
.
set-meta
title "DjVu mac fixture"
.
save
`;
  if (!run([t.djvused!, "sample.djvu", "-f", "/dev/stdin"], work, script)) return;
  save("sample.djvu", readFileSync(join(work, "sample.djvu")));
});
save("corrupt.djvu", concat([bytes("AT&TFORM\x00\x00\x10\x00DJVU"), new Uint8Array(400).fill(0x11)]));

console.log(`wrote ${written.length} fixtures to ${outDir}, ${unchanged.length} unchanged`);
for (const n of written) console.log(`  ${n}`);
if (kept.length > 0) console.log(`kept tool-made fixtures (--force rebuilds them): ${kept.join(", ")}`);
if (skipped.length > 0) {
  console.log("skipped (kept committed file):");
  for (const s of skipped) console.log(`  ${s}`);
}
