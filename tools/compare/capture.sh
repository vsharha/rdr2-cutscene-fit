#!/usr/bin/env bash
set -euo pipefail

# Change this command if the installed gamescopectl uses different argument syntax.
GAMESCOPE_SCREENSHOT=(gamescopectl screenshot)

if [[ $# -ne 2 ]]; then
    printf 'Usage: %s GAME_FOLDER OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi

game_folder=$1
output_dir=$2
handshake="$game_folder/CutsceneFit.compare"
done_file="$game_folder/CutsceneFit.compare.done"

if [[ -z ${GAMESCOPE_WAYLAND_DISPLAY:-} ]]; then
    runtime_dir=${XDG_RUNTIME_DIR:-}
    if [[ -z $runtime_dir ]]; then
        printf 'XDG_RUNTIME_DIR is unset; set GAMESCOPE_WAYLAND_DISPLAY explicitly.\n' >&2
        exit 1
    fi
    # gamescope also creates gamescope-N-ei, an input socket that gamescopectl hangs on.
    socket=$(find "$runtime_dir" -maxdepth 1 -type s -regextype posix-extended -regex '.*/gamescope-[0-9]+' -print -quit)
    if [[ -z $socket ]]; then
        printf 'No gamescope-* socket found in %s; set GAMESCOPE_WAYLAND_DISPLAY explicitly.\n' "$runtime_dir" >&2
        exit 1
    fi
    export GAMESCOPE_WAYLAND_DISPLAY=${socket##*/}
fi

mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd -P)
last_sequence=0
temp_done="$done_file.tmp"
cleanup() {
    rm -f "$temp_done"
    printf '\nCapture script stopped.\n'
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'Watching %s; screenshots go to %s.\n' "$handshake" "$output_dir"
while true; do
    if [[ -r $handshake ]]; then
        read -r sequence state < "$handshake" || true
        if [[ ${sequence:-} == 1 && ${state:-} == game && $last_sequence -gt 0 ]]; then
            last_sequence=0
        fi
        if [[ ${sequence:-} =~ ^[0-9]+$ && ${state:-} =~ ^[a-z_]+$ && sequence -gt last_sequence ]]; then
            image="$output_dir/$sequence-$state.png"
            printf 'Capturing %s...\n' "$image"
            rm -f "$image"
            if timeout 10 "${GAMESCOPE_SCREENSHOT[@]}" "$image"; then
                :
            else
                status=$?
                if [[ $status -eq 124 ]]; then
                    printf 'Timed out after 10 seconds running gamescopectl screenshot for %s.\n' "$image" >&2
                else
                    printf 'gamescopectl screenshot failed with status %s for %s.\n' "$status" "$image" >&2
                fi
                exit 1
            fi
            printf '%s\n' "$sequence" > "$temp_done"
            mv -f "$temp_done" "$done_file"
            last_sequence=$sequence
            wait_count=0
            while [[ ! -f $image && $wait_count -lt 100 ]]; do
                sleep 0.1
                wait_count=$((wait_count + 1))
            done
            if [[ ! -f $image ]]; then
                printf 'Timed out after 10 seconds waiting for screenshot: %s\n' "$image" >&2
                exit 1
            fi
            previous_size=-1
            stable=0
            while (( stable < 3 )); do
                size=$(stat -c '%s' "$image")
                if [[ $size == "$previous_size" ]]; then
                    ((stable += 1))
                else
                    stable=0
                fi
                previous_size=$size
                sleep 0.2
            done
            printf 'Acknowledged sequence %s (%s).\n' "$sequence" "$state"
        fi
    fi
    sleep 0.01
done
