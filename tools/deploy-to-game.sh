#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# OptiScaler & DLSS-NR Game Deployment Script
# Targets: Linux/Proton/Wine gaming environments with Steam / Lutris / Heroic
#
#   tools/deploy-to-game.sh [options] <game-directory> [proxy-dll-name]
#   tools/deploy-to-game.sh --uninstall [--dry-run] <game-directory>
#
# Each refusal below is a failure this fork has already paid for once (CLAUDE.md):
#   - A proxy DLL overwritten while its game ran killed Cyberpunk two minutes later (2026-09-19).
#     The guard then was `pgrep -x Cyberpunk2077.exe`, which never matched: comm is cut at 15
#     characters. This script reads /proc/<pid>/maps for any file under the game directory.
#   - Two proxy names holding OptiScaler in one directory load two instances whose hooks fight
#     (Divinity: dxgi.dll and winmm.dll, under GE-Proton).
#   - A key added to OptiScaler.ini beside the shipped one is silently overridden, because the
#     parser keeps the last one. Keys are replaced in place, never added next to an existing one.
# And a game directory may hold someone else's DLL under our name (ReShade as dxgi.dll, a mod).
# That is never replaced without being asked, and never without a recorded copy.
#
# The receipt, <game>/.retoque/receipt.txt, records everything a deploy wrote, set aside or
# renamed; --uninstall reads it and touches nothing else. One record per line, tab-separated,
# paths relative to the game directory:
#   version  1
#   status   complete | partial (a deploy stopped part-way) | uninstall-incomplete
#   updated  <UTC timestamp>
#   proxy    <name>                         a proxy name deployed here
#   file     <sha256>  <path>               a file this script wrote, as it wrote it
#   backup   <sha256>  <path>  <copy>       what <path> held before; <copy> is under .retoque/backup/
#   rename   <sha256>  <from>  <to>         <from> was renamed to <to>; <sha256> is its content
# It is rewritten (temp file, then rename) after every step, so a run that dies part-way leaves a
# receipt that lists exactly what was done. Backups live under .retoque/backup/<UTC stamp>/, never
# as *.bak siblings: a sibling DLL is one more thing Wine might load.
#
# Exit status: 0 done (or a dry run that would go ahead), 1 usage or error, 3 a process maps files
# in the game directory (the game is running), 4 a file in the way is not ours, 5 another proxy name
# in the directory already holds OptiScaler.
#
# RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP=<n> stops the run with exit 1 after its n-th step, as a failed
# copy would. It exists for tests/deploy-safety, which checks the partial receipt that leaves.
# ==============================================================================

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # this script lives in tools/
INI_SRC="${REPO_ROOT}/OptiScaler.ini"

# Names OptiScaler answers to when the game loads it (dllmain.cpp, dllNames). Any of them in the game
# directory can be loaded, which is how Divinity ended up with two instances.
PROXY_NAMES=(dxgi.dll winmm.dll version.dll dbghelp.dll d3d12.dll winhttp.dll wininet.dll)

# What a DLL is, from its bytes. "OptiScaler working as" is CheckWorkingMode's log line, in every
# OptiScaler build, upstream's included. "dlssnr_call_" prefixes the forwarder's exports. OptiScaler
# itself contains "ReShade" (it loads ReShade64.dll), so ReShade is only tested after OptiScaler has
# been ruled out.
MARK_OPTISCALER='OptiScaler working as'
MARK_FORWARDER='dlssnr_call_'
MARK_RESHADE='ReShade'

FORWARDER_NAME='nvngx.dll_dlssnr.dll'
INI_NAME='OptiScaler.ini'

usage() {
    cat <<EOF
Usage: $0 [options] <game-target-directory> [proxy-dll-name]
       $0 --uninstall [--dry-run] <game-target-directory>

Arguments:
  <game-target-directory>   Path to the directory where the game executable lives
  [proxy-dll-name]          Name of proxy DLL (default: dxgi.dll)
                            Options: dxgi.dll, winmm.dll, version.dll, d3d12.dll, dbghelp.dll,
                                     winhttp.dll, wininet.dll, OptiScaler.dll, <name>.asi

Options:
  -n, --dry-run             Print what would be done; write nothing
      --uninstall           Remove what .retoque/receipt.txt records (only files whose hash still
                            matches), restore what it set aside, undo its renames
      --keep-reshade        The proxy name holds ReShade: rename it to ReShade64.dll and set
                            [Plugins] LoadReshade=true so OptiScaler loads it
      --replace-foreign     Replace a file at one of our names that is not ours; the original is
                            kept under .retoque/backup/ and --uninstall puts it back
      --allow-second-proxy  Deploy even though another proxy name here already holds OptiScaler
      --source-dir DIR      Take OptiScaler.dll and ${FORWARDER_NAME} from DIR (default: x64/out)
  -h, --help                This text

Refuses, writing nothing, while any process maps a file in the game directory.

Examples:
  $0 "~/.local/share/Steam/steamapps/common/Cyberpunk 2077/bin/x64" dxgi.dll
  $0 "~/.local/share/Steam/steamapps/common/Crimson Desert/bin64" dxgi.dll
  $0 --uninstall "~/.local/share/Steam/steamapps/common/Crimson Desert/bin64"
EOF
    exit "${1:-1}"
}

die() {
    local code=$1
    shift
    echo "[-] Error: $*" >&2
    exit "$code"
}

# ------------------------------------------------------------------------------
# Arguments
# ------------------------------------------------------------------------------

DRY_RUN=0 UNINSTALL=0 KEEP_RESHADE=0 REPLACE_FOREIGN=0 ALLOW_SECOND_PROXY=0 SRC_DIR=''
POSITIONAL=()
while (( $# )); do
    case $1 in
        -n | --dry-run) DRY_RUN=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --keep-reshade) KEEP_RESHADE=1 ;;
        --replace-foreign) REPLACE_FOREIGN=1 ;;
        --allow-second-proxy) ALLOW_SECOND_PROXY=1 ;;
        --source-dir)
            (( $# >= 2 )) || usage
            SRC_DIR=$2
            shift
            ;;
        --source-dir=*) SRC_DIR=${1#*=} ;;
        -h | --help) usage 0 ;;
        --)
            shift
            POSITIONAL+=("$@")
            break
            ;;
        -*)
            echo "[-] Unknown option: $1" >&2
            usage
            ;;
        *) POSITIONAL+=("$1") ;;
    esac
    shift
done

if (( ${#POSITIONAL[@]} < 1 || ${#POSITIONAL[@]} > 2 )); then
    usage
fi
if (( UNINSTALL && ${#POSITIONAL[@]} > 1 )); then
    die 1 "--uninstall takes only the game directory; the proxy name comes from the receipt"
fi

TARGET_DIR="${POSITIONAL[0]}"
PROXY_NAME="${POSITIONAL[1]:-dxgi.dll}"

# Expand tilde if present
TARGET_DIR="${TARGET_DIR/#\~/$HOME}"

if [[ ! -d "${TARGET_DIR}" ]]; then
    echo "[-] Error: Target directory does not exist: ${TARGET_DIR}" >&2
    exit 1
fi
# /proc/<pid>/maps shows resolved paths, so the directory is compared in its resolved form too.
TARGET_DIR="$(cd -P -- "${TARGET_DIR}" && pwd -P)"

case ${PROXY_NAME,,} in
    */* | *\\*) die 1 "the proxy name is a file name, not a path: ${PROXY_NAME}" ;;
    dxgi.dll | winmm.dll | version.dll | dbghelp.dll | d3d12.dll | winhttp.dll | wininet.dll) ;;
    optiscaler.dll | ?*.asi) ;;
    *) die 1 "${PROXY_NAME} is not a name OptiScaler answers to (see --help)" ;;
esac

STATE_DIR="${TARGET_DIR}/.retoque"
RECEIPT="${STATE_DIR}/receipt.txt"
FILE_MODE=$(printf '%o' $(( 0666 & ~0$(umask) )))

# ------------------------------------------------------------------------------
# Small helpers
# ------------------------------------------------------------------------------

# sha256 of a regular file's content, or "-" when there is none to read (gone, dangling link).
sha_of() {
    local h
    if [[ -f $1 && -r $1 ]]; then
        h=$(sha256sum < "$1")
        printf '%s' "${h%% *}"
    else
        printf '%s' '-'
    fi
}

has_marker() { grep -a -q -F -- "$2" "$1"; }

# Every entry of TARGET_DIR whose name equals $1 ignoring case, into CI_HITS. Wine resolves DLL names
# without regard to case, so a DXGI.DLL beside a new dxgi.dll is two candidates for one load. The
# first letter goes in a bracket so the word is a pattern at all; a name with no glob character is
# never matched against the directory.
ci_lookup() {
    local name=$1
    shopt -s nocaseglob nullglob
    CI_HITS=("${TARGET_DIR}"/["${name:0:1}"]"${name:1}")
    shopt -u nocaseglob nullglob
    CI_HITS=("${CI_HITS[@]##*/}")
}

exists() { [[ -e ${TARGET_DIR}/$1 || -L ${TARGET_DIR}/$1 ]]; }

# A receipt path must stay inside the game directory: relative, no "." or ".." component, no tab or
# newline (the record separators). A receipt is plain text in a game folder; it is not trusted further.
safe_rel() {
    [[ -n $1 && $1 != /* && $1 != *$'\t'* && $1 != *$'\n'* ]] || return 1
    case /$1/ in
        *//* | */../* | */./*) return 1 ;;
    esac
}

# ------------------------------------------------------------------------------
# Receipt
# ------------------------------------------------------------------------------

declare -A FILE_SHA=()
FILE_ORDER=() BACKUPS=() RENAMES=() PROXIES=()
OLD_STATUS=''
RECEIPT_OPEN=0     # 1 while a run is changing the directory; the EXIT trap reports then
INCOMING=''        # a temp file not yet renamed into place, removed on exit

remember_file() {
    if [[ -z ${FILE_SHA[$1]+set} ]]; then
        FILE_ORDER+=("$1")
    fi
    FILE_SHA[$1]=$2
}

forget_file() {
    local rel=$1 r
    local -a kept=()
    unset "FILE_SHA[$rel]"
    for r in "${FILE_ORDER[@]}"; do
        if [[ $r != "$rel" ]]; then kept+=("$r"); fi
    done
    FILE_ORDER=("${kept[@]}")
}

drop_line() {   # drop_line ARRAY LINE
    local -n arr=$1
    local -a kept=()
    local l
    for l in "${arr[@]}"; do
        if [[ $l != "$2" ]]; then kept+=("$l"); fi
    done
    arr=("${kept[@]}")
}

add_proxy() {
    local p
    for p in "${PROXIES[@]}"; do
        if [[ $p == "$1" ]]; then return 0; fi
    done
    PROXIES+=("$1")
}

load_receipt() {
    local kind a b c
    [[ -f $RECEIPT ]] || return 0
    while IFS=$'\t' read -r kind a b c || [[ -n $kind ]]; do
        case $kind in
            '' | '#'*) ;;
            version) [[ $a == 1 ]] || die 1 "${RECEIPT} is version '${a}'; this script reads version 1" ;;
            status) OLD_STATUS=$a ;;
            updated) ;;
            proxy) add_proxy "$a" ;;
            file)
                safe_rel "$b" || die 1 "unsafe path '${b}' in ${RECEIPT}"
                remember_file "$b" "$a"
                ;;
            backup | rename)
                { safe_rel "$b" && safe_rel "$c"; } || die 1 "unsafe path in ${RECEIPT}: ${b} ${c}"
                if [[ $kind == backup ]]; then
                    BACKUPS+=("${a}"$'\t'"${b}"$'\t'"${c}")
                else
                    RENAMES+=("${a}"$'\t'"${b}"$'\t'"${c}")
                fi
                ;;
            *) die 1 "unrecognised record '${kind}' in ${RECEIPT}" ;;
        esac
    done < "$RECEIPT"
}

# Written whole to a temp file in the same directory and renamed over the old one, so a reader (or a
# crash) sees the previous receipt or the new one, never half of one.
write_receipt() {
    local status=$1 tmp rel line p
    tmp=$(mktemp "${STATE_DIR}/receipt.XXXXXX")
    {
        printf '# Retoque deploy receipt: what tools/deploy-to-game.sh wrote, set aside or renamed here.\n'
        printf '# --uninstall reads it. Tab-separated; paths are relative to the game directory.\n'
        printf 'version\t1\nstatus\t%s\nupdated\t%s\n' "$status" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        for p in "${PROXIES[@]}"; do printf 'proxy\t%s\n' "$p"; done
        for rel in "${FILE_ORDER[@]}"; do printf 'file\t%s\t%s\n' "${FILE_SHA[$rel]}" "$rel"; done
        for line in "${BACKUPS[@]}"; do printf 'backup\t%s\n' "$line"; done
        for line in "${RENAMES[@]}"; do printf 'rename\t%s\n' "$line"; done
    } > "$tmp"
    chmod "$FILE_MODE" "$tmp"
    mv -f -T -- "$tmp" "$RECEIPT"
}

RUN_MODE=deploy
on_exit() {
    local rc=$?
    if [[ -n $INCOMING ]]; then rm -f -- "$INCOMING"; fi
    if (( rc != 0 && RECEIPT_OPEN )); then
        if [[ $RUN_MODE == deploy ]]; then
            write_receipt partial 2>/dev/null || true
            echo "[-] Deploy stopped part-way (exit ${rc}). Everything it did is in ${RECEIPT}" \
                "(status partial); --uninstall takes it back out, or run the deploy again." >&2
        else
            write_receipt uninstall-incomplete 2>/dev/null || true
            echo "[-] Uninstall stopped part-way (exit ${rc}). ${RECEIPT} lists what is left." >&2
        fi
    fi
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ------------------------------------------------------------------------------
# Is the game running?
# ------------------------------------------------------------------------------

REFUSALS=()
REFUSE_CODE=0
refuse() {
    REFUSALS+=("$2")
    if (( REFUSE_CODE == 0 )); then REFUSE_CODE=$1; fi
}

# Processes that map any file under TARGET_DIR, as "pid<TAB>comm<TAB>exe<TAB>path" in MAPPERS.
#
# Not pgrep: comm is cut at 15 characters ("Cyberpunk2077.e"). A game under Wine maps every DLL it
# loaded from its directory, ours included, so any path under the directory in /proc/<pid>/maps means
# the game is up. Another user's maps cannot be read; a Proton game runs as this user, so those are
# counted and reported rather than treated as a reason to refuse. One grep reads every maps file;
# its own error lines are how unreadable ones are counted.
find_mappers() {
    local line pid comm exe hit
    MAPPERS=()
    UNREADABLE_MAPS=0
    while IFS= read -r line; do
        case $line in
            /proc/*/maps)
                pid=${line#/proc/}
                pid=${pid%/maps}
                hit=$(grep -a -F -m1 -- " ${TARGET_DIR}/" "$line" 2>/dev/null) || continue
                hit=${hit#*" ${TARGET_DIR}/"}
                comm=$(cat "/proc/${pid}/comm" 2>/dev/null) || comm='?'
                exe=''
                { IFS= read -r -d '' exe < "/proc/${pid}/cmdline"; } 2>/dev/null || true
                exe=${exe##*[/\\]}
                MAPPERS+=("${pid}"$'\t'"${comm}"$'\t'"${exe:-?}"$'\t'"${hit}")
                ;;
            *'Permission denied'*) UNREADABLE_MAPS=$((UNREADABLE_MAPS + 1)) ;;
        esac
    done < <(LC_ALL=C grep -a -F -l -- "${TARGET_DIR}/" /proc/[0-9]*/maps 2>&1 || true)
}

check_not_running() {
    local m pid comm exe path msg
    find_mappers
    if (( UNREADABLE_MAPS > 0 )); then
        echo "[=] ${UNREADABLE_MAPS} process(es) of other users could not be checked (their maps are not readable)"
    fi
    if (( ${#MAPPERS[@]} )); then
        msg="a running process maps files in this directory; overwriting them is how Cyberpunk died on 2026-09-19:"
        for m in "${MAPPERS[@]}"; do
            IFS=$'\t' read -r pid comm exe path <<< "$m"
            msg+=$'\n'"      pid ${pid} (comm '${comm}', exe '${exe}') maps ${path}"
        done
        msg+=$'\n'"    Quit the game, wait for it to exit, then run this again."
        refuse 3 "$msg"
    fi
}

# ------------------------------------------------------------------------------
# OptiScaler.ini, read the way simpleini reads it: section and key names compare without case, ';' and
# '#' start comment lines, and the last of several equal keys wins.
# ------------------------------------------------------------------------------

RE_INI_HEADER='^[[:space:]]*\[([^]]*)\][[:space:]]*$'
RE_INI_KEY='^[[:space:]]*([^=;#[:space:]][^=]*)=(.*)$'
INI_HEADER='' INI_KEY='' INI_VALUE='' INI_VALUES=()   # set through trim_into's nameref

trim_into() {   # trim_into VAR TEXT
    local -n out=$1
    local s=$2
    s=${s#"${s%%[![:space:]]*}"}
    s=${s%"${s##*[![:space:]]}"}
    out=$s
}

ini_header() { [[ $1 =~ $RE_INI_HEADER ]] && trim_into INI_HEADER "${BASH_REMATCH[1]}"; }

ini_key() {
    [[ $1 =~ $RE_INI_KEY ]] || return 1
    local v=${BASH_REMATCH[2]}
    trim_into INI_KEY "${BASH_REMATCH[1]}"
    trim_into INI_VALUE "$v"
}

# The values KEY has in [SECTION] of FILE, into INI_VALUES.
ini_scan() {
    local file=$1 want_section=${2,,} want_key=${3,,} line section=''
    INI_VALUES=()
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if ini_header "$line"; then
            section=${INI_HEADER,,}
        elif [[ $section == "$want_section" ]] && ini_key "$line" && [[ ${INI_KEY,,} == "$want_key" ]]; then
            INI_VALUES+=("$INI_VALUE")
        fi
    done < "$file"
}

# FILE with KEY=VALUE in [SECTION], written to OUT. Every existing KEY line in the section gets the
# value, keeping its own spelling and line ending; only when the section has no such key at all is a
# line added (right under the header), so the result never holds two of it. A missing section is
# appended.
ini_write() {
    local in=$1 out=$2 section_name=$3 key=$4 value=$5
    local want_section=${3,,} want_key=${4,,} line body cr section='' insert=0 done_insert=0
    ini_scan "$in" "$section_name" "$key"
    if (( ${#INI_VALUES[@]} == 0 )); then insert=1; fi
    {
        while IFS= read -r line || [[ -n $line ]]; do
            body=${line%$'\r'}
            cr=${line:${#body}}
            if ini_header "$body"; then
                section=${INI_HEADER,,}
                printf '%s\n' "$line"
                if (( insert && ! done_insert )) && [[ $section == "$want_section" ]]; then
                    printf '%s=%s%s\n' "$key" "$value" "$cr"
                    done_insert=1
                fi
                continue
            fi
            if [[ $section == "$want_section" ]] && ini_key "$body" && [[ ${INI_KEY,,} == "$want_key" ]]; then
                printf '%s=%s%s\n' "${body%%=*}" "$value" "$cr"
                continue
            fi
            printf '%s\n' "$line"
        done < "$in"
        if (( insert && ! done_insert )); then
            printf '\n[%s]\n%s=%s\n' "$section_name" "$key" "$value"
        fi
    } > "$out"
}

# ------------------------------------------------------------------------------
# The plan: every change is decided, and every refusal found, before the first write. A dry run
# prints it; a real run carries it out one step at a time, recording each step in the receipt.
# ------------------------------------------------------------------------------

PLAN_KIND=() PLAN_A=() PLAN_B=() PLAN_C=() PLAN_TEXT=()
plan() {
    PLAN_KIND+=("$1")
    PLAN_A+=("$2")
    PLAN_B+=("$3")
    PLAN_C+=("$4")
    PLAN_TEXT+=("$5")
}

BACKUP_REL=''
choose_backup_dir() {
    local base n=1
    base=".retoque/backup/$(date -u +%Y%m%dT%H%M%SZ)"
    BACKUP_REL=$base
    while exists "$BACKUP_REL"; do
        n=$((n + 1))
        BACKUP_REL="${base}-${n}"
    done
}

# What the file at REL is, into CLASS (and CLASS_WHY for a refusal):
#   absent       nothing there
#   same         byte-identical to what is about to be installed
#   ours         what an earlier deploy wrote, unchanged since (the receipt's hash)
#   known        carries MARKER: an OptiScaler build or forwarder this script has no record of
#   reshade      ReShade
#   foreign      anything else, or a symlink
#   casevariant  the name exists in another case; Wine would see two files for one name
#   notfile      a directory or other non-file
classify() {
    local rel=$1 marker=$2 src_sha=$3 path="${TARGET_DIR}/$1" sha hit
    CLASS=absent
    CLASS_WHY=''
    ci_lookup "$rel"
    for hit in "${CI_HITS[@]}"; do
        if [[ $hit != "$rel" ]]; then
            CLASS=casevariant
            CLASS_WHY="${hit} is already here, and to Wine it is the same name"
            return 0
        fi
    done
    if [[ -L $path ]]; then
        CLASS=foreign
        CLASS_WHY='it is a symlink'
    elif [[ ! -e $path ]]; then
        CLASS=absent
    elif [[ ! -f $path ]]; then
        CLASS=notfile
        CLASS_WHY='it is not a regular file'
    else
        sha=$(sha_of "$path")
        if [[ -n $src_sha && $sha == "$src_sha" ]]; then
            CLASS=same
        elif [[ ${FILE_SHA[$rel]:-} == "$sha" ]]; then
            CLASS=ours
        elif has_marker "$path" "$marker"; then
            CLASS=known
        elif has_marker "$path" "$MARK_RESHADE"; then
            CLASS=reshade
        else
            CLASS=foreign
            CLASS_WHY='it is not an OptiScaler build, and no deploy here recorded it'
        fi
    fi
}

RESHADE_ROUTE=0

plan_keep_reshade() {
    local rel=$1 new taken
    case ${rel,,} in
        dxgi.dll) new='ReShade64.dll' ;;
        # ReShade picks its API from its own file name; OptiScaler accepts this one for that reason.
        d3d12.dll) new='d3d12-ReShade64.dll' ;;
        *)
            refuse 4 "${rel} is ReShade; --keep-reshade handles ReShade installed as dxgi.dll or d3d12.dll only"
            return 0
            ;;
    esac
    ci_lookup ReShade64.dll
    taken=("${CI_HITS[@]}")
    ci_lookup d3d12-ReShade64.dll
    taken+=("${CI_HITS[@]}")
    if (( ${#taken[@]} )); then
        refuse 4 "${rel} is ReShade, but ${taken[*]} is already here and OptiScaler would load that one. Move one of them aside first."
        return 0
    fi
    plan rename "$rel" "$new" '' "rename ${rel} (ReShade) to ${new}; OptiScaler loads it from there"
    RESHADE_ROUTE=1
}

plan_dll() {
    local rel=$1 src=$2 marker=$3 src_sha=$4 is_proxy=$5 r
    local replace_hint="Pass --replace-foreign to set it aside under ${BACKUP_REL}/ and install ours"
    classify "$rel" "$marker" "$src_sha"
    case $CLASS in
        absent) plan install "$src" "$rel" '' "install ${src##*/} as ${rel}" ;;
        same) plan keep "$rel" '' '' "${rel}: already this build, left as it is" ;;
        ours) plan install "$src" "$rel" '' "replace ${rel} (deployed here earlier, unchanged since)" ;;
        known)
            plan backup "$rel" '' '' "back up ${rel} (an earlier build no deploy here recorded) to ${BACKUP_REL}/"
            plan install "$src" "$rel" '' "install ${src##*/} as ${rel}"
            ;;
        reshade)
            if (( is_proxy && KEEP_RESHADE )); then
                plan_keep_reshade "$rel"
                plan install "$src" "$rel" '' "install ${src##*/} as ${rel}"
            elif (( REPLACE_FOREIGN )); then
                plan backup "$rel" '' '' "back up ${rel} (ReShade) to ${BACKUP_REL}/"
                plan install "$src" "$rel" '' "install ${src##*/} as ${rel}"
            else
                r="${rel} is ReShade."
                if (( is_proxy )); then
                    r+=" Pass --keep-reshade to rename it to ReShade64.dll and have OptiScaler load it, or"
                    r+=" --replace-foreign to set it aside under ${BACKUP_REL}/."
                else
                    r+=" ${replace_hint}."
                fi
                refuse 4 "$r"
            fi
            ;;
        foreign)
            if (( REPLACE_FOREIGN )); then
                plan backup "$rel" '' '' "back up ${rel} (${CLASS_WHY#it is }) to ${BACKUP_REL}/"
                plan install "$src" "$rel" '' "install ${src##*/} as ${rel}"
            else
                r="${rel} is not ours: ${CLASS_WHY}."
                if (( is_proxy && KEEP_RESHADE )); then r+=" It is not ReShade either, so --keep-reshade does not apply."; fi
                refuse 4 "${r} ${replace_hint}."
            fi
            ;;
        casevariant | notfile) refuse 4 "${rel}: ${CLASS_WHY}. Rename or move it, then run this again." ;;
    esac
    if (( is_proxy && KEEP_RESHADE && ! RESHADE_ROUTE )) && [[ $CLASS == same || $CLASS == ours ]]; then
        # An earlier --keep-reshade deploy: the rename is in the receipt; keep the ini key true.
        for r in "${RENAMES[@]}"; do
            if [[ ${r,,} == *reshade64.dll ]]; then RESHADE_ROUTE=1; fi
        done
        if (( ! RESHADE_ROUTE )); then echo "[=] --keep-reshade: no ReShade at ${rel}, nothing to keep"; fi
    fi
}

check_other_proxies() {
    local name hit
    for name in "${PROXY_NAMES[@]}"; do
        if [[ ${name,,} == "${PROXY_NAME,,}" ]]; then continue; fi
        ci_lookup "$name"
        for hit in "${CI_HITS[@]}"; do
            [[ -f ${TARGET_DIR}/${hit} ]] || continue
            has_marker "${TARGET_DIR}/${hit}" "$MARK_OPTISCALER" || continue
            if (( ALLOW_SECOND_PROXY )); then
                echo "[!] ${hit} also holds an OptiScaler build; deploying anyway (--allow-second-proxy)." \
                    "If the game loads both, their hooks fight."
            else
                refuse 5 "${hit} already holds an OptiScaler build, and ${PROXY_NAME} would be a second one in this directory. Depending on the Proton build both load, and their hooks fight (Divinity, 2026-09-19). Run --uninstall, or move ${hit} aside (e.g. to ${hit}.off), or pass --allow-second-proxy."
            fi
        done
    done
}

plan_ini() {
    local ini_rel=$INI_NAME path input sha how
    ci_lookup "$INI_NAME"
    if (( ${#CI_HITS[@]} )); then ini_rel=${CI_HITS[0]}; fi
    path="${TARGET_DIR}/${ini_rel}"

    # Copy OptiScaler.ini if not present (preserve user customized configs)
    if ! exists "$ini_rel"; then
        input=/dev/null
        if [[ -f "${INI_SRC}" ]]; then
            plan install "$INI_SRC" "$INI_NAME" '' "install the default ${INI_NAME}"
            input=$INI_SRC
        fi
    else
        echo "[!] Preserving existing ${path}"
        input=$path
    fi

    (( RESHADE_ROUTE )) || return 0
    ini_scan "$input" Plugins LoadReshade
    if (( ${#INI_VALUES[@]} == 1 )) && [[ ${INI_VALUES[0],,} == true ]]; then
        echo "[=] ${ini_rel}: [Plugins] LoadReshade is already true"
        return 0
    fi
    if exists "$ini_rel"; then
        sha=$(sha_of "$path")
        if [[ ${FILE_SHA[$ini_rel]:-} != "$sha" ]]; then
            plan backup "$ini_rel" '' '' "back up ${ini_rel} to ${BACKUP_REL}/ before editing it"
        fi
    fi
    if (( ${#INI_VALUES[@]} )); then
        how="replacing the ${#INI_VALUES[@]} line(s) it already has"
    else
        how="the section has no such key, so one line is added"
    fi
    plan setini "$ini_rel" Plugins LoadReshade=true "set [Plugins] LoadReshade=true in ${ini_rel} (${how})"
}

# Stand our libxell down when the game ships its own.
#
# OptiScaler bundles libxell.dll (~415 KB). Several games ship a different one, and
# XeLL_Proxy::InitXeLLProper points one at the other with RedirectAllExports; a missing export
# becomes a null function pointer. That crashed Dying Light: The Beast at startup roughly three
# launches in four, and three Streamline minidumps all showed 0xC0000005 at address 0x0 with the
# return address in libxell.dll. Renamed, never deleted, so it is one mv to undo (and --uninstall
# does it, since the rename is in the receipt).
plan_libxell() {
    local ours='OptiScaler/libxell.dll' theirs='libxell.dll'
    if [[ -f "${TARGET_DIR}/${theirs}" && -f "${TARGET_DIR}/${ours}" ]]; then
        if [[ $(sha_of "${TARGET_DIR}/${theirs}") == "$(sha_of "${TARGET_DIR}/${ours}")" ]]; then
            echo "[=] libxell.dll: the game's copy is ours, leaving it"
        elif exists "${ours}.disabled"; then
            echo "[!] libxell.dll: ours and ${ours}.disabled both exist; leaving both as they are"
        else
            plan rename "$ours" "${ours}.disabled" '' "libxell.dll: the game ships its own, ours renamed to .disabled"
        fi
    elif [[ -f "${TARGET_DIR}/${ours}.disabled" ]]; then
        echo "[=] libxell.dll: ours already disabled"
    fi
}

# ------------------------------------------------------------------------------
# Steps
# ------------------------------------------------------------------------------

# Copied beside the target, then renamed over it. A rename replaces the directory entry and leaves
# the old inode whole for anything still mapping it; cp truncates and rewrites that inode in place,
# which is the Cyberpunk failure. The running-game refusal comes first anyway; this is the second line.
put_file() {
    local src=$1 rel=$2
    INCOMING=$(mktemp "${STATE_DIR}/incoming.XXXXXX")
    cp -- "$src" "$INCOMING"
    chmod "$FILE_MODE" "$INCOMING"
    mv -f -T -- "$INCOMING" "${TARGET_DIR}/${rel}"
    INCOMING=''
}

do_backup() {
    local rel=$1 sha copy
    copy="${BACKUP_REL}/${rel}"
    mkdir -p -- "${TARGET_DIR}/${copy%/*}"
    sha=$(sha_of "${TARGET_DIR}/${rel}")
    cp -P -p -- "${TARGET_DIR}/${rel}" "${TARGET_DIR}/${copy}"
    BACKUPS+=("${sha}"$'\t'"${rel}"$'\t'"${copy}")
}

do_rename() {
    local from=$1 to=$2 sha
    if exists "$to"; then die 1 "${to} appeared while deploying; stopping"; fi
    sha=$(sha_of "${TARGET_DIR}/${from}")
    mv -T -- "${TARGET_DIR}/${from}" "${TARGET_DIR}/${to}"
    RENAMES+=("${sha}"$'\t'"${from}"$'\t'"${to}")
}

do_setini() {
    local rel=$1 section=$2 kv=$3 in="${TARGET_DIR}/$1"
    [[ -e $in ]] || in=/dev/null
    INCOMING=$(mktemp "${STATE_DIR}/incoming.XXXXXX")
    ini_write "$in" "$INCOMING" "$section" "${kv%%=*}" "${kv#*=}"
    if [[ $in != /dev/null ]]; then
        chmod --reference="$in" "$INCOMING"
    else
        chmod "$FILE_MODE" "$INCOMING"
    fi
    mv -f -T -- "$INCOMING" "${TARGET_DIR}/${rel}"
    INCOMING=''
    remember_file "$rel" "$(sha_of "${TARGET_DIR}/${rel}")"
}

do_step() {
    local kind=$1 a=$2 b=$3 c=$4 sha rel copy from to
    case $kind in
        install)
            put_file "$a" "$b"
            remember_file "$b" "$(sha_of "${TARGET_DIR}/${b}")"
            ;;
        keep) remember_file "$a" "$(sha_of "${TARGET_DIR}/${a}")" ;;
        backup) do_backup "$a" ;;
        rename) do_rename "$a" "$b" ;;
        setini) do_setini "$a" "$b" "$c" ;;
        remove)
            rm -f -- "${TARGET_DIR:?}/${a}"
            forget_file "$a"
            ;;
        restore)
            IFS=$'\t' read -r sha rel copy <<< "$a"
            mv -T -- "${TARGET_DIR}/${copy}" "${TARGET_DIR}/${rel}"
            drop_line BACKUPS "$a"
            ;;
        unrename)
            IFS=$'\t' read -r sha from to <<< "$a"
            mv -T -- "${TARGET_DIR}/${to}" "${TARGET_DIR}/${from}"
            drop_line RENAMES "$a"
            ;;
        forget-file) forget_file "$a" ;;
        forget-backup) drop_line BACKUPS "$a" ;;
        forget-rename) drop_line RENAMES "$a" ;;
        *) die 1 "internal: unknown step ${kind}" ;;
    esac
}

print_plan() {
    local i
    for i in "${!PLAN_KIND[@]}"; do echo "[plan] ${PLAN_TEXT[i]}"; done
    if (( ${#PLAN_KIND[@]} == 0 )); then echo "[plan] nothing to do"; fi
}

run_plan() {   # run_plan RECEIPT_STATUS_WHILE_RUNNING
    local i step=0 tag
    for i in "${!PLAN_KIND[@]}"; do
        case ${PLAN_KIND[i]} in
            keep | forget-*) tag='[=]' ;;
            *) tag='[+]' ;;
        esac
        echo "${tag} ${PLAN_TEXT[i]}"
        do_step "${PLAN_KIND[i]}" "${PLAN_A[i]}" "${PLAN_B[i]}" "${PLAN_C[i]}"
        write_receipt "$1"
        step=$((step + 1))
        if [[ ${RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP:-} == "$step" ]]; then
            echo "[-] RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP=${step}: stopping here, as a failed step would" >&2
            exit 1
        fi
    done
}

stop_if_refused() {
    local r
    if (( ${#REFUSALS[@]} )); then
        for r in "${REFUSALS[@]}"; do echo "[-] Refusing: ${r}" >&2; done
        echo "[-] Nothing was written." >&2
        exit "$REFUSE_CODE"
    fi
}

# ------------------------------------------------------------------------------
# Uninstall
# ------------------------------------------------------------------------------

declare -A GONE=() BACK=()
exists_after() {   # whether REL will exist once the steps planned so far have run
    if [[ -n ${BACK[$1]:-} ]]; then return 0; fi
    if [[ -n ${GONE[$1]:-} ]]; then return 1; fi
    exists "$1"
}

uninstall() {
    local rel path line sha copy from to i
    RUN_MODE=uninstall
    echo "[+] Uninstalling from: ${TARGET_DIR}"
    if [[ ! -f $RECEIPT ]]; then
        die 1 "no receipt at ${RECEIPT}. Only what a receipt records is ever removed, so nothing was" \
            "touched; a deploy made before receipts existed has to be undone by hand."
    fi
    check_not_running
    stop_if_refused
    load_receipt

    # Files first: ours come out only while they are still what this script wrote.
    for rel in "${FILE_ORDER[@]}"; do
        path="${TARGET_DIR}/${rel}"
        if ! exists "$rel"; then
            plan forget-file "$rel" '' '' "${rel}: already gone"
        elif [[ -f $path && ! -L $path && $(sha_of "$path") == "${FILE_SHA[$rel]}" ]]; then
            plan remove "$rel" '' '' "remove ${rel}"
            GONE[$rel]=1
        else
            echo "[!] ${rel}: left in place, it changed since this script wrote it"
        fi
    done
    # Then what was set aside, newest first, so the state just before the last deploy comes back.
    for (( i = ${#BACKUPS[@]} - 1; i >= 0; i-- )); do
        line=${BACKUPS[i]}
        IFS=$'\t' read -r sha rel copy <<< "$line"
        if ! exists "$copy"; then
            plan forget-backup "$line" '' '' "${rel}: its copy ${copy} is gone, nothing to restore"
        elif exists_after "$rel"; then
            echo "[!] ${rel}: not restored, a file is still there; the earlier one stays at ${copy}"
        else
            plan restore "$line" '' '' "restore ${rel} from ${copy}"
            BACK[$rel]=1
        fi
    done
    # Then renames, newest first.
    for (( i = ${#RENAMES[@]} - 1; i >= 0; i-- )); do
        line=${RENAMES[i]}
        IFS=$'\t' read -r sha from to <<< "$line"
        if exists_after "$from"; then
            if exists "$to"; then
                echo "[!] ${to}: not renamed back, ${from} is in the way"
            else
                plan forget-rename "$line" '' '' "${from}: already back in place"
            fi
        elif [[ -f ${TARGET_DIR}/${to} && $(sha_of "${TARGET_DIR}/${to}") == "$sha" ]]; then
            plan unrename "$line" '' '' "rename ${to} back to ${from}"
            BACK[$from]=1
            GONE[$to]=1
        elif ! exists "$to"; then
            plan forget-rename "$line" '' '' "${to}: gone, nothing to rename back to ${from}"
        else
            echo "[!] ${to}: not renamed back to ${from}, it changed since"
        fi
    done

    if (( DRY_RUN )); then
        print_plan
        echo "[=] Dry run: nothing was written."
        exit 0
    fi

    RECEIPT_OPEN=1
    run_plan uninstall-incomplete
    RECEIPT_OPEN=0

    rmdir -- "${STATE_DIR}"/backup/* 2>/dev/null || true
    if (( ${#FILE_ORDER[@]} + ${#BACKUPS[@]} + ${#RENAMES[@]} == 0 )); then
        rm -f -- "$RECEIPT"
        rmdir -- "${STATE_DIR}/backup" 2>/dev/null || true
        rmdir -- "${STATE_DIR}" 2>/dev/null || true
        echo ""
        echo "[✓] Uninstalled: nothing this script recorded is left in ${TARGET_DIR}"
    else
        write_receipt uninstall-incomplete
        echo ""
        echo "[!] Uninstalled what could be; the notes above say what was left and why."
        echo "    ${RECEIPT} still lists it."
    fi
}

# ------------------------------------------------------------------------------
# Deploy
# ------------------------------------------------------------------------------

deploy() {
    local custom_src=0 sha_opti='' sha_fwd='' f

    if [[ -n $SRC_DIR ]]; then
        custom_src=1
        SRC_DIR="${SRC_DIR/#\~/$HOME}"
    else
        SRC_DIR="${REPO_ROOT}/x64/out"
    fi
    BIN_SRC_OPTI="${SRC_DIR}/OptiScaler.dll"
    BIN_SRC_DLSSNR="${SRC_DIR}/${FORWARDER_NAME}"

    if [[ ! -f "${BIN_SRC_OPTI}" ]]; then
        if (( custom_src )); then
            die 1 "${BIN_SRC_OPTI} not found"
        elif (( DRY_RUN )); then
            echo "[plan] build first: tools/build-local.sh Release (there is no ${BIN_SRC_OPTI} yet)"
        else
            echo "[-] Error: ${BIN_SRC_OPTI} not found! Building first..."
            "${REPO_ROOT}/tools/build-local.sh" Release
        fi
    fi
    if [[ -f "${BIN_SRC_OPTI}" ]]; then
        sha_opti=$(sha_of "$BIN_SRC_OPTI")
        if ! has_marker "$BIN_SRC_OPTI" "$MARK_OPTISCALER"; then
            echo "[!] ${BIN_SRC_OPTI} does not contain '${MARK_OPTISCALER}'; other deploys will not" \
                "recognise it as OptiScaler, and the second-proxy check depends on that string"
        fi
    fi
    if [[ -f "${BIN_SRC_DLSSNR}" ]]; then
        sha_fwd=$(sha_of "$BIN_SRC_DLSSNR")
    elif (( ! DRY_RUN )) || [[ -f "${BIN_SRC_OPTI}" ]]; then
        echo "[-] Error: ${BIN_SRC_DLSSNR} not found!" >&2
        exit 1
    fi

    echo "[+] Deploying OptiScaler & DLSS-NR to: ${TARGET_DIR}"
    echo "[+] Using proxy filename: ${PROXY_NAME}"

    check_not_running
    load_receipt
    if [[ $OLD_STATUS == partial ]]; then
        echo "[!] The last deploy here stopped part-way; its records are kept and this one carries on from them"
    fi
    choose_backup_dir

    # 1. OptiScaler as the proxy DLL, and as OptiScaler.dll beside it when the proxy is a renamed copy
    plan_dll "$PROXY_NAME" "$BIN_SRC_OPTI" "$MARK_OPTISCALER" "$sha_opti" 1
    if [[ ${PROXY_NAME,,} != optiscaler.dll ]]; then
        plan_dll OptiScaler.dll "$BIN_SRC_OPTI" "$MARK_OPTISCALER" "$sha_opti" 0
    fi
    # 2. The DLSS-NR forwarder
    plan_dll "$FORWARDER_NAME" "$BIN_SRC_DLSSNR" "$MARK_FORWARDER" "$sha_fwd" 0
    # 3. No second OptiScaler under another proxy name
    check_other_proxies
    # 4. OptiScaler.ini: installed if absent, otherwise preserved; edited only for --keep-reshade
    plan_ini
    # 5. libxell
    plan_libxell

    stop_if_refused

    if (( DRY_RUN )); then
        print_plan
    else
        mkdir -p -- "$STATE_DIR"
        add_proxy "$PROXY_NAME"
        RECEIPT_OPEN=1
        write_receipt partial
        run_plan partial
        write_receipt complete
        RECEIPT_OPEN=0
    fi

    # 6. The Vulkan hook marker, if this deployment relies on it.
    #
    # Under vkd3d-proton the Vulkan overlay competed with Streamline DLSS-G's pacer and lost the
    # device. The marker beside the DLL disables those hooks; [Vulkan] SkipHooks=true in the ini does
    # the same and is the supported way to ask. Report which one is in force so a deployment never
    # silently changes it.
    if [[ -f "${TARGET_DIR}/optiscaler_skip_vulkan_hooks" ]]; then
        echo "[=] Vulkan hooks: skipped via marker file"
    elif [[ -f "${TARGET_DIR}/optiscaler_skip_vulkan_hooks.disabled" ]]; then
        echo "[=] Vulkan hooks: ACTIVE (marker present but disabled)"
    else
        echo "[=] Vulkan hooks: ACTIVE (no marker); set [Vulkan] SkipHooks=true if the device is lost"
    fi

    if (( DRY_RUN )); then
        echo "[=] Dry run: nothing was written."
        return 0
    fi

    echo ""
    echo "[✓] Deployment completed successfully!"
    echo "    Files installed:"
    for f in "$PROXY_NAME" "$FORWARDER_NAME" "$INI_NAME"; do
        echo "      - ${TARGET_DIR}/${f}"
    done
    if (( RESHADE_ROUTE )); then
        echo "    ReShade is kept beside it and loaded by OptiScaler ([Plugins] LoadReshade=true)."
    fi
    echo "    Receipt: ${RECEIPT}"
    echo "      (undo with: $0 --uninstall \"${TARGET_DIR}\")"
    echo ""
    echo "    Proton / Steam Launch Option reminder (if needed):"
    echo "      WINEDLLOVERRIDES=\"${PROXY_NAME%.*}=n,b\" %command%"
}

if (( UNINSTALL )); then
    uninstall
else
    deploy
fi
