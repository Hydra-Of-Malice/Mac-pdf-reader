import { readdir, readFile } from "node:fs/promises";
import { join, extname, basename, dirname } from "node:path";

const allowedExts = new Set([".go", ".cpp", ".h"]);
const excludedDirs = new Set(["ext"]);

function isAllowed(path: string): boolean {
  const ext = extname(path).toLowerCase();
  if (!allowedExts.has(ext)) return false;
  // check if any parent directory is excluded
  let dir = dirname(path);
  while (dir !== ".") {
    if (excludedDirs.has(basename(dir))) return false;
    dir = dirname(dir);
  }
  return true;
}

interface FileLineCount {
  path: string;
  ext: string;
  lines: number;
}

async function countLines(path: string): Promise<number> {
  const content = await readFile(path);
  if (content.length === 0) return 0;
  let n = 1;
  for (const b of content) {
    if (b === 10) n++;
  }
  return n;
}

async function scanDir(dir: string, results: FileLineCount[]): Promise<void> {
  const entries = await readdir(dir, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = join(dir, entry.name);
    if (entry.isDirectory()) {
      await scanDir(fullPath, results);
    } else if (entry.isFile()) {
      if (!isAllowed(fullPath)) continue;
      const lines = await countLines(fullPath);
      results.push({
        path: fullPath,
        ext: extname(entry.name).toLowerCase(),
        lines,
      });
    }
  }
}

async function main() {
  const results: FileLineCount[] = [];
  await scanDir("src", results);

  // sort by path
  results.sort((a, b) => a.path.localeCompare(b.path));

  // print per-file
  let total = 0;
  for (const f of results) {
    console.log(`${f.lines.toString().padStart(6)} ${f.path}`);
    total += f.lines;
  }

  // per-extension summary
  const extMap = new Map<string, number>();
  for (const f of results) {
    extMap.set(f.ext, (extMap.get(f.ext) || 0) + f.lines);
  }
  const extEntries = [...extMap.entries()].sort((a, b) => a[1] - b[1]);

  console.log("\nPer extension:");
  for (const [ext, lines] of extEntries) {
    console.log(`${lines} ${ext}`);
  }
  console.log(`\ntotal: ${total}`);
}

await main();
