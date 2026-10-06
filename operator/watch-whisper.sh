#!/usr/bin/env bash
# Poll for WAV files; append one timestamped transcription per completed file.
set -euo pipefail

# Same literal KEY=VALUE format as operator; never source/evaluate this file.
ENV_FILE=${OPERATOR_ENV_FILE:-"$(dirname -- "${BASH_SOURCE[0]}")/operator.env"}
if [[ -f "$ENV_FILE" ]]; then
    number=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        number=$((number + 1))
        line=${line%$'\r'}
        [[ -z "$line" || "$line" == \#* ]] && continue
        name=${line%%=*}
        if [[ "$line" != *=* || ! "$name" =~ ^(OPERATOR_[A-Z0-9_]*|WHISPER_BIN|WHISPER_MODEL)$ || "$name" == OPERATOR_ENV_FILE ]]; then
            echo "Invalid environment entry at line $number (expected literal KEY=VALUE)." >&2
            exit 2
        fi
        if [[ ! -v "$name" ]]; then export "$name=${line#*=}"; fi
    done < "$ENV_FILE"
elif [[ -n "${OPERATOR_ENV_FILE:-}" ]]; then
    echo "Cannot open environment file." >&2
    exit 2
fi

WATCH_DIR=${1:-${OPERATOR_OUTPUT_DIR:?Set OPERATOR_OUTPUT_DIR or pass a watch directory}}
LOG_FILE=${2:-"$WATCH_DIR/operator/transcriptions.log"}
ARCHIVE_DIR=${3:-"$WATCH_DIR/archive"}
WHISPER_BIN=${WHISPER_BIN:-whisper-cli}
WHISPER_MODEL=${WHISPER_MODEL:?Set WHISPER_MODEL in operator.env or the environment}
STATE_FILE=${STATE_FILE:-"${LOG_FILE}.done"}
ERROR_LOG=${ERROR_LOG:-"${LOG_FILE}.errors"}

[[ -d "$WATCH_DIR" ]] || { echo "Missing directory: $WATCH_DIR" >&2; exit 1; }
command -v -- "$WHISPER_BIN" >/dev/null 2>&1 || { echo "Missing executable: $WHISPER_BIN" >&2; exit 1; }
[[ -f "$WHISPER_MODEL" ]] || { echo "Missing model: $WHISPER_MODEL" >&2; exit 1; }
WATCH_DIR=$(realpath -- "$WATCH_DIR")
mkdir -p -- "$ARCHIVE_DIR"
ARCHIVE_DIR=$(realpath -- "$ARCHIVE_DIR")
[[ "$ARCHIVE_DIR" != "$WATCH_DIR" ]] || { echo "Archive must differ from watched directory." >&2; exit 1; }
mkdir -p -- "$(dirname -- "$LOG_FILE")" "$(dirname -- "$STATE_FILE")" "$(dirname -- "$ERROR_LOG")"
# Prevent two watchers using the same state from transcribing simultaneously.
exec 9>"${STATE_FILE}.lock"
flock -n 9 || { echo "A watcher is already running." >&2; exit 1; }
touch -- "$LOG_FILE" "$STATE_FILE" "$ERROR_LOG"

declare -A done=() previous=()
while IFS= read -r id; do
    [[ -z "$id" ]] || done["$id"]=1
done < "$STATE_FILE"

fingerprint() {
    stat -c '%s:%y:%i' -- "$1" 2>/dev/null
}

archive_wav() {
    local wav=$1 destination
    destination="$ARCHIVE_DIR/$(basename -- "$wav")"
    # Avoid overwriting an earlier recording with the same filename.
    if [[ -e "$destination" ]]; then
        destination=$(mktemp -- "$ARCHIVE_DIR/$(basename -- "${wav%.wav}").XXXXXX.wav")
    fi
    mv -- "$wav" "$destination"
}

echo "Watching $WATCH_DIR; logging to $LOG_FILE; archiving to $ARCHIVE_DIR" >&2
while true; do
    while IFS= read -r -d '' wav; do
        sig=$(fingerprint "$wav") || continue
        id=$(printf '%s\0%s' "$wav" "$sig" | sha256sum)
        id=${id%% *}
        if [[ ${done[$id]:-} ]]; then
            # Retry an archive move that failed after a successful transcription.
            archive_wav "$wav" || echo "Archive failed: $wav; will retry." >&2
            continue
        fi

        # Require unchanged size/mtime across polls (at least two seconds).
        if [[ ${previous[$wav]:-} != "$sig" ]]; then
            previous["$wav"]=$sig
            continue
        fi

        printf '[%s] %s\n' "$(date --iso-8601=seconds)" "$wav" >> "$ERROR_LOG"
        if text=$("$WHISPER_BIN" -m "$WHISPER_MODEL" -f "$wav" -nt -np 2>>"$ERROR_LOG"); then
            # Retry if the source changed during transcription.
            after=$(fingerprint "$wav") || continue
            [[ "$after" == "$sig" ]] || continue
            text=$(printf '%s\n' "$text" | tr '\r\n\t' '   ' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
            printf '[%s] %s\n' "$(date --iso-8601=seconds)" "$text" >> "$LOG_FILE"
            printf '%s\n' "$id" >> "$STATE_FILE"
            done["$id"]=1
            archive_wav "$wav" || echo "Archive failed: $wav; will retry." >&2
        else
            echo "Transcription failed: $wav (see $ERROR_LOG); will retry." >&2
        fi
    done < <(find "$WATCH_DIR" -maxdepth 1 -type f -iname '*.wav' -print0 | sort -z)
    sleep 2
done
