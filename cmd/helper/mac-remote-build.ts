/**
 * Run the macOS build (cmd/build.ts -mac) for a pushed branch on a Mac, over ssh.
 *
 * Invoked by cmd/build.ts -mac-remote -branch <name>. The Mac is $SUMATRA_MAC_HOST (an ssh destination, e.g.
 * user@mac.local) and its checkout is $SUMATRA_MAC_DIR (default: sumatrapdf, relative to the remote home). The
 * checkout must be clean; it is switched to the branch for the build and restored afterwards, on success or failure.
 */

function shellQuote(s: string): string {
  return `'${s.replace(/'/g, `'\\''`)}'`;
}

export async function buildMacRemote(branch: string, buildArgs: string[]): Promise<void> {
  const remoteHost = process.env.SUMATRA_MAC_HOST;
  if (!remoteHost) {
    throw new Error("-mac-remote needs SUMATRA_MAC_HOST set to the Mac's ssh destination (e.g. user@mac.local)");
  }
  const remoteDir = process.env.SUMATRA_MAC_DIR || "sumatrapdf";
  const quotedArgs = buildArgs.map(shellQuote).join(" ");
  const remoteScript = `
set -euo pipefail

cd ${shellQuote(remoteDir)}

branch=${shellQuote(branch)}

if [[ -n "$(git status --porcelain)" ]]; then
  echo "remote checkout is not clean; aborting" >&2
  git status --short >&2
  exit 2
fi

orig_branch="$(git symbolic-ref --quiet --short HEAD || true)"
orig_ref="$(git rev-parse --verify HEAD)"

restore_original() {
  local rc=$?
  trap - EXIT INT TERM
  echo "> restoring original git checkout"
  if [[ -n "$orig_branch" ]]; then
    git switch "$orig_branch"
  else
    git switch --detach "$orig_ref"
  fi
  local restore_rc=$?
  if [[ $restore_rc -ne 0 ]]; then
    echo "failed to restore original git checkout" >&2
    exit $restore_rc
  fi
  exit $rc
}

trap restore_original EXIT INT TERM

echo "> fetching origin/$branch"
git fetch origin "$branch"

echo "> switching to $branch"
git switch -C "$branch" FETCH_HEAD

if [[ -n "$(git status --porcelain)" ]]; then
  echo "remote checkout is not clean after switching to $branch; aborting" >&2
  git status --short >&2
  exit 2
fi

if command -v bun >/dev/null 2>&1; then
  bun cmd/build.ts -mac ${quotedArgs}
else
  ~/.bun/bin/bun cmd/build.ts -mac ${quotedArgs}
fi
`;
  const remoteCmd = `/bin/bash -lc ${shellQuote(remoteScript)}`;

  console.log(`> ssh ${remoteHost} (in ${remoteDir}: bun cmd/build.ts -mac ${buildArgs.join(" ")})`);
  const proc = Bun.spawn(["ssh", remoteHost, remoteCmd], {
    stdout: "inherit",
    stderr: "inherit",
    stdin: "inherit",
  });

  const exitCode = await proc.exited;
  if (exitCode !== 0) {
    throw new Error(`remote mac build failed with exit code ${exitCode}`);
  }
}
