// Test for "Trim Empty Margins" (CmdToggleTrimEmptyMargins)
import { writeFileSync } from "node:fs";
import { ControlCommand, withControlledSumatra } from "./control.ts";
import { EXE, runStandalone, tmpPath } from "./util.ts";

function makePdfWithMargins(): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const body: Record<number, Buffer> = {};
  body[1] = enc("<< /Type /Catalog /Pages 2 0 R >>");
  body[2] = enc("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  body[4] = enc("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

  // MediaBox is 400x400. Text is in a small box in the center (180, 200)
  const stream = "BT /F1 12 Tf 180 200 Td (Hello World) Tj ET";
  body[5] = enc(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  body[3] = enc(
    `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>`,
  );

  let offset = 0;
  const parts: Buffer[] = [enc("%PDF-1.4\n")];
  offset += parts[0]!.length;

  const xref: number[] = [0];
  for (let i = 1; i <= 5; i++) {
    xref[i] = offset;
    const chunk = Buffer.concat([enc(`${i} 0 obj\n`), body[i]!, enc("\nendobj\n")]);
    parts.push(chunk);
    offset += chunk.length;
  }

  const xrefStart = offset;
  let xrefStr = `xref\n0 6\n0000000000 65535 f \n`;
  for (let i = 1; i <= 5; i++) {
    xrefStr += `${xref[i]!.toString().padStart(10, "0")} 00000 n \n`;
  }
  xrefStr += `trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n${xrefStart}\n%%EOF\n`;
  parts.push(enc(xrefStr));

  return Buffer.concat(parts);
}

type PagePos = { n: number; y: number; dy: number; sy: number; sdy: number; sx: number; sdx: number };

function parsePages(raw: string): PagePos[] {
  const pages: PagePos[] = [];
  for (const line of raw.split("\n")) {
    const m =
      /^page n=(\d+) shown=\d+ pos=(-?\d+),(-?\d+),(-?\d+),(-?\d+) screen=(-?\d+),(-?\d+),(-?\d+),(-?\d+)$/.exec(
        line.trim(),
      );
    if (m) {
      pages.push({
        n: +m[1]!,
        y: +m[3]!,
        dy: +m[5]!,
        sx: +m[6]!,
        sy: +m[7]!,
        sdx: +m[8]!,
        sdy: +m[9]!,
      });
    }
  }
  return pages;
}

export async function testit(): Promise<void> {
  const pdf = tmpPath("trim-margins-test.pdf");
  writeFileSync(pdf, makePdfWithMargins());

  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.waitForRenderIdle();

      const [, rawBefore] = await client.request(ControlCommand.TestLayout, ["get"]);
      const pagesBefore = parsePages(String(rawBefore ?? ""));
      if (pagesBefore.length === 0) {
        throw new Error("Failed to get initial layout");
      }
      const initialDy = pagesBefore[0]!.dy;

      // Toggle Trim Empty Margins ON
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleTrimEmptyMargins"]);
      await client.waitForRenderIdle();

      const [, rawAfter] = await client.request(ControlCommand.TestLayout, ["get"]);
      const pagesAfter = parsePages(String(rawAfter ?? ""));
      if (pagesAfter.length === 0) {
        throw new Error("Failed to get trimmed layout");
      }
      const trimmedDy = pagesAfter[0]!.dy;

      // The trimmed page height should be smaller than initial 400 pt MediaBox layout
      if (trimmedDy >= initialDy) {
        throw new Error(`Expected trimmedDy (${trimmedDy}) to be smaller than initialDy (${initialDy})`);
      }

      // Toggle Trim Empty Margins OFF again
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleTrimEmptyMargins"]);
      await client.waitForRenderIdle();

      const [, rawUntrimmed] = await client.request(ControlCommand.TestLayout, ["get"]);
      const pagesUntrimmed = parsePages(String(rawUntrimmed ?? ""));
      const untrimmedDy = pagesUntrimmed[0]!.dy;

      if (untrimmedDy !== initialDy) {
        throw new Error(`Expected untrimmedDy (${untrimmedDy}) to restore to initialDy (${initialDy})`);
      }
    },
    [pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
