import { readdirSync, rmSync, existsSync } from "node:fs";
import { join } from "node:path";

function getThumbCacheDirs(): string[] {
  const localAppData = process.env.LOCALAPPDATA;
  if (!localAppData) {
    console.log("LOCALAPPDATA not set");
    return [];
  }
  const explorerDir = join(localAppData, "Microsoft", "Windows", "Explorer");
  if (!existsSync(explorerDir)) {
    console.log(`directory not found: ${explorerDir}`);
    return [];
  }
  return [explorerDir];
}

function clearThumbnailCache(): void {
  const dirs = getThumbCacheDirs();
  if (dirs.length === 0) {
    return;
  }
  let nDeleted = 0;
  for (const dir of dirs) {
    const entries = readdirSync(dir);
    for (const name of entries) {
      // thumbnail cache files: thumbcache_*.db, iconcache_*.db, thumbcache_*.db
      const lower = name.toLowerCase();
      if (lower.startsWith("thumbcache_") && lower.endsWith(".db")) {
        const fullPath = join(dir, name);
        try {
          rmSync(fullPath, { force: true });
          console.log(`deleted: ${fullPath}`);
          nDeleted++;
        } catch (e) {
          console.log(`failed to delete: ${fullPath} - ${e}`);
        }
      }
    }
  }
  if (nDeleted === 0) {
    console.log("no thumbnail cache files found");
  } else {
    console.log(`deleted ${nDeleted} thumbnail cache files`);
  }
}

if (import.meta.main) {
  clearThumbnailCache();
}
