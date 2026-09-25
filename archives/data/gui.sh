#!/usr/bin/env bash

set -u
set -o pipefail

# ==========================================================
# DICTAPHONE USB
# INTERFACE GRAPHIQUE — V3.2
# ==========================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
ENGINE="$SCRIPT_DIR/script.sh"

GUI_TMP=""

cleanup() {
    if [[ -n "$GUI_TMP" && -d "$GUI_TMP" ]]; then
        rm -rf "$GUI_TMP"
    fi
}

trap cleanup EXIT

# ==========================================================
# ZENITY
# ==========================================================

if ! command -v zenity >/dev/null 2>&1; then
    echo "Zenity n'est pas installé."
    echo "Installez-le avec : sudo apt install zenity"
    exit 1
fi

# ==========================================================
# MOTEUR
# ==========================================================

if [[ ! -f "$ENGINE" ]]; then
    zenity \
        --error \
        --title="Dictaphone" \
        --width=550 \
        --text="Le moteur de transcription est introuvable :\n\n$ENGINE"

    exit 1
fi

# ==========================================================
# DOSSIER TEMPORAIRE
# ==========================================================

GUI_TMP="$(mktemp -d "${TMPDIR:-/tmp}/dictaphone-gui.XXXXXX")" || {
    zenity \
        --error \
        --title="Dictaphone" \
        --text="Impossible de créer le dossier temporaire."

    exit 1
}

LOG_FILE="$GUI_TMP/transcription.log"
RESULT_FILE="$GUI_TMP/result"
STATUS_FILE="$GUI_TMP/status"

touch "$LOG_FILE"

echo "Démarrage..." > "$STATUS_FILE"

# ==========================================================
# RECHERCHE DES WAV
# ==========================================================

WAV_FILES=()

for file in "$ROOT_DIR"/REC*.WAV; do
    [[ -f "$file" ]] || continue
    WAV_FILES+=("$file")
done

for file in "$ROOT_DIR"/REC*.wav; do
    [[ -f "$file" ]] || continue
    WAV_FILES+=("$file")
done

if (( ${#WAV_FILES[@]} > 0 )); then
    mapfile -t WAV_FILES < <(
        printf '%s\n' "${WAV_FILES[@]}" |
        LC_ALL=C sort -u
    )
fi

COUNT="${#WAV_FILES[@]}"

# ==========================================================
# AUCUN ENREGISTREMENT
# ==========================================================

if (( COUNT == 0 )); then

    zenity \
        --info \
        --title="Dictaphone" \
        --width=450 \
        --text="<b>Aucun enregistrement</b>\n\nIl n'y a actuellement aucun nouvel enregistrement à transcrire."

    exit 0
fi

# ==========================================================
# LISTE DES FICHIERS
# ==========================================================

LIST=""

for file in "${WAV_FILES[@]}"; do
    LIST="${LIST}• $(basename "$file")"$'\n'
done

# ==========================================================
# CONFIRMATION
# ==========================================================

CONFIRM_TEXT="<b>$COUNT enregistrement(s) trouvé(s)</b>"$'\n\n'"$LIST"$'\n'"Voulez-vous lancer la transcription ?"

if ! zenity \
    --question \
    --title="Dictaphone" \
    --width=550 \
    --text="$CONFIRM_TEXT" \
    --ok-label="Transcrire" \
    --cancel-label="Annuler"
then
    exit 0
fi

# ==========================================================
# EXECUTION DU MOTEUR
# ==========================================================

echo "Préparation..." > "$STATUS_FILE"

(
    bash "$ENGINE" > "$LOG_FILE" 2>&1
    RESULT=$?
    echo "$RESULT" > "$RESULT_FILE"
) &

ENGINE_PID=$!

# ==========================================================
# PROGRESSION
# ==========================================================

(
    echo "0"
    echo "# Préparation..."

    sleep 1

    if kill -0 "$ENGINE_PID" 2>/dev/null; then
        echo "15"
        echo "# Vérification des enregistrements..."
    fi

    sleep 1

    if kill -0 "$ENGINE_PID" 2>/dev/null; then
        echo "25"
        echo "# Préparation de Whisper..."
    fi

    sleep 1

    if kill -0 "$ENGINE_PID" 2>/dev/null; then
        echo "30"
        echo "# Transcription en cours..."
    fi

    while kill -0 "$ENGINE_PID" 2>/dev/null; do

        echo "30"
        echo "# Whisper travaille..."

        sleep 2
    done

    wait "$ENGINE_PID" 2>/dev/null || true

    RESULT=1

    if [[ -f "$RESULT_FILE" ]]; then
        RESULT="$(cat "$RESULT_FILE")"
    fi

    if [[ "$RESULT" == "0" ]]; then
        echo "100"
        echo "# Transcription terminée."
    else
        echo "100"
        echo "# Une erreur est survenue."
    fi

) | zenity \
    --progress \
    --title="Dictaphone — Transcription" \
    --width=650 \
    --percentage=0 \
    --auto-close \
    --no-cancel \
    --text="Préparation..."

# ==========================================================
# RESULTAT DU MOTEUR
# ==========================================================

ENGINE_RESULT=1

if [[ -f "$RESULT_FILE" ]]; then
    ENGINE_RESULT="$(cat "$RESULT_FILE")"
fi

# ==========================================================
# ERREUR
# ==========================================================

if [[ "$ENGINE_RESULT" != "0" ]]; then

    ERROR_LOG=""

    if [[ -s "$LOG_FILE" ]]; then
        ERROR_LOG="$(tail -20 "$LOG_FILE")"
    fi

    ERROR_TEXT="<b>La transcription n'a pas pu être terminée.</b>"$'\n\n'
    ERROR_TEXT+="Les enregistrements originaux ont été conservés."$'\n\n'
    ERROR_TEXT+="Vous pouvez réessayer."

    zenity \
        --error \
        --title="Dictaphone — Erreur" \
        --width=600 \
        --text="$ERROR_TEXT"

    exit 1
fi

# ==========================================================
# RECHERCHE DE LA TRANSCRIPTION
# ==========================================================

LATEST_TXT="$(
    find "$ROOT_DIR" \
        -maxdepth 1 \
        -type f \
        -name '*.txt' \
        -printf '%T@ %p\n' 2>/dev/null |
    LC_ALL=C sort -n |
    tail -1 |
    cut -d' ' -f2-
)"

# ==========================================================
# VALIDATION
# ==========================================================

if [[ -z "$LATEST_TXT" ]]; then

    ERROR_TEXT="<b>La transcription semble terminée, mais le fichier texte est introuvable.</b>"$'\n\n'
    ERROR_TEXT+="Les enregistrements originaux ont été conservés."

    zenity \
        --error \
        --title="Dictaphone — Erreur" \
        --width=600 \
        --text="$ERROR_TEXT"

    exit 1
fi

# ==========================================================
# RESULTAT FINAL
# ==========================================================

FINAL_TEXT="<b>✓ Transcription terminée</b>"$'\n\n'
FINAL_TEXT+="$COUNT enregistrement(s) ont été traité(s)."$'\n\n'
FINAL_TEXT+="<b>Transcription :</b>"$'\n'
FINAL_TEXT+="$(basename "$LATEST_TXT")"$'\n\n'
FINAL_TEXT+="<b>Archive audio :</b>"$'\n'
FINAL_TEXT+="archives/"$'\n\n'
FINAL_TEXT+="Les enregistrements originaux ont été traités avec succès."

zenity \
    --info \
    --title="Dictaphone — Terminé" \
    --width=600 \
    --text="$FINAL_TEXT" \
    --ok-label="Fermer"

exit 0

