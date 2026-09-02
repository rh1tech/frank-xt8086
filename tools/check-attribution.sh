#!/usr/bin/env bash
#
# frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-xt8086
# SPDX-License-Identifier: GPL-3.0-or-later
#
# check-attribution.sh — refuse history that credits an AI.
#
# This repository's history names the people responsible for it and nobody
# else. A co-author trailer naming a tool is not a courtesy: it puts that
# tool in GitHub's contributor list, where it stays until the history is
# rewritten.
#
# One implementation, used by every gate — .githooks/commit-msg,
# .githooks/applypatch-msg, .githooks/pre-push and the CI job all call
# this. The regexes used to be copy-pasted between the hook and the
# workflow, which is a guarantee that one day they say different things
# and the weaker one is the one that runs.
#
# Usage:
#   check-attribution.sh --message FILE       one prospective message
#   check-attribution.sh --range A..B         every commit in a range
#   check-attribution.sh --range SHA -1       a single commit
#   check-attribution.sh --self-test          verify the patterns
#
# Exit 0 clean, 1 on a finding, 2 on a usage error.
#
set -uo pipefail

# ---------------------------------------------------------------------------
# What counts as a tool
# ---------------------------------------------------------------------------
#
# Deliberately long and deliberately never matched as free text. Every use
# below is anchored to an attribution construct — a trailer key, an
# advertising phrase, or an email address — because this is firmware for a
# machine with a text CURSOR, a CODEC or two and a GEMINI-era clock chip,
# and a bare word match would reject "fix: MC6845 cursor blink" while
# letting a hand-written "thanks to my mate Claude" through untouched.
#
# Add to the list freely; a name here costs nothing until it turns up next
# to a colon.
TOOLS='copilot|claude|chatgpt|gpt-[0-9]|anthropic|openai|codex|cursor|devin'
TOOLS="$TOOLS"'|gemini|bard|aider|windsurf|cody|codewhisperer|amazon ?q'
TOOLS="$TOOLS"'|tabnine|codeium|continue\.dev|sourcegraph|replit|v0\.dev'
TOOLS="$TOOLS"'|bolt\.new|lovable|sweep ?ai|jules|deepseek|qwen|grok'
TOOLS="$TOOLS"'|mistral|llama|perplexity|phind|cline|roo ?code|kilo ?code'
TOOLS="$TOOLS"'|augment ?code|qodo|goose|opencode|crush|factory ?ai'
TOOLS="$TOOLS"'|junie|trae|blackbox ?ai|refact|codebuddy|zencoder'
TOOLS="$TOOLS"'|llm|large language model|\bai\b|artificial intelligence'

# Trailer keys that assign credit. "reviewed-by" and "tested-by" are
# absent on purpose: a person may legitimately have reviewed a change, and
# the tool name is what makes a trailer a problem, not the key.
KEYS='co-authored-by|coauthored-by|assisted-by|ai-assisted-by|co-developed-by'
KEYS="$KEYS"'|generated-by|authored-by|on-behalf-of|signed-off-by|created-by'
KEYS="$KEYS"'|written-by|helped-by|agent|session'

# Advertising, with or without a trailer. These are whole phrases, so they
# cannot fire on ordinary prose.
ADS='generated with \[?(claude|copilot|chatgpt|codex|cursor|gemini|windsurf)'
ADS="$ADS"'|(made|built|written|created) (with|by) (claude|copilot|chatgpt|codex)'
ADS="$ADS"'|powered by (claude|copilot|chatgpt|openai|anthropic)'
ADS="$ADS"'|claude-session|claude\.ai/code|chat\.openai\.com|copilot\.github\.com'
ADS="$ADS"'|🤖|:robot:'

# Machine identities. An address is an attribution construct in itself, so
# tool names are safe to match inside one.
#
# Note what is NOT here: a bare @users.noreply.github.com. That is the
# address GitHub gives every human who keeps their email private, and
# rejecting it would lock out real contributors to catch bots that are
# already caught by the [bot] marker in the name.
MAILS='noreply@anthropic\.com|@openai\.com'
MAILS="$MAILS"'|(copilot|claude|chatgpt|codex|anthropic|openai|devin|aider)[a-z0-9._%+-]*@'
MAILS="$MAILS"'|\[bot\]'

usage() {
    sed -n '2,/^set -uo/p' "$0" | sed 's/^# \{0,1\}//;$d'
    exit 2
}

# ---------------------------------------------------------------------------
# The check itself
# ---------------------------------------------------------------------------
#
# stdin is one record: the message, optionally preceded by "author: ..."
# and "committer: ..." lines that the range mode synthesises. Everything
# is lowercased first so the patterns need no case alternatives.
scan() {
    local label="$1" text findings

    text=$(tr '[:upper:]' '[:lower:]')

    # One grep, three alternatives, so a line that trips two of them is
    # reported once. Three separate greps reported it twice and ran them
    # together without a separator.
    #
    #   1. a trailer key whose value names a tool. The key must start the
    #      line, which is what a trailer is — "the co-authored-by trailer
    #      is banned" written mid-paragraph is prose and stays legal.
    #   2. an advertising phrase, anywhere.
    #   3. a machine identity, anywhere.
    findings=$(printf '%s\n' "$text" | grep -nE \
        "(^[[:space:]]*($KEYS)[[:space:]]*:.*($TOOLS))|($ADS)|($MAILS)" || true)

    if [ -n "$findings" ]; then
        printf '%s\n' "--- $label" >&2
        printf '%s\n' "$findings" | sed 's/^/    /' >&2
        return 1
    fi
    return 0
}

fail_banner() {
    cat >&2 <<'EOF'

This repository's history names the people responsible for it and nobody
else. Remove the trailer, the advertising line or the machine identity and
commit again.

  git commit --amend                     fix the message you just wrote
  git rebase -i <base>                   fix one further back
  git -c user.name=... -c user.email=... set the identity for one commit

See tools/check-attribution.sh for what is matched and why.
EOF
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
[ $# -ge 1 ] || usage
rc=0

case "$1" in
    --message)
        [ $# -eq 2 ] || usage
        [ -f "$2" ] || { echo "no such message file: $2" >&2; exit 2; }

        # The identity this commit would carry. git var applies the same
        # precedence git itself will, so a -c override on the command line
        # is seen here too.
        ident="author: $(git var GIT_AUTHOR_IDENT 2>/dev/null || echo '')
committer: $(git var GIT_COMMITTER_IDENT 2>/dev/null || echo '')"

        { printf '%s\n' "$ident"; cat "$2"; } | scan "the message being committed" || rc=1
        ;;

    --range)
        shift
        [ $# -ge 1 ] || usage

        # Via a temp file, for two reasons that pull the same way.
        #
        # git's exit status has to be checkable. Reading straight from a
        # process substitution hid the failure of the command inside it:
        # an unresolvable range made git print a fatal, the loop read
        # nothing, and the script exited 0 — a gate reporting "clean"
        # precisely when it had inspected nothing. CI derives its range
        # from event metadata that can name a commit this clone does not
        # have, so that is the failure mode that would really have
        # happened.
        #
        # And the records are NUL-separated, which rules out the obvious
        # alternative: bash drops NUL bytes when a command substitution
        # assigns to a variable, so log=$(git log ...) silently welds
        # every commit into one unterminated record that `read -d ''`
        # then never yields. A file survives both.
        tmp=$(mktemp "${TMPDIR:-/tmp}/attribution.XXXXXX") || exit 2
        trap 'rm -f "$tmp"' EXIT

        git log --format='%H%nauthor: %an <%ae>%ncommitter: %cn <%ce>%n%B%x00' "$@" > "$tmp" || {
            echo "check-attribution: git log failed for: $*" >&2
            echo "check-attribution: refusing to report a clean result." >&2
            exit 2
        }

        n=0
        # NUL between records so a message containing blank lines, or a
        # subject that looks like a delimiter, cannot split a commit in two.
        while IFS= read -r -d '' record; do
            # git writes a newline after each record's NUL, so every
            # record after the first arrives with that separator glued to
            # its front. Left in place it became the "sha", which was
            # empty, which hit the guard below and skipped the commit —
            # so a range of twenty commits had exactly one inspected and
            # reported itself clean.
            while [ "${record#$'\n'}" != "$record" ]; do record="${record#$'\n'}"; done

            sha=${record%%$'\n'*}
            [ -n "$sha" ] || continue
            n=$((n+1))
            scan "commit $sha" <<<"${record#*$'\n'}" || rc=1
        done < "$tmp"

        # Cross-check against git's own count. The bug above was invisible
        # because the script had no idea how many commits it should have
        # seen; now a parser that loses records fails loudly instead of
        # passing quietly, which is the only failure direction that
        # matters for a gate.
        want=$(git rev-list --count "$@" 2>/dev/null || echo "?")
        if [ "$want" != "?" ] && [ "$n" != "$want" ]; then
            echo "check-attribution: inspected $n commit(s) but the range holds $want." >&2
            echo "check-attribution: refusing to report a clean result." >&2
            exit 2
        fi

        # An empty range is legitimate — a push that adds no commits — but
        # the count is always reported, so "clean" always says what it read.
        [ "$rc" -ne 0 ] || echo "check-attribution: $n commit(s) inspected, clean."
        ;;

    --self-test)
        # The patterns are the whole security of this thing, so they get
        # tested. Cheap enough to run from CI on every push.
        pass=0 fail=0
        check() { # check <expect: bad|ok> <text>
            local want="$1" text="$2" got
            if printf '%s' "$text" | scan "self-test" >/dev/null 2>&1; then got=ok; else got=bad; fi
            if [ "$got" = "$want" ]; then pass=$((pass+1)); else
                fail=$((fail+1)); printf 'FAIL want=%s got=%s: %s\n' "$want" "$got" "$text" >&2
            fi
        }
        # Must be rejected.
        check bad 'fix: thing

Co-Authored-By: Claude <noreply@anthropic.com>'
        check bad 'Co-authored-by: Copilot <copilot@github.com>'
        check bad 'co-authored-by: GitHub Copilot'
        check bad 'Assisted-by: Codex'
        check bad 'Signed-off-by: Cursor Agent <agent@cursor.sh>'
        check bad '🤖 Generated with [Claude Code](https://claude.ai/code)'
        check bad 'Claude-Session: https://claude.ai/code/session_01'
        check bad 'author: openai[bot] <bot@openai.com>'
        check bad 'Built with ChatGPT'
        check bad 'Co-Authored-By: Gemini <x@google.com>'
        # Must be accepted — the false positives that matter here.
        check ok  'fix: MC6845 cursor blink is inverted at 80x25'
        check ok  'feat: cursor_start and cursor_end now honour R10/R11'
        check ok  'docs: explain why the AD bus cannot move'
        check ok  'fix: the serial-console build needs pico/bootrom.h of its own'
        check ok  'Reviewed-by: Mikhail Matveev <xtreme@rh1.tech>'
        check ok  'chore: note that co-authored-by trailers are rejected by CI'
        check ok  'perf: claude waits one fewer cycle'   # a person, mid-prose
        check ok  'Signed-off-by: Mikhail Matveev <xtreme@rh1.tech>'
        check ok  'author: Some Contributor <9821+contrib@users.noreply.github.com>'
        check ok  'feat: decode the 8253 gate, mode 3, square wave'
        check ok  'fix: AD0-AD15 pindirs are restored on the cleanup path'
        printf 'self-test: %d passed, %d failed\n' "$pass" "$fail"
        [ "$fail" -eq 0 ] || exit 1
        exit 0
        ;;

    *) usage ;;
esac

[ "$rc" -eq 0 ] || fail_banner
exit "$rc"
