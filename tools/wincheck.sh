#!/usr/bin/env bash
# Windows (MSVC) check for cloud sessions that have no Windows compiler.
#
#   tools/wincheck.sh [local] [build-dir]   fast: re-reads the files this branch changed (against
#                                           origin/main) with clang and the warnings MSVC's /W4
#                                           turns into errors under /WX, plus a few MSVC-only
#                                           traps. Needs compile_commands.json in build-dir
#                                           (default build/linux-clang-debug). Exit 1 on findings.
#   tools/wincheck.sh all [build-dir]       the same over every file of the tree.
#   tools/wincheck.sh push                  the real thing: pushes HEAD to wincheck/<branch>, which
#                                           runs .github/workflows/wincheck.yml (MSVC, /WX, every
#                                           error listed, then the tests) on GitHub's Windows
#                                           runner. Read the result with the GitHub tools:
#                                           actions_list list_workflow_runs on wincheck.yml for
#                                           branch wincheck/<branch>, then get_job_logs (the end of
#                                           the build log lists every MSVC error).
set -euo pipefail

mode="${1:-local}"
root="$(git rev-parse --show-toplevel)"
cd "$root"

if [[ "$mode" == "push" ]]; then
    branch="$(git rev-parse --abbrev-ref HEAD)"
    target="wincheck/${branch#claude/}"
    git push --force -q origin "HEAD:refs/heads/$target"
    echo "Pushed $(git rev-parse --short HEAD) to $target."
    echo "Workflow: wincheck.yml, branch $target. The run takes about as long as a full MSVC build."
    echo "Delete the branch afterwards: git push origin --delete $target"
    exit 0
fi

build="${2:-build/linux-clang-debug}"
db="$build/compile_commands.json"
if [[ ! -f "$db" ]]; then
    echo "No $db: configure and build first (cmake --preset linux-clang-debug)." >&2
    exit 2
fi

if [[ "$mode" == "all" ]]; then
    files=""
else
    git fetch -q origin main || true
    files="$(git diff --name-only origin/main...HEAD; git diff --name-only HEAD)"
    files="$(echo "$files" | grep -E '\.(cpp|h|hpp)$' | sort -u || true)"
    if [[ -z "$files" ]]; then
        echo "No C++ files changed against origin/main."
        exit 0
    fi
fi

status=0

# MSVC-only traps that no clang flag shows.
grep_files="$files"
[[ "$mode" == "all" ]] && grep_files="$(git ls-files 'libs/*.cpp' 'libs/*.h' 'apps/*.cpp' 'apps/*.h' | grep -v third_party)"
if [[ -n "$grep_files" ]]; then
    # M_PI needs _USE_MATH_DEFINES before <cmath> on MSVC; the tree has tbcad::kPi.
    if hits="$(echo "$grep_files" | xargs -r grep -nE '\bM_PI(_2|_4)?\b' 2>/dev/null)"; then
        echo "$hits" | sed 's/$/   [MSVC: M_PI is not defined; use kPi]/'
        status=1
    fi
fi

python3 - "$db" "$mode" "$files" <<'EOF' || status=1
import concurrent.futures as cf, json, os, shlex, subprocess, sys

db, mode, files = sys.argv[1], sys.argv[2], sys.argv[3].split()
# Clang warnings that match MSVC /W4 warnings the tree trips (MSVC number in the comment).
flags = [
    "-Wshadow",  # C4456 C4457 C4458 C4459: a declaration hides another
]
root = os.getcwd()
entries = json.load(open(db))
entries = [e for e in entries if "/third_party/" not in e["file"] and "/build/" not in e["file"]]
if mode != "all":
    want = {os.path.join(root, f) for f in files}
    headers = {f for f in want if not f.endswith(".cpp")}
    picked = [e for e in entries if e["file"] in want]
    # A changed header: check one file that includes it, per directory of the header's library.
    for h in headers:
        name = os.path.basename(h)
        for e in entries:
            try:
                if name in open(e["file"], encoding="utf-8", errors="ignore").read():
                    picked.append(e)
                    break
            except OSError:
                pass
    entries = list({e["file"]: e for e in picked}.values())

def check(e):
    args = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c" or a.startswith("-Werror"):
            continue
        out.append(a)
    out += ["-fsyntax-only", "-Wno-everything"] + flags
    r = subprocess.run(out, cwd=e["directory"], capture_output=True, text=True)
    return [l for l in r.stderr.splitlines() if " warning: " in l or " error: " in l]

found = set()
with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
    for lines in ex.map(check, entries):
        found.update(lines)
for l in sorted(found):
    print(l.replace(root + "/", ""))
print(f"wincheck: {len(entries)} files read, {len(found)} findings.", file=sys.stderr)
sys.exit(1 if found else 0)
EOF

if [[ $status -ne 0 ]]; then
    echo "wincheck: fix the lines above; they fail the MSVC build (/W4 /WX)." >&2
fi
exit $status
