/**
 * SumatraPDF.app bundle resources (Info.plist, icon, license texts, source offer) and the distribution
 * packages (.tar.gz, .zip, .dmg).
 *
 * Used by cmd/helper/mac-build.ts (cmd/build.ts -mac). See docs/mac/BUILDING.md and
 * docs/mac/THIRD-PARTY-LICENSES.md.
 */

import {
  copyFileSync,
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, join } from "node:path";
import { tmpdir } from "node:os";
import { extractSumatraVersion } from "../util";

const resourcesDir = join("src", "mac", "Resources");
const infoPlistTemplate = join(resourcesDir, "Info.plist");
const appIcon = join(resourcesDir, "SumatraPDF.icns");
const extraLicensesDir = join(resourcesDir, "Licenses");
const archiveReadme = join(resourcesDir, "package-README.md"); // README.md in the packages, not in the bundle
const upstreamRepo = "https://github.com/sumatrapdfreader/sumatrapdf";
const buildNoBase = 1000; // same numbering as `bun cmd/build.ts -build-no`

export interface SourceInfo {
  version: string; // CURR_VERSION, e.g. "3.7"
  bundleVersion: string; // CFBundleVersion, e.g. "3.7.22488"
  repoUrl: string;
  commit?: string;
  modified: boolean; // working tree differs from commit
}

interface Component {
  name: string;
  license: string; // SPDX identifier where one exists
  dir: string; // sub-directory of Contents/Resources/Licenses
  files: string[]; // license texts in the repo
  note?: string;
}

const mupdfFonts: Component[] = [
  {
    name: "URW base fonts (MuPDF built-in fonts)",
    license: "OFL-1.1",
    dir: "fonts-urw",
    files: ["ext/mupdf/resources/fonts/urw/OFL.txt"],
  },
  {
    name: "Charis SIL (MuPDF built-in fonts)",
    license: "OFL-1.1",
    dir: "fonts-sil",
    files: ["ext/mupdf/resources/fonts/sil/OFL.txt"],
  },
  {
    name: "Noto fonts (MuPDF built-in fonts)",
    license: "OFL-1.1",
    dir: "fonts-noto",
    files: ["ext/mupdf/resources/fonts/noto/COPYING"],
  },
  {
    name: "Droid Sans Fallback (MuPDF built-in fonts)",
    license: "Apache-2.0",
    dir: "fonts-droid",
    files: ["ext/mupdf/resources/fonts/droid/NOTICE"],
  },
];

// Keyed by normalized static library name (see libKey()). Every library linked into the app must be listed:
// addBundleResources() fails the build for an unknown one.
const libComponents: Record<string, Component[]> = {
  base: [], // src/base: BSD, covered by COPYING.BSD
  mupdf: [{ name: "MuPDF", license: "AGPL-3.0-or-later", dir: "mupdf", files: ["ext/mupdf/COPYING"] }, ...mupdfFonts],
  extract: [
    {
      name: "extract",
      license: "AGPL-3.0-or-later",
      dir: "extract",
      files: ["ext/mupdf/COPYING"],
      note: "upstream COPYING is the same AGPL v3 text as MuPDF's",
    },
  ],
  jbig2dec: [
    {
      name: "jbig2dec",
      license: "AGPL-3.0-or-later",
      dir: "jbig2dec",
      files: ["ext/a-jbig2dec/COPYING", "ext/a-jbig2dec/LICENSE"],
    },
  ],
  mujs: [{ name: "MuJS", license: "ISC", dir: "mujs", files: ["ext/a-mujs/COPYING"] }],
  freetype: [
    {
      name: "FreeType",
      license: "FTL",
      dir: "freetype",
      files: ["ext/a-freetype/LICENSE.TXT", "ext/a-freetype/docs/FTL.TXT"],
      note: "Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.",
    },
  ],
  harfbuzz: [{ name: "HarfBuzz", license: "MIT-Modern-Variant", dir: "harfbuzz", files: ["ext/a-harfbuzz/COPYING"] }],
  lcms2: [{ name: "Little CMS (lcms2mt)", license: "MIT", dir: "lcms2", files: ["ext/a-lcms2/LICENSE"] }],
  openjpeg: [{ name: "OpenJPEG", license: "BSD-2-Clause", dir: "openjpeg", files: ["ext/a-openjpeg/LICENSE"] }],
  "libjpeg-turbo": [
    {
      name: "libjpeg-turbo",
      license: "IJG AND BSD-3-Clause AND Zlib",
      dir: "libjpeg-turbo",
      files: ["ext/libjpeg-turbo/LICENSE.md", "ext/libjpeg-turbo/README.ijg"],
      note: "This software is based in part on the work of the Independent JPEG Group.",
    },
  ],
  libwebp: [{ name: "libwebp", license: "BSD-3-Clause", dir: "libwebp", files: ["ext/a-libwebp/COPYING"] }],
  brotli: [{ name: "Brotli", license: "MIT", dir: "brotli", files: ["ext/a-brotli/LICENSE"] }],
  gumbo: [
    {
      name: "Gumbo HTML parser",
      license: "Apache-2.0",
      dir: "gumbo",
      files: [join(extraLicensesDir, "gumbo", "COPYING")],
      note: "license text from the upstream repository; ext/a-gumbo has none",
    },
  ],
  "cmark-gfm": [
    { name: "cmark-gfm", license: "BSD-2-Clause AND MIT", dir: "cmark-gfm", files: ["ext/cmark-gfm/COPYING"] },
  ],
  zlib: [{ name: "zlib", license: "Zlib", dir: "zlib", files: ["ext/a-zlib/LICENSE"] }],
  libarchive: [
    { name: "libarchive", license: "BSD-2-Clause", dir: "libarchive", files: ["ext/a-libarchive/COPYING"] },
    { name: "bzip2", license: "bzip2-1.0.6", dir: "bzip2", files: ["ext/a-bzip2/LICENSE"] },
    { name: "liblzma (XZ Utils)", license: "0BSD", dir: "", files: [], note: "0BSD: no notice required" },
  ],
  chmdec: [{ name: "chmdec", license: "MIT", dir: "chmdec", files: ["ext/chmdec/LICENSE.md"] }],
  djvudec: [
    {
      name: "djvudec",
      license: "MIT",
      dir: "djvudec",
      files: [join(extraLicensesDir, "djvudec", "LICENSE.md")],
      note: "license text from the upstream repository; ext/djvudec has none",
    },
  ],
  msdes: [{ name: "D3DES (msdes)", license: "LicenseRef-Public-Domain", dir: "msdes", files: ["ext/msdes/README.md"] }],
  // compiled by mac-build.ts; listed so they are covered if they get linked into the app
  dav1d: [{ name: "dav1d", license: "BSD-2-Clause", dir: "dav1d", files: ["ext/dav1d/COPYING"] }],
  heicdec: [
    {
      name: "heicdec",
      license: "AGPL-3.0-only",
      dir: "heicdec",
      files: [join(extraLicensesDir, "heicdec", "LICENSE.md"), "ext/mupdf/COPYING"],
      note: "port of imazen/heic (AGPL-3.0 or commercial); HEVC may be patent-encumbered",
    },
  ],
  jxldec: [
    {
      name: "jxldec",
      license: "NOASSERTION",
      dir: "jxldec",
      files: ["ext/jxldec/README.md"],
      note: "upstream states no license: resolve before distributing a build that links it",
    },
  ],
  unrar: [
    {
      name: "UnRAR",
      license: "LicenseRef-UnRAR",
      dir: "unrar",
      files: ["ext/a-unrar/license.txt"],
      note: "freeware license, not GPL-compatible",
    },
  ],
};

const iconCredit = [
  "Application icon by Alex (koo.studios at gmail.com), licensed under CC BY 3.0",
  "(https://creativecommons.org/licenses/by/3.0/); converted to ICNS format (cmd/gen-mac-icon.ts).",
];

// liba-zlib.a, libzlib.a -> zlib; liblibwebp.a, liba-libwebp.a -> libwebp
export function libKey(path: string): string {
  return basename(path).replace(/\.a$/, "").replace(/^lib/, "").replace(/^a-/, "");
}

export function componentsForLibs(linkArgs: string[]): Component[] {
  const libs = linkArgs.filter((a) => a.endsWith(".a"));
  const unknown = libs.filter((l) => !Object.hasOwn(libComponents, libKey(l)));
  if (unknown.length > 0) {
    throw new Error(`no license entry for ${unknown.join(", ")}: add it to libComponents in cmd/helper/mac-bundle.ts`);
  }
  const res: Component[] = [];
  for (const lib of libs) {
    for (const c of libComponents[libKey(lib)]) {
      if (!res.includes(c)) res.push(c);
    }
  }
  return res;
}

function git(args: string[]): string | undefined {
  try {
    const p = Bun.spawnSync(["git", ...args], { stdout: "pipe", stderr: "ignore" });
    return p.exitCode === 0 ? p.stdout.toString().trim() : undefined;
  } catch {
    return undefined;
  }
}

// https URL without credentials; git@host:owner/repo -> https://host/owner/repo
function publicRepoUrl(remote: string | undefined): string {
  if (!remote) return upstreamRepo;
  let u = remote.replace(/\.git$/, "");
  const scp = u.match(/^[^@/]+@([^:/]+):(.+)$/);
  if (scp) u = `https://${scp[1]}/${scp[2]}`;
  u = u.replace(/^ssh:\/\/[^@/]+@/, "https://").replace(/^(https?:\/\/)[^@/]+@/, "$1");
  return /^https?:\/\//.test(u) ? u : upstreamRepo;
}

// CFBundleVersion allows at most 3 integers: 3.7 + build 22488 -> 3.7.22488
function bundleVersion(version: string, buildNo: number | undefined): string {
  const parts = version.split(".");
  if (!buildNo || parts.length > 2) return version;
  while (parts.length < 2) parts.push("0");
  return [...parts, String(buildNo)].join(".");
}

export function getSourceInfo(): SourceInfo {
  const version = extractSumatraVersion();
  const commit = git(["rev-parse", "HEAD"]);
  const count = Number(git(["rev-list", "--count", "HEAD"]));
  const buildNo = count > 0 ? count + buildNoBase : undefined;
  const status = commit ? git(["status", "--porcelain"]) : undefined;
  return {
    version,
    bundleVersion: bundleVersion(version, buildNo),
    repoUrl: publicRepoUrl(git(["remote", "get-url", "origin"])),
    commit,
    modified: status === undefined || status.length > 0,
  };
}

function copyrightFromVersionH(): string {
  const m = readFileSync(join("src", "Version.h"), "utf-8").match(/#define\s+kCopyrightStr\s+"([^"]*)"/);
  if (!m) throw new Error("couldn't find kCopyrightStr in src/Version.h");
  return m[1];
}

function xmlEscape(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

export function renderInfoPlist(template: string, vars: Record<string, string>): string {
  let s = template.replace(/<!--[\s\S]*?-->\n/g, "");
  for (const [k, v] of Object.entries(vars)) s = s.replaceAll(`@${k}@`, xmlEscape(v));
  const left = s.match(/@[A-Z_]+@/);
  if (left) throw new Error(`${infoPlistTemplate}: no value for ${left[0]}`);
  return s;
}

function sourceOffer(src: SourceInfo, what: string): string {
  const commit = src.commit ?? "unknown (not built from a git checkout)";
  const tree = src.commit ? `${src.repoUrl}/tree/${src.commit}` : src.repoUrl;
  const lines = [
    "SumatraPDF for macOS: corresponding source code",
    "",
    `Build:      ${what}`,
    `Version:    ${src.version} (bundle version ${src.bundleVersion})`,
    `Repository: ${src.repoUrl}`,
    `Commit:     ${commit}`,
    "",
    "SumatraPDF is licensed under GPLv3 and includes MuPDF and other components licensed under AGPLv3",
    "(see Licenses/INDEX.txt in SumatraPDF.app/Contents/Resources). The complete corresponding source",
    "code of this build, including the build scripts (cmd/) and all bundled libraries (ext/), is the",
    "repository above at the commit above:",
    "",
    `  ${tree}`,
    "",
    "Build instructions: docs/mac/BUILDING.md in that source tree.",
  ];
  if (!src.commit) {
    lines.push(
      "",
      "NOTE: this build was not made from a git checkout. Whoever distributes it must provide its source.",
    );
  } else if (src.modified) {
    lines.push(
      "",
      "NOTE: this build was made from a working tree with changes that are not part of the commit above.",
      "Whoever distributes it must also provide those changes as source code.",
    );
  }
  return lines.join("\n") + "\n";
}

function licenseIndex(components: Component[], src: SourceInfo): string {
  const rows = components.map((c) => {
    const files = c.files.map((f) => `${c.dir}/${basename(f)}`).join(", ") || "-";
    return `${c.name.padEnd(44)} ${c.license.padEnd(30)} ${files}`;
  });
  const notes = components.filter((c) => c.note).map((c) => `- ${c.name}: ${c.note}`);
  return [
    `SumatraPDF ${src.version} for macOS: licenses`,
    "",
    "SumatraPDF is licensed under the GNU GPL v3 (COPYING); code in src/base and some other files is under",
    "the BSD license (COPYING.BSD); authors are listed in AUTHORS. It links MuPDF, which is AGPL v3, so",
    "section 13 of the AGPL v3 applies to SumatraPDF as well. Source code: SOURCE.txt.",
    "",
    ...iconCredit,
    "",
    "Included third-party components (statically linked):",
    "",
    `${"Component".padEnd(44)} ${"License".padEnd(30)} Files`,
    ...rows,
    "",
    "Notes:",
    ...notes,
    "",
  ].join("\n");
}

function writeLicenses(dir: string, components: Component[], src: SourceInfo, what: string): void {
  mkdirSync(dir, { recursive: true });
  for (const f of ["COPYING", "COPYING.BSD", "AUTHORS"]) copyFileSync(f, join(dir, f));
  for (const c of components) {
    if (c.files.length === 0) continue;
    mkdirSync(join(dir, c.dir), { recursive: true });
    for (const f of c.files) {
      if (!existsSync(f)) throw new Error(`license file ${f} for ${c.name} is missing`);
      copyFileSync(f, join(dir, c.dir, basename(f)));
    }
  }
  writeFileSync(join(dir, "INDEX.txt"), licenseIndex(components, src));
  writeFileSync(join(dir, "SOURCE.txt"), sourceOffer(src, what));
}

// "arm64", "x86_64", or "x86_64 arm64" for a universal binary
function exeArchs(exePath: string): string {
  if (process.platform === "darwin") {
    const p = Bun.spawnSync(["lipo", "-archs", exePath], { stdout: "pipe", stderr: "ignore" });
    if (p.exitCode === 0) return p.stdout.toString().trim();
  }
  return process.arch === "x64" ? "x86_64" : process.arch;
}

/**
 * Write Info.plist, PkgInfo, the icon and license texts into appDir (after linking Contents/MacOS/SumatraPDF).
 * linkArgs are the app's link inputs; the .a files among them decide which third-party licenses ship.
 * Must run before code signing: the signature seals Contents/Resources and Info.plist.
 */
export function addBundleResources(appDir: string, linkArgs: string[]): void {
  const contents = join(appDir, "Contents");
  const res = join(contents, "Resources");
  const components = componentsForLibs(linkArgs);
  const src = getSourceInfo();
  const what = `macOS ${exeArchs(join(contents, "MacOS", "SumatraPDF"))} (${basename(dirname(appDir))})`;
  rmSync(res, { recursive: true, force: true });
  mkdirSync(res, { recursive: true });

  const plist = renderInfoPlist(readFileSync(infoPlistTemplate, "utf-8"), {
    VERSION: src.version,
    BUNDLE_VERSION: src.bundleVersion,
    COPYRIGHT: copyrightFromVersionH(),
  });
  writeFileSync(join(contents, "Info.plist"), plist);
  writeFileSync(join(contents, "PkgInfo"), "APPL????");
  copyFileSync(appIcon, join(res, "SumatraPDF.icns"));
  writeLicenses(join(res, "Licenses"), components, src, what);

  if (!src.commit || src.modified) {
    console.log(
      "  NOTE: built from a modified or non-git tree; SOURCE.txt says so (see docs/mac/THIRD-PARTY-LICENSES.md)",
    );
  }
}

async function run(args: string[], env?: Record<string, string>): Promise<void> {
  const proc = Bun.spawn(args, { stdout: "ignore", stderr: "pipe", env: { ...process.env, ...env } });
  const code = await proc.exited;
  if (code !== 0) {
    const stderr = await new Response(proc.stderr).text();
    throw new Error(`${args.slice(0, 2).join(" ")} failed (exit ${code}): ${stderr}`);
  }
}

export interface MacPackageOptions {
  outDir: string;
  appDir: string;
  packageName: string; // e.g. SumatraPDF-3.7-mac-arm64
  dmg: boolean;
}

/**
 * Create outDir/<packageName>.tar.gz, and on macOS also .zip (ditto) and, if requested, .dmg (hdiutil).
 * Each holds <packageName>/{SumatraPDF.app, README.md, COPYING, SOURCE.txt}; the .dmg adds an
 * Applications link for drag-and-drop install.
 */
export async function packageMacApp(o: MacPackageOptions): Promise<string[]> {
  const isMac = process.platform === "darwin";
  const tempRoot = mkdtempSync(join(tmpdir(), "sumatrapdf-mac-package-"));
  const pkgDir = join(tempRoot, o.packageName);
  const base = join(o.outDir, o.packageName);
  const outputs: string[] = [];
  try {
    mkdirSync(pkgDir, { recursive: true });
    // ditto keeps extended attributes and the code signature intact
    const pkgApp = join(pkgDir, "SumatraPDF.app");
    if (isMac) await run(["ditto", o.appDir, pkgApp]);
    else cpSync(o.appDir, pkgApp, { recursive: true });
    copyFileSync(archiveReadme, join(pkgDir, "README.md"));
    copyFileSync("COPYING", join(pkgDir, "COPYING"));
    copyFileSync(join(o.appDir, "Contents", "Resources", "Licenses", "SOURCE.txt"), join(pkgDir, "SOURCE.txt"));

    // COPYFILE_DISABLE: no AppleDouble ._* files in the tarball
    rmSync(`${base}.tar.gz`, { force: true });
    await run(["tar", "-czf", `${base}.tar.gz`, "-C", tempRoot, o.packageName], { COPYFILE_DISABLE: "1" });
    outputs.push(`${base}.tar.gz`);

    if (isMac) {
      rmSync(`${base}.zip`, { force: true });
      await run(["ditto", "-c", "-k", "--keepParent", pkgDir, `${base}.zip`]);
      outputs.push(`${base}.zip`);
    }

    if (isMac && o.dmg) {
      symlinkSync("/Applications", join(pkgDir, "Applications"));
      await run([
        "hdiutil",
        "create",
        "-volname",
        "SumatraPDF",
        "-srcfolder",
        pkgDir,
        "-ov",
        "-format",
        "UDZO",
        `${base}.dmg`,
      ]);
      outputs.push(`${base}.dmg`);
    }
  } finally {
    rmSync(tempRoot, { recursive: true, force: true });
  }
  for (const p of outputs) console.log(`  -> ${p}`);
  return outputs;
}
