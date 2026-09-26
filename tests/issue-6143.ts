// issue #6143: MuPDF markdown (UseFixedPageUI, no WebView) crashed in
// fz_md_to_html on an empty .md: after fz_terminate_buffer it did
// len = buf->len-1, which underflows to SIZE_MAX when len is 0 and then
// walks off the buffer. Same path as a remote/other-drive file that
// reads as 0 bytes, and as a name with spaces (the report).

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { withControlledSumatra } from "./control.ts";
import { EXE, runStandalone, tmpPath } from "./util.ts";

function settings(): string {
  return ["MarkdownUI [", "\tUseFixedPageUI = true", "]", "RestoreSession = false", "ShowStartPage = false", ""].join(
    "\n",
  );
}

async function openMd(path: string, appdata: string, label: string): Promise<void> {
  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.waitForRenderIdle();
      const info = await client.chapterInfo();
      if (info.pageCount < 1) {
        throw new Error(`issue-6143: ${label}: expected at least 1 page, got ${info.pageCount}`);
      }
    },
    ["-appdata", appdata, path],
  );
}

export async function testit(): Promise<void> {
  const dir = tmpPath("issue-6143-data");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });

  const appdata = tmpPath("issue-6143-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settings());

  const empty = join(dir, "empty.md");
  writeFileSync(empty, "");
  await openMd(empty, appdata, "empty.md");

  const spaced = join(dir, "remote file.md");
  writeFileSync(spaced, "# Hello\n\nspaces in the name\n");
  await openMd(spaced, appdata, "'remote file.md'");

  const nuls = join(dir, "embedded nuls.md");
  writeFileSync(nuls, Buffer.from("a\0b\0c"));
  await openMd(nuls, appdata, "embedded NULs");

  console.log("issue-6143: OK");
}

if (import.meta.main) {
  await runStandalone(testit);
}
