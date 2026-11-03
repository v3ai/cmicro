#!/usr/bin/env bash
# CBMC proofs for src/util.c (memory safety + absence of UB + functional
# properties).  Run from anywhere; works relative to the repo root.
#
#   tests/formal/cbmc/util/run.sh            run every proof; exit 1 on any failure
#   tests/formal/cbmc/util/run.sh NAME...    run only the named proofs (shell
#                                            patterns allowed, e.g. 'glob_match_p*')
#   tests/formal/cbmc/util/run.sh --cover    non-vacuity: run every proof with
#                                            --cover location and print the
#                                            line-block coverage of the
#                                            function(s) under test
#
# -D__NO_CTYPE (misc_int, strcmp_family): glibc's <ctype.h> otherwise
# expands isalnum()/tolower() into lookups through __ctype_b_loc(), which
# CBMC does not model (it reports every such lookup as an invalid pointer);
# with it the calls go to CBMC's own isalnum/tolower models.
#
# Needs CBMC 5.95 (cbmc, goto-cc, goto-instrument) on PATH.
set -u
cd "$(dirname "$0")/../../../.." || exit 2

D=tests/formal/cbmc/util
U=src/util.c
S=$D/stubs.c
OUT=${CBMC_OUT:-${TMPDIR:-/tmp}/cbmc-util.$$}
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# Checks used by every bounded proof.  --unwinding-assertions is mandatory:
# a loop that is not fully unwound within --unwind makes the proof FAIL.
CHECKS="--bounds-check --pointer-check --pointer-overflow-check --pointer-primitive-check
        --signed-overflow-check --undefined-shift-check --div-by-zero-check
        --memory-leak-check --unwinding-assertions"
# Contract runs: the same checks minus --memory-leak-check (the objects the
# contract instrumentation allocates for __CPROVER_is_fresh are owned by
# the instrumentation, not by the code under test).  Loops (utf8_prev's
# 3-step walk, in_table's <= 6-step binary search) are first unwound by
# goto-instrument with unwinding assertions, so the bound is itself proved.
CCHECKS="--bounds-check --pointer-check --pointer-overflow-check --pointer-primitive-check
        --signed-overflow-check --undefined-shift-check --div-by-zero-check"
# For code that calls malloc/realloc itself and must cope with failure
# (util.c's x* wrappers exit via oom()): every allocation may return NULL.
MAYFAIL="--malloc-may-fail --malloc-fail-null"

MODE=prove
if [ "${1:-}" = --cover ]; then MODE=cover; shift; fi
ONLY=("$@")
fail=0

selected() {
    local pat
    [ ${#ONLY[@]} -eq 0 ] && return 0
    for pat in "${ONLY[@]}"; do
        # shellcheck disable=SC2053
        [[ $1 == $pat ]] && return 0
    done
    return 1
}

# proof NAME "FUNCTIONS UNDER TEST" cbmc-args...
proof() {
    local name=$1 funcs=$2 t0 t1 out rc
    shift 2
    selected "$name" || return 0
    t0=$(date +%s)
    if [ $MODE = prove ]; then
        # shellcheck disable=SC2086
        out=$(cbmc "$@" $CHECKS 2>&1); rc=$?
        t1=$(date +%s)
        if [ $rc -eq 0 ] && grep -q "VERIFICATION SUCCESSFUL" <<<"$out"; then
            printf '%-26s PASS  %4ss  %s\n' "$name" $((t1 - t0)) \
                "$(grep -Eo '[0-9]+ of [0-9]+ failed' <<<"$out" | tail -1)"
        else
            printf '%-26s FAIL  %4ss\n' "$name" $((t1 - t0))
            grep -E 'FAILURE|ERROR|error:' <<<"$out" | head -20; echo "    (cbmc exit status $rc; last output line: $(tail -1 <<<"$out"))"
            fail=1
        fi
    else
        # coverage goals are solved with the default SAT back end (--cover
        # does not support --cvc4)
        local a cargs=()
        for a in "$@"; do [ "$a" = --cvc4 ] || cargs+=("$a"); done
        out=$(cbmc "${cargs[@]}" --cover location 2>&1)
        t1=$(date +%s)
        grep -q "coverage" <<<"$out" ||
            echo "$name: coverage run failed: $(tail -1 <<<"$out")"
        local f tot sat
        for f in $funcs; do
            tot=$(grep -cE "^\[$f\.coverage\.[0-9]+\]" <<<"$out")
            sat=$(grep -cE "^\[$f\.coverage\.[0-9]+\].*: SATISFIED" <<<"$out")
            printf '%-26s %-16s %3s/%-3s blocks covered (%3s%%)  %4ss\n' "$name" "$f" "$sat" "$tot" \
                $(( tot ? 100 * sat / tot : 0 )) $((t1 - t0))
            grep -E "^\[$f\.coverage\.[0-9]+\].*: FAILED" <<<"$out" | sed 's/^/    uncovered: /'
        done
    fi
}

# proofgi NAME "FUNCTIONS" "GOTO-CC ARGS" "GOTO-INSTRUMENT ARGS" cbmc-args...
# Like proof, but compiles with goto-cc and transforms the program with
# goto-instrument (e.g. --replace-calls) before running cbmc.
proofgi() {
    local name=$1 funcs=$2 cc=$3 gi=$4 t0 t1 out rc f tot sat
    shift 4
    selected "$name" || return 0
    t0=$(date +%s)
    # shellcheck disable=SC2086
    if ! out=$( { goto-cc -I src -o "$OUT/$name.goto" $cc &&
                  goto-instrument $gi "$OUT/$name.goto" "$OUT/$name.i.goto"; } 2>&1); then
        printf '%-26s FAIL  (build)\n%s\n' "$name" "$out"; fail=1; return 0
    fi
    if [ $MODE = prove ]; then
        # shellcheck disable=SC2086
        out=$(cbmc "$OUT/$name.i.goto" "$@" $CHECKS 2>&1); rc=$?
        t1=$(date +%s)
        if [ $rc -eq 0 ] && grep -q "VERIFICATION SUCCESSFUL" <<<"$out"; then
            printf '%-26s PASS  %4ss  %s\n' "$name" $((t1 - t0)) \
                "$(grep -Eo '[0-9]+ of [0-9]+ failed' <<<"$out" | tail -1)"
        else
            printf '%-26s FAIL  %4ss\n' "$name" $((t1 - t0))
            grep -E 'FAILURE|ERROR|error:' <<<"$out" | head -20; echo "    (cbmc exit status $rc; last output line: $(tail -1 <<<"$out"))"
            fail=1
        fi
    else
        out=$(cbmc "$OUT/$name.i.goto" "$@" --cover location 2>&1)
        t1=$(date +%s)
        for f in $funcs; do
            tot=$(grep -cE "^\[$f\.coverage\.[0-9]+\]" <<<"$out")
            sat=$(grep -cE "^\[$f\.coverage\.[0-9]+\].*: SATISFIED" <<<"$out")
            printf '%-26s %-16s %3s/%-3s blocks covered (%3s%%)  %4ss\n' "$name" "$f" "$sat" "$tot" \
                $(( tot ? 100 * sat / tot : 0 )) $((t1 - t0))
            grep -E "^\[$f\.coverage\.[0-9]+\].*: FAILED" <<<"$out" | sed 's/^/    uncovered: /'
        done
    fi
}

# contract NAME FUNCTION
# Unbounded, modular proof of FUNCTION against the contract attached to its
# declaration in utf8_contracts.c: goto-cc, goto-instrument
# --enforce-contract (assumes the requires clauses, asserts ensures and the
# assigns write set), then cbmc with the usual checks from harness_FUNCTION.
contract() {
    local name=$1 fn=$2 t0 t1 out rc
    selected "$name" || return 0
    [ $MODE = cover ] && return 0 # line coverage comes from the bounded proof of the same function
    t0=$(date +%s)
    # shellcheck disable=SC2086
    out=$( { goto-cc -I src -o "$OUT/$name.goto" $D/utf8_contracts.c $S &&
             goto-instrument --unwind 8 --unwinding-assertions "$OUT/$name.goto" "$OUT/$name.u.goto" &&
             goto-instrument --enforce-contract "$fn" "$OUT/$name.u.goto" "$OUT/$name.i.goto" &&
             cbmc "$OUT/$name.i.goto" --function "harness_$fn" $CCHECKS; } 2>&1); rc=$?
    t1=$(date +%s)
    if [ $rc -eq 0 ] && grep -q "VERIFICATION SUCCESSFUL" <<<"$out"; then
        printf '%-26s PASS  %4ss  (contract, unbounded) %s\n' "$name" $((t1 - t0)) \
            "$(grep -Eo '[0-9]+ of [0-9]+ failed' <<<"$out" | tail -1)"
    else
        printf '%-26s FAIL  %4ss  (contract)\n' "$name" $((t1 - t0))
        grep -E 'FAILURE|ERROR|error' <<<"$out" | head -20
        fail=1
    fi
}

# ---------------------------------------------------------------- UTF-8
proof utf8_decode "utf8_decode" \
    $D/utf8_decode.c $U $S -I src --function harness --unwind 6
proof utf8_encode "utf8_encode" \
    $D/utf8_encode.c $U $S -I src --function harness --unwind 6
proof utf8_next "utf8_next" \
    $D/utf8_next.c $U $S -I src --function harness --unwind 6
proof utf8_prev "utf8_prev" \
    $D/utf8_prev.c $U $S -I src --function harness --unwind 5
proof utf8_count_offset "utf8_count utf8_offset" \
    $D/utf8_count_offset.c $U $S -I src --function harness -DCBMC_LEN=8 --unwind 10
proof char_width "char_width in_table" \
    $D/char_width.c $S -I src --function harness --unwind 60
proof char_width_tables "harness_tables" \
    $D/char_width.c $S -I src --function harness_tables --unwind 2
proof misc_int "is_word_char is_space_char clampi mini maxi digits" \
    $D/misc_int.c $U $S -I src --function harness --unwind 12 -D__NO_CTYPE

# ------------------------------------------------- UTF-8, unbounded contracts
contract utf8_decode_contract utf8_decode
contract utf8_encode_contract utf8_encode
contract utf8_next_contract utf8_next
contract utf8_prev_contract utf8_prev
contract char_width_contract char_width

# ---------------------------------------------------------------- Str
# Safety proofs: capacity, lengths and counts fully symbolic up to INT_MAX;
# memcpy/memmove replaced by checking+havocking models (stubs.c,
# CBMC_ABSTRACT_COPY) so the array solver copes with INT_MAX-sized objects.
SAFE="-DCBMC_ABSTRACT_COPY"
proof str_reserve "str_reserve" \
    $D/str_reserve.c $U $S -I src --function harness --unwind 34 $MAYFAIL $SAFE
proof str_putn "str_putn str_reserve" \
    $D/str_putn.c $U $S -I src --function harness --unwind 34 $MAYFAIL $SAFE
proof str_puts_putc "str_puts str_putc" \
    $D/str_puts.c $U $S -I src --function harness --unwind 34 $MAYFAIL $SAFE
proof str_printf "str_printf" \
    $D/str_printf.c $U $S -I src --function harness --unwind 34 $MAYFAIL $SAFE
proof str_insert "str_insert" \
    $D/str_insert.c $U $S -I src --function harness --unwind 34 $MAYFAIL $SAFE
proof str_delete "str_delete" \
    $D/str_delete.c $U $S -I src --function harness --unwind 2 $SAFE
proof str_lifecycle "str_init str_clear str_free str_detach" \
    $D/str_lifecycle.c $U $S -I src --function harness --unwind 2 $MAYFAIL $SAFE
# Content proofs: capacity and byte counts <= 8, byte-loop copy models,
# additionally asserting the resulting buffer CONTENTS (CONTENT_ASSERT).
CONT="-DCBMC_LOOP_COPY -DCBMC_CONTENT -DCBMC_CAP=8"
proof str_reserve_content "str_reserve" \
    $D/str_reserve.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT
proof str_putn_content "str_putn" \
    $D/str_putn.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT
proof str_puts_putc_content "str_puts str_putc" \
    $D/str_puts.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT
proof str_printf_content "str_printf" \
    $D/str_printf.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT
proof str_insert_content "str_insert xstrndup" \
    $D/str_insert.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT
proof str_delete_content "str_delete" \
    $D/str_delete.c $U $S -I src --function harness --unwind 10 $CONT
proof str_lifecycle_content "str_detach" \
    $D/str_lifecycle.c $U $S -I src --function harness --unwind 10 $MAYFAIL $CONT

# ---------------------------------------------------------------- misc
proof xalloc "xmalloc xcalloc xrealloc xstrdup xstrndup xsprintf" \
    $D/xalloc.c $U $S -I src --function harness --unwind 10 $MAYFAIL --cvc4
proof strcmp_family "streq strieq starts_with ends_with" \
    $D/strcmp_family.c $U $S -I src --function harness -DCBMC_SLEN=16 --unwind 18 -D__NO_CTYPE
proof path "path_last_sep path_base path_dir path_join expand_home" \
    $D/path.c $U $S -I src --function harness -DCBMC_SLEN=16 --unwind 18 $MAYFAIL
proof read_file "read_file" \
    $D/read_file.c $U $S -I src --function harness -DCBMC_FILE_MAX=65536 \
    --unwindset read_file.0:18,str_reserve.0:33 --unwind 2 $MAYFAIL
proof read_file_large "read_file" \
    $D/read_file.c $U $S -I src --function harness -DCBMC_LARGE_HINT \
    --unwindset read_file.0:6,str_reserve.0:33 --unwind 2 $MAYFAIL
# glob_match: one run per pair of exact lengths (see glob_match.c).
GM_MAX=${GM_MAX:-4}
for P in $(seq 0 $GM_MAX); do
    for L in $(seq 0 $GM_MAX); do
        proof glob_match_p${P}_s$L "glob_match glob_class" \
            $D/glob_match.c $S -I src --function harness -DCBMC_EXACT -DCBMC_LOOP_COPY \
            -DCBMC_PLEN=$P -DCBMC_SLEN=$L --unwind $((GM_MAX + 2))
    done
done
for P in 0 1 2; do
    for L in 0 1 2; do
        proof glob_match_ref_p${P}_s$L "glob_match glob_class" \
            $D/glob_match.c $S -I src --function harness -DCBMC_EXACT -DCBMC_LOOP_COPY -DCBMC_REF \
            -DCBMC_PLEN=$P -DCBMC_SLEN=$L --unwind 6 --object-bits 12
    done
done
for L in 255 256; do
    proof glob_match_heap_p0_s$L "glob_match glob_class" \
        $D/glob_match.c $S -I src --function harness_heap \
        -DCBMC_PLEN=0 -DCBMC_SLEN=$L --unwind 260 --object-bits 10
done
# shell_split: one run per exact input length L = 0..SS_MAX (together:
# every line of <= SS_MAX bytes), bytes drawn from the six classes the
# function distinguishes (data independence, see shell_split.c), loop
# bounds derived from L (unwinding assertions prove them), str_putc
# replaced by its asserted fast path (see str_putc_model).  No
# --malloc-may-fail here: shell_split allocates only through the x*
# wrappers, which never return NULL (proved in xalloc); with it CBMC 5.95
# produced an inconsistent counterexample (memcpy in xstrdup reading a
# byte value the trace shows was never written), see the report.
SS_MAX=${SS_MAX:-5}
for L in $(seq 0 $SS_MAX); do
    proofgi shell_split_len$L "shell_split free_strv" \
        "-DCBMC_EXACT -DCBMC_SLEN=$L $D/shell_split.c $U $S" \
        "--replace-calls str_putc:str_putc_model" \
        --function harness --unwind $((L + 2)) \
        --unwindset str_reserve.0:1,shell_split.4:$(( (L + 1) / 2 + 1 )),shell_split.0:$((L + 1)),shell_split.3:$((L + 1)),shell_split.1:$((L + 1)),shell_split.2:$((L + 1))
done

if [ $MODE = prove ]; then
    if [ $fail -ne 0 ]; then echo "util CBMC proofs: FAILED"; exit 1; fi
    echo "util CBMC proofs: all passed"
fi
exit 0
