// #6127: save a rectangular page area as an image at a chosen DPI, independent
// of the current zoom / viewport.
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand, withControlledSumatra } from "./control.ts";
import { EXE, runStandalone, tmpPath } from "./util.ts";

const SETTINGS = `UiLanguage = en
CheckForUpdates = false
RestoreSession = false
RememberOpenedFiles = false
RememberStatePerDocument = false
`;

function makeLetterPdf(): Buffer {
  const objs = [
    `<< /Type /Catalog /Pages 2 0 R >>`,
    `<< /Type /Pages /Kids [3 0 R] /Count 1 >>`,
    `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>`,
  ];
  let pdf = "%PDF-1.7\n%\xe2\xe3\xcf\xd3\n";
  const off: number[] = [];
  for (let i = 0; i < objs.length; i++) {
    off[i] = Buffer.byteLength(pdf, "latin1");
    pdf += `${i + 1} 0 obj\n${objs[i]}\nendobj\n`;
  }
  const xref = Buffer.byteLength(pdf, "latin1");
  const n = objs.length + 1;
  pdf += `xref\n0 ${n}\n0000000000 65535 f \n`;
  for (let i = 0; i < objs.length; i++) {
    pdf += off[i]!.toString().padStart(10, "0") + " 00000 n \n";
  }
  pdf += `trailer\n<< /Size ${n} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return Buffer.from(pdf, "latin1");
}

function pngSize(buf: Buffer): { w: number; h: number } {
  if (buf.length < 24 || buf[0] !== 0x89 || buf[1] !== 0x50 || buf[2] !== 0x4e || buf[3] !== 0x47) {
    throw new Error(`issue-6127: not a PNG (${buf.length} bytes)`);
  }
  return { w: buf.readUInt32BE(16), h: buf.readUInt32BE(20) };
}

async function saveSel(
  client: ControlClient,
  dest: string,
  dpi: number,
  page: number,
  x: number,
  y: number,
  dx: number,
  dy: number,
): Promise<string> {
  const deadline = Date.now() + 30_000;
  let last = "";
  for (;;) {
    const res = await client.request(ControlCommand.TestSaveSelectionAsImage, [dest, dpi, page, x, y, dx, dy]);
    const raw = String(res[1] ?? "").trim();
    last = raw;
    if (res[0] === 0 && raw.startsWith("OK")) {
      return raw;
    }
    if (res[0] !== 0 && !raw.includes("NOTREADY")) {
      throw new Error(`issue-6127: save failed: ${raw}`);
    }
    if (Date.now() > deadline) {
      throw new Error(`issue-6127: save timed out: ${last}`);
    }
    await new Promise((r) => setTimeout(r, 50));
  }
}

function expectSize(path: string, wantW: number, wantH: number) {
  if (!existsSync(path)) {
    throw new Error(`issue-6127: missing ${path}`);
  }
  const s = pngSize(readFileSync(path));
  if (Math.abs(s.w - wantW) > 2 || Math.abs(s.h - wantH) > 2) {
    throw new Error(`issue-6127: ${path} is ${s.w}x${s.h}, want ~${wantW}x${wantH}`);
  }
}

export async function testit(): Promise<void> {
  const dir = tmpPath("issue-6127");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, "SumatraPDF-settings.txt"), SETTINGS);
  const pdf = join(dir, "page.pdf");
  writeFileSync(pdf, makeLetterPdf());

  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.waitForRenderIdle();

      // 72x72 pt = 1 inch. 150 DPI → 150x150; 300 DPI → 300x300.
      const p150 = join(dir, "sel-150.png");
      await saveSel(client, p150, 150, 1, 0, 0, 72, 72);
      expectSize(p150, 150, 150);

      const p300 = join(dir, "sel-300.png");
      await saveSel(client, p300, 300, 1, 0, 0, 72, 72);
      expectSize(p300, 300, 300);

      const jpg = join(dir, "sel.jpg");
      await saveSel(client, jpg, 150, 1, 0, 0, 72, 72);
      if (!existsSync(jpg)) {
        throw new Error("issue-6127: jpeg save did not write sel.jpg");
      }
      const jpgBuf = readFileSync(jpg);
      if (jpgBuf.length < 2 || jpgBuf[0] !== 0xff || jpgBuf[1] !== 0xd8) {
        throw new Error(`issue-6127: sel.jpg is not a JPEG (${jpgBuf.length} bytes)`);
      }
    },
    ["-appdata", dir, pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
