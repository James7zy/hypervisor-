#!/bin/sh
# SPDX-License-Identifier: TBD
# Arch boundary check (ADR-0015; migration: docs/superpowers/specs/2026-09-29-arch-boundary-design.md).
#
# Code outside hypervisor/arch/ must reach the architecture only through
# *_arch_* hooks and <arch/xxx.h> headers (ADR-0015). This scans every
# non-arch source file for forbidden patterns and fails on any hit. Comments
# and string literals are blanked first, so prose and log text such as
# "[hv] PSCI: ..." do not count; #include paths are kept and checked.
#
# There is no allowlist: the migration that needed one is finished. The only
# exceptions are the permanent ones in exempt() below.
#
# Usage: sh scripts/check-arch-boundary.sh
set -eu

ROOT=hypervisor

# rule-id  extended-regex (matched against comment-stripped source)
RULES='
asm        (^|[^A-Za-z0-9_])(__asm__|asm)([^A-Za-z0-9_]|$)
sysreg     [A-Za-z0-9]_(EL[0-3]|el[0-3])([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])(ICH|ICC)_[A-Z0-9_]+
psci       (^|[^A-Za-z0-9_])(psci|PSCI)
vgic       (^|[^A-Za-z0-9_])vgic
stage2     (^|[^A-Za-z0-9_])stage2
gic        (^|[^A-Za-z0-9_])gic_
vtimer     (^|[^A-Za-z0-9_])vtimer
board      (^|[^A-Za-z0-9_])BOARD_[A-Z0-9_]+|board\.h
arch-path  #[[:space:]]*include[[:space:]]*[<"].*arch/(arm64|x86|riscv)/
'

# Permanent exemptions: the static VM configuration table IS board config
# (ADR-0008), so it may read BOARD_* constants.
exempt() {
    case "$1 $2" in
        "$ROOT/common/vm/vm_config.h board") return 0 ;;
    esac
    return 1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

found=$tmp/found
: > "$found"
status=0

files=$(find "$ROOT" -path "$ROOT/arch" -prune -o -type f \
        \( -name '*.c' -o -name '*.h' -o -name '*.S' \) -print | sort)

for f in $files; do
    case "$f" in
        *.S)
            echo "$f asm-file" >> "$found"
            printf '%s: asm-file: assembly source outside arch/\n' "$f" \
                > "$tmp/$(echo "$f asm-file" | tr '/ ' '__')"
            continue ;;
    esac
    # Blank string literals and comments, keeping every newline so the
    # reported line numbers are the real ones. #include paths are kept
    # verbatim: a quoted path is code, not prose.
    perl -0777 -pe '
        s{(^[ \t]*\#[ \t]*include[ \t]*[<"][^>"\n]*[>"])|("(?:\\.|[^"\\\n])*")|(\x27(?:\\.|[^\x27\\\n])*\x27)|(/\*.*?\*/)|(//[^\n]*)}{
            defined $1 ? $1 : defined $2 ? q("") : defined $3 ? $3 :
            do { (my $c = $4 // q()) =~ s/[^\n]//g; $c }
        }gsme' "$f" > "$tmp/src"
    echo "$RULES" | while read -r id re; do
        [ -n "$id" ] || continue
        exempt "$f" "$id" && continue
        if grep -En -- "$re" "$tmp/src" > "$tmp/hits"; then
            echo "$f $id" >> "$found"
            sed "s#^#$f: $id: #" "$tmp/hits" > "$tmp/$(echo "$f $id" | tr '/ ' '__')"
        fi
    done
done

sort -u "$found" -o "$found"

if [ -s "$found" ]; then
    echo "FAIL: arch boundary crossed outside hypervisor/arch/:"
    while read -r f id; do
        sed 's/^/  /' "$tmp/$(echo "$f $id" | tr '/ ' '__')"
    done < "$found"
    echo "  Move the code into arch/ behind a *_arch_* hook (see CLAUDE.md)."
    status=1
else
    echo "PASS: arch boundary holds"
fi
exit "$status"
