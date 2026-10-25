#!/usr/bin/env bash
# Hand one task to a low-cost coding model (the "worker"), then stop for review.
#
#   tools/worker.sh <task.md> [model]
#
# <task.md> is a plain task spec: goal, files to look at, what "done" means, tests to add.
# [model] is an opencode model id. Default: $TBCAD_WORKER_MODEL, else deepseek/deepseek-chat.
#   deepseek/deepseek-chat              needs DEEPSEEK_API_KEY   (host api.deepseek.com)
#   openrouter/qwen/qwen3-coder         needs OPENROUTER_API_KEY (host openrouter.ai)
#   openrouter/moonshotai/kimi-k2       needs OPENROUTER_API_KEY
#
# The worker runs opencode (npm i -g opencode-ai) on branch worker/<task name>, starting from
# origin/main, with AGENTS.md as its rules. It leaves its changes uncommitted. The reviewer then
# reads `git diff`, builds, runs ctest and tools/wincheck.sh, and commits only if all are green.
set -euo pipefail

task="${1:?usage: tools/worker.sh <task.md> [model]}"
model="${2:-${TBCAD_WORKER_MODEL:-deepseek/deepseek-chat}}"
root="$(git rev-parse --show-toplevel)"
task="$(realpath "$task")"
cd "$root"

if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
    echo "worker: working tree has changes; commit or stash them first" >&2
    exit 1
fi

name="$(basename "$task" .md)"
git fetch -q origin main
git checkout -q -B "worker/$name" origin/main
rm -f WORKER_REPORT.md

mkdir -p build/worker-logs
log="build/worker-logs/$name-$(date -u +%Y%m%dT%H%M%SZ).log"
echo "worker: $model on worker/$name, log $log"

prompt="Follow AGENTS.md. Your task:

$(cat "$task")"

start=$(date +%s)
OPENCODE_CONFIG="$root/tools/worker/opencode.json" \
    timeout "${TBCAD_WORKER_TIMEOUT:-3600}" \
    opencode run --model "$model" --print-logs "$prompt" >"$log" 2>&1 || echo "worker: opencode exited $?"
echo "worker: finished in $(( $(date +%s) - start ))s"

git status --short
git diff --stat
[[ -f WORKER_REPORT.md ]] && cat WORKER_REPORT.md
opencode stats 2>/dev/null | tail -15 || true
