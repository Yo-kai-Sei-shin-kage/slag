#!/bin/bash
# Slag Examples Browser
#   ./run_examples.sh          interactive menu
#   ./run_examples.sh --check  compile every example, report pass/fail
#   ./run_examples.sh --list   print the numbered list and exit

COLS="$(tput cols)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VIEW_DIR="$SCRIPT_DIR/view"
PARENT="$(dirname "$SCRIPT_DIR")"
COMPILER_DIR="$PARENT/comp""iler"
SLAG="$COMPILER_DIR/slag"
SLAGRUN="$COMPILER_DIR/slagrun"
RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
BLUE=$'\033[0;34m'; CYAN=$'\033[0;36m'; NC=$'\033[0m'


draw_line() { printf "%s%s%s\n" "$BLUE" "-----------------------------------------------------------------------------------" "$NC"; }

show_header() {
    printf "%s==============================================%s\n" "$CYAN" "$NC"
    printf "%s|%s      %sSlag Language Examples Browser%s       %s|%s\n" "$CYAN" "$NC" "$YELLOW" "$NC" "$CYAN" "$NC"
    printf "%s==============================================%s\n\n" "$CYAN" "$NC"
}

FILES=()
for g in "$SCRIPT_DIR"/*.slag; do [ -e "$g" ] && FILES+=("$g"); done

list_examples() {
    printf "%sAvailable Examples:%s\n" "$GREEN" "$NC"; draw_line
    local i name desc line j
    for i in "${!FILES[@]}"; do
        name="${FILES[$i]##*/}"; name="${name%.slag}"
        desc="$name"; j=0
        while IFS= read -r line; do
            j=$((j+1))
            if [ "$j" -eq 8 ]; then
                case "$line" in "// Example:"*) desc="${line#// Example: }";; esac
                break
            fi
        done < "${FILES[$i]}"
        printf "%s%2d%s) %-25s - %s\n" "$YELLOW" "$((i+1))" "$NC" "${name:3}" "$desc"
    done
    draw_line; echo
    printf " %sv%s) View   %sr%s) Run   %sq%s) Quit\n\n" "$CYAN" "$NC" "$CYAN" "$NC" "$CYAN" "$NC"
}

view_source() {
    local file=$1
    clear; show_header
    printf "%sSource: %s%s%s\n" "$GREEN" "$YELLOW" "${file##*/}" "$NC"; draw_line
    cat -n "$file"; echo; draw_line
    printf "Press %sEnter%s to return..." "$YELLOW" "$NC"; read -r
}

run_example() {
    local file=$1 base="${1##*/}"; base="${base%.slag}"
    clear; show_header
    printf "%sBuilding & running: %s%s.slag%s\n\n" "$GREEN" "$YELLOW" "$base" "$NC"
    ( cd "$SCRIPT_DIR" && "$SLAGRUN" -g "$base.slag" )
    printf "\n%sDone.%s Press %sEnter%s..." "$GREEN" "$NC" "$YELLOW" "$NC"; read -r
}

check_all() {
    local fail=0 total=0 base RM=rm
    cd "$SCRIPT_DIR" || exit 1
    for f in "${FILES[@]}"; do
        base="${f##*/}"; base="${base%.slag}"; total=$((total+1))
        if "$SLAG" "$base.slag" >/dev/null 2>&1; then
            printf "%sOK  %s %s.slag\n" "$GREEN" "$NC" "$base"
        else
            printf "%sFAIL%s %s.slag\n" "$RED" "$NC" "$base"; fail=$((fail+1))
        fi
        [ -f "$base.asm" ] && "$RM" -f -- "$base.asm"
    done
    echo
    if [ "$fail" -eq 0 ]; then printf "%sAll %d examples compiled cleanly.%s\n" "$GREEN" "$total" "$NC"
    else printf "%s%d of %d failed.%s\n" "$RED" "$fail" "$total" "$NC"; fi
    return "$fail"
}

case "${1:-}" in
    --check|-c) check_all; exit $? ;;
    --list|-l)  list_examples; exit 0 ;;
esac

while true; do
    clear; show_header; list_examples
    printf "%sChoice: %s" "$CYAN" "$NC"; read -r choice
    case $choice in
        q|Q) clear; printf "%sBye.%s\n" "$GREEN" "$NC"; exit 0 ;;
        *)
            case $choice in *[!0-9]*|"") continue ;; esac
            idx=$((choice-1))
            [ -z "${FILES[$idx]+x}" ] && continue
            printf "%s[v]iew or [r]un? %s" "$CYAN" "$NC"; read -r act
            case $act in
                v|V) vf="$VIEW_DIR/${FILES[$idx]##*/}"; [ -f "$vf" ] || vf="${FILES[$idx]}"; view_source "$vf" ;;
                r|R) run_example "${FILES[$idx]}" ;;
            esac
            ;;
    esac
done
