#!/bin/bash
# Slag Examples Browser
# Interactive terminal interface to view and run Slag examples

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VIEW_DIR="$SCRIPT_DIR/view"
COMPILER_DIR="$(dirname "$SCRIPT_DIR")/compiler"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

clear_screen() {
    clear
}

draw_line() {
    local cols="$(tput cols)"
    printf "${BLUE}"
    printf '─%.0s' $(seq 1 "$cols")
    printf "${NC}\n"
}

show_header() {
    echo -e "${CYAN}╔════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║${NC}              ${YELLOW}Slag Language Examples Browser${NC}                ${CYAN}║${NC}"
    echo -e "${CYAN}╚════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

list_examples() {
    echo -e "${GREEN}Available Examples:${NC}"
    draw_line

    local i=1 name desc line
    for file in $(ls -1 "$SCRIPT_DIR"/*.slag 2>/dev/null | sort); do
        if [ -f "$file" ]; then
            name="${file##*/}"
            name="${name%.slag}"
            # Read line 8 for description (after 6 blank comment lines)
            desc=""
            line=$(sed -n '8p' "$file")
            [[ "$line" == "// Example:"* ]] && desc="${line#// Example: }"
            [ -z "$desc" ] && desc="$name"
            printf "${YELLOW}%2d${NC}) %-25s    %s\n" "$i" "${name:3}" "- $desc"
            i=$((i + 1))
        fi
    done

    draw_line
    echo ""
    echo -e "${CYAN} v${NC}) View source code of an example"
    echo -e "${CYAN} r${NC}) Run an example"
    echo -e "${CYAN} q${NC}) Quit"
    echo ""
}

get_example_file() {
    local num=$1
    local i=1
    for file in $(ls -1 "$SCRIPT_DIR"/*.slag 2>/dev/null | sort); do
        if [ -f "$file" ]; then
            if [ "$i" -eq "$num" ]; then
                echo "$file"
                return
            fi
            i=$((i + 1))
        fi
    done
}

get_view_file() {
    local num=$1
    local i=1
    for file in $(ls -1 "$VIEW_DIR"/*.slag 2>/dev/null | sort); do
        if [ -f "$file" ]; then
            if [ "$i" -eq "$num" ]; then
                echo "$file"
                return
            fi
            i=$((i + 1))
        fi
    done
}

count_examples() {
    local count=0
    for file in $(ls -1 "$SCRIPT_DIR"/*.slag 2>/dev/null | sort); do
        if [ -f "$file" ]; then
            count=$((count + 1))
        fi
    done
    echo $count
}

view_source() {
    local file=$1
    clear_screen
    show_header
    echo -e "${GREEN}Source: ${YELLOW}$(basename "$file")${NC}"
    draw_line

    # Show source with line numbers
    cat -n "$file"

    echo ""
    draw_line
    echo -e "Press ${YELLOW}Enter${NC} to return to menu..."
    read
}

run_example() {
    local file=$1
    local basename=$(basename "$file" .slag)
    local dir=$(dirname "$file")

    clear_screen
    show_header
    echo -e "${GREEN}Compiling: ${YELLOW}$basename.slag${NC}"
    echo ""

    # Change to examples directory (for config files)
    cd "$dir"

    # Compile
    echo -e "${CYAN}[1/3]${NC} Running slag compiler..."
    if ! "$COMPILER_DIR/slag" "$basename.slag" 2>&1; then
        echo -e "${RED}Compilation failed!${NC}"
        echo "Press Enter to continue..."
        read
        return 1
    fi

    echo -e "${CYAN}[2/3]${NC} Assembling with NASM..."
    if ! nasm -f win64 "$basename.asm" -o "$basename.obj" 2>&1; then
        echo -e "${RED}Assembly failed!${NC}"
        echo "Press Enter to continue..."
        read
        return 1
    fi

    echo -e "${CYAN}[3/3]${NC} Linking..."
    if ! x86_64-w64-mingw32-gcc "$basename.obj" -o "$basename.exe" -nostdlib -e _start -lkernel32 -luser32 -lgdi32 -lws2_32 -lwinmm -mwindows 2>&1; then
        echo -e "${RED}Linking failed!${NC}"
        echo "Press Enter to continue..."
        read
        return 1
    fi
    chmod +x "$basename.exe"

    echo ""
    echo -e "${GREEN}Running: ${YELLOW}$basename.exe${NC}"
    echo -e "${BLUE}(Close the window to return to menu)${NC}"
    echo ""

    # Run the example
    "./$basename.exe"

    # Cleanup build artifacts
    rm -f "$basename.asm" "$basename.obj" "$basename.exe"

    echo ""
    echo -e "${GREEN}Example finished.${NC} Press Enter to continue..."
    read
}

# Main loop
while true; do
    clear_screen
    show_header
    list_examples

    echo -ne "${CYAN}Enter choice: ${NC}"
    read choice

    case $choice in
        q|Q)
            clear_screen
            echo -e "${GREEN}Thanks for exploring Slag examples!${NC}"
            exit 0
            ;;
        v|V)
            echo -ne "${CYAN}Enter example number to view: ${NC}"
            read num
            if [[ "$num" =~ ^[0-9]+$ ]]; then
                file=$(get_view_file "$num")
                if [ -n "$file" ]; then
                    view_source "$file"
                else
                    echo -e "${RED}Invalid example number${NC}"
                    sleep 1
                fi
            fi
            ;;
        r|R)
            echo -ne "${CYAN}Enter example number to run: ${NC}"
            read num
            if [[ "$num" =~ ^[0-9]+$ ]]; then
                file=$(get_example_file "$num")
                if [ -n "$file" ]; then
                    run_example "$file"
                else
                    echo -e "${RED}Invalid example number${NC}"
                    sleep 1
                fi
            fi
            ;;
        *)
            if [[ "$choice" =~ ^[0-9]+$ ]]; then
                # Direct number entry - show options
                file=$(get_example_file "$choice")
                if [ -n "$file" ]; then
                    echo -ne "${CYAN}[v]iew or [r]un? ${NC}"
                    read action
                    case $action in
                        v|V) view_file=$(get_view_file "$choice"); view_source "$view_file" ;;
                        r|R) run_example "$file" ;;
                    esac
                else
                    echo -e "${RED}Invalid example number${NC}"
                    sleep 1
                fi
            fi
            ;;
    esac
done
