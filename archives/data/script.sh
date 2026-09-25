#!/usr/bin/env bash

set -u
set -o pipefail


# ==========================================================
# DICTAPHONE USB
# VERSION 3.2
#
# Ajout V3.2 :
#   - génération d'un nom de fichier basé sur la transcription
#
# Exemple :
#
#   2026-08-27_15-42-08_reunion_client.wav
#   2026-08-27_15-42-08_reunion_client.txt
#
# L'identifiant date/heure reste toujours présent.
# ==========================================================


# ==========================================================
# CHEMINS
# ==========================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ARCHIVES_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ROOT_DIR="$(cd "$ARCHIVES_DIR/.." && pwd)"

WHISPER_SOURCE="$SCRIPT_DIR/whisper"


# ==========================================================
# MODELE WHISPER
# ==========================================================

MODEL_NAME="ggml-small.bin"

MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"

MODEL_SIZE="487601967"

MODEL_SHA256="1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b"


# ==========================================================
# CACHE LOCAL
# ==========================================================

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/dictaphone"

MODEL_CACHE_DIR="$CACHE_ROOT/whisper"

MODEL="$MODEL_CACHE_DIR/$MODEL_NAME"


# ==========================================================
# VARIABLES TEMPORAIRES
# ==========================================================

RUNTIME_DIR=""
WHISPER=""

LOCAL_WAV=""
LOCAL_TXT=""
LOCAL_BASE=""

SESSION=""
ARCHIVE_WAV=""
OUTPUT_TXT=""

WAV_FILES=()


# ==========================================================
# GESTION DES ERREURS
# ==========================================================

error() {

    echo ""
    echo "=========================================="
    echo "                 ERREUR"
    echo "=========================================="
    echo ""
    echo "$1"
    echo ""
    echo "Les enregistrements originaux n'ont PAS été supprimés."
    echo ""

    exit 1
}


# ==========================================================
# NETTOYAGE
# ==========================================================

cleanup() {

    if [[ -n "$RUNTIME_DIR" && -d "$RUNTIME_DIR" ]]; then
        rm -rf "$RUNTIME_DIR"
    fi
}

trap cleanup EXIT


# ==========================================================
# OUTILS NECESSAIRES
# ==========================================================

check_dependencies() {

    local missing=0

    for command in \
        awk \
        cp \
        date \
        dd \
        mkdir \
        mktemp \
        mv \
        rm \
        sha256sum \
        wc \
        sed \
        tr \
        fold
    do

        if ! command -v "$command" >/dev/null 2>&1; then

            echo "⚠ Commande manquante : $command"

            missing=1

        fi

    done


    if (( missing != 0 )); then

        error "Une ou plusieurs commandes nécessaires sont absentes."

    fi
}


# ==========================================================
# TELECHARGEMENT DU MODELE
# ==========================================================

download_model() {

    local part
    local local_size
    local local_sha
    local downloaded_size
    local downloaded_sha


    echo ""
    echo "=========================================="
    echo "          MODELE WHISPER"
    echo "=========================================="
    echo ""


    if ! mkdir -p "$MODEL_CACHE_DIR"; then

        error "Impossible de créer le cache local :

$MODEL_CACHE_DIR"

    fi


    if [[ -f "$MODEL" ]]; then

        echo "Modèle trouvé dans le cache local."
        echo ""
        echo "Vérification..."

        local_size="$(wc -c < "$MODEL")"


        if [[ "$local_size" == "$MODEL_SIZE" ]]; then

            local_sha="$(sha256sum "$MODEL" | awk '{print $1}')"


            if [[ "$local_sha" == "$MODEL_SHA256" ]]; then

                echo "✓ Taille correcte"
                echo "✓ SHA-256 correct"
                echo "✓ Modèle prêt"
                echo ""

                return 0

            fi

        fi


        echo "⚠ Le modèle présent dans le cache est invalide."
        echo "  Il sera téléchargé à nouveau."
        echo ""

        rm -f "$MODEL"

    fi


    part="$MODEL_CACHE_DIR/$MODEL_NAME.part"


    echo "Modèle absent du cache."
    echo ""
    echo "Téléchargement de $MODEL_NAME"
    echo ""
    echo "Taille attendue : environ 488 MB"
    echo ""


    if command -v curl >/dev/null 2>&1; then

        echo "Téléchargement avec curl..."
        echo ""


        if ! curl \
            -L \
            --fail \
            --show-error \
            --progress-bar \
            --retry 5 \
            --retry-delay 3 \
            --connect-timeout 20 \
            -C - \
            -o "$part" \
            "$MODEL_URL"
        then

            echo ""
            echo "⚠ Le téléchargement a échoué."
            echo ""
            echo "Le fichier partiel est conservé :"
            echo "$part"
            echo ""

            return 1

        fi


    elif command -v wget >/dev/null 2>&1; then

        echo "Téléchargement avec wget..."
        echo ""


        if ! wget \
            --continue \
            --show-progress \
            -O "$part" \
            "$MODEL_URL"
        then

            echo ""
            echo "⚠ Le téléchargement a échoué."
            echo ""

            return 1

        fi


    else

        error "Ni curl ni wget ne sont disponibles."

    fi


    echo ""
    echo "Vérification du téléchargement..."
    echo ""


    if [[ ! -f "$part" ]]; then

        error "Le fichier téléchargé est introuvable."

    fi


    downloaded_size="$(wc -c < "$part")"


    if [[ "$downloaded_size" != "$MODEL_SIZE" ]]; then

        echo "✗ Taille incorrecte"
        echo ""
        echo "Attendu : $MODEL_SIZE octets"
        echo "Obtenu  : $downloaded_size octets"
        echo ""

        return 1

    fi


    echo "✓ Taille correcte"


    echo ""
    echo "Calcul du SHA-256..."
    echo ""


    downloaded_sha="$(sha256sum "$part" | awk '{print $1}')"


    if [[ "$downloaded_sha" != "$MODEL_SHA256" ]]; then

        echo ""
        echo "✗ SHA-256 incorrect"
        echo ""
        echo "Attendu :"
        echo "$MODEL_SHA256"
        echo ""
        echo "Obtenu :"
        echo "$downloaded_sha"
        echo ""

        rm -f "$part"

        return 1

    fi


    echo "✓ SHA-256 correct"


    if ! mv -f "$part" "$MODEL"; then

        error "Impossible d'installer le modèle dans le cache."

    fi


    echo ""
    echo "✓ Modèle installé dans le cache local."
    echo ""

    return 0
}


# ==========================================================
# VERIFICATION D'UN WAV
# ==========================================================

check_wav_header() {

    local file="$1"

    local riff
    local wave


    riff="$(dd \
        if="$file" \
        bs=1 \
        count=4 \
        2>/dev/null)"


    wave="$(dd \
        if="$file" \
        bs=1 \
        skip=8 \
        count=4 \
        2>/dev/null)"


    if [[ "$riff" != "RIFF" ]]; then
        return 1
    fi


    if [[ "$wave" != "WAVE" ]]; then
        return 1
    fi


    return 0
}


# ==========================================================
# GENERATION DU NOM INTELLIGENT
# ==========================================================

generate_smart_name() {

    local text="$1"

    local words
    local word
    local result=""
    local count=0


    # ------------------------------------------------------
    # Nettoyage général
    # ------------------------------------------------------

    text="$(tr '\n' ' ' < "$text")"

    text="$(sed \
        -e 's/[[:space:]][[:space:]]*/ /g' \
        -e 's/^ //' \
        -e 's/ $//' \
        <<< "$text")"


    # ------------------------------------------------------
    # Conversion en minuscules
    # ------------------------------------------------------

    text="$(printf '%s' "$text" | tr '[:upper:]' '[:lower:]')"


    # ------------------------------------------------------
    # Suppression de la ponctuation
    # ------------------------------------------------------

    text="$(printf '%s' "$text" | \
        sed 's/[,.!?;:(){}\[\]"'"'"'«»…]/ /g')"


    # ------------------------------------------------------
    # Normalisation des accents
    #
    # On utilise iconv s'il est disponible.
    # ------------------------------------------------------

    if command -v iconv >/dev/null 2>&1; then

        text="$(printf '%s' "$text" | \
            iconv -f UTF-8 -t ASCII//TRANSLIT 2>/dev/null || true)"

    fi


    # ------------------------------------------------------
    # Suppression des caractères non sûrs
    # ------------------------------------------------------

    text="$(printf '%s' "$text" | \
        sed 's/[^a-z0-9_-]/ /g')"

    text="$(printf '%s' "$text" | \
        sed 's/[[:space:]][[:space:]]*/ /g')"


    # ------------------------------------------------------
    # Mots à ignorer
    # ------------------------------------------------------

    local stopwords="\
a au aux avec ce ceci cela ces cette
de des du
en est et
il ils
je
la le les
lui
ma mais me mes mon
ne nos notre nous
on ont ou
par pas pour
que quel quelle quels quelles qui
se son sont
sur
ta te tes ton tous tout toute
un une
vos votre vous
y"


    # ------------------------------------------------------
    # Extraction des mots significatifs
    # ------------------------------------------------------

    read -ra words <<< "$text"


    for word in "${words[@]}"; do

        [[ -n "$word" ]] || continue


        # Mot trop court
        if (( ${#word} < 3 )); then
            continue
        fi


        # Mot générique
        if grep -qx "$word" <<< "$stopwords"; then
            continue
        fi


        if [[ -z "$result" ]]; then

            result="$word"

        else

            result="${result}_${word}"

        fi


        count=$((count + 1))


        # 4 mots maximum
        if (( count >= 4 )); then
            break
        fi

    done


    # ------------------------------------------------------
    # Limitation de longueur
    # ------------------------------------------------------

    if [[ -n "$result" ]]; then

        result="${result:0:55}"

        # éviter un underscore final
        result="${result%_}"

    fi


    printf '%s' "$result"
}


# ==========================================================
# DEMARRAGE
# ==========================================================

echo ""
echo "=========================================="
echo "          DICTAPHONE USB"
echo "             VERSION 3.2"
echo "=========================================="
echo ""


# ==========================================================
# DEPENDANCES
# ==========================================================

check_dependencies


# ==========================================================
# VERIFICATION WHISPER
# ==========================================================

if [[ ! -f "$WHISPER_SOURCE" ]]; then

    error "Whisper est introuvable :

$WHISPER_SOURCE

Le binaire doit être placé dans :

$SCRIPT_DIR/whisper"

fi


echo "✓ Whisper trouvé"


# ==========================================================
# ARCHIVES
# ==========================================================

if [[ ! -d "$ARCHIVES_DIR" ]]; then

    if ! mkdir -p "$ARCHIVES_DIR"; then

        error "Impossible de créer :

$ARCHIVES_DIR"

    fi

fi


echo "✓ Archives prêtes"


# ==========================================================
# MODELE
# ==========================================================

if ! download_model; then

    error "Impossible d'obtenir un modèle Whisper valide."

fi


# ==========================================================
# RUNTIME LOCAL
# ==========================================================

echo "=========================================="
echo "        PREPARATION DU RUNTIME"
echo "=========================================="
echo ""


RUNTIME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dictaphone.XXXXXX")" || {

    error "Impossible de créer le dossier temporaire."

}


WHISPER="$RUNTIME_DIR/whisper-cli"

LOCAL_WAV="$RUNTIME_DIR/input.wav"

LOCAL_BASE="$RUNTIME_DIR/transcription"

LOCAL_TXT="$LOCAL_BASE.txt"


# ==========================================================
# COPIE WHISPER
# ==========================================================

echo "Copie de Whisper vers le disque local..."


if ! cp "$WHISPER_SOURCE" "$WHISPER"; then

    error "Impossible de copier Whisper localement."

fi


if ! chmod +x "$WHISPER"; then

    error "Impossible de rendre Whisper exécutable."

fi


echo "✓ Whisper prêt"


# ==========================================================
# RECHERCHE DES ENREGISTREMENTS
# ==========================================================

echo ""
echo "Recherche des enregistrements..."
echo ""


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


if (( COUNT == 0 )); then

    echo "Aucun nouvel enregistrement trouvé."
    echo ""

    exit 0

fi


echo "✓ $COUNT enregistrement(s) trouvé(s)"
echo ""


for file in "${WAV_FILES[@]}"; do

    echo "  • $(basename "$file")"

done


# ==========================================================
# VALIDATION WAV
# ==========================================================

echo ""
echo "Vérification des enregistrements..."
echo ""


for file in "${WAV_FILES[@]}"; do

    if ! check_wav_header "$file"; then

        error "Fichier WAV invalide :

$(basename "$file")"

    fi

done


echo "✓ Tous les WAV sont valides"


# ==========================================================
# SESSION
# ==========================================================

SESSION="$(date '+%Y-%m-%d_%H-%M-%S')"


# ==========================================================
# CONCATENATION LOCALE
# ==========================================================

echo ""
echo "=========================================="
echo "       PREPARATION DE L'AUDIO"
echo "=========================================="
echo ""

echo "Création de l'audio de travail sur le disque local."
echo ""


FIRST_WAV="${WAV_FILES[0]}"


if ! dd \
    if="$FIRST_WAV" \
    of="$LOCAL_WAV" \
    bs=44 \
    count=1 \
    status=none
then

    error "Impossible de créer le WAV local."

fi


for file in "${WAV_FILES[@]}"; do

    echo "  + $(basename "$file")"


    if ! dd \
        if="$file" \
        bs=44 \
        skip=1 \
        status=none \
        >> "$LOCAL_WAV"
    then

        error "Erreur pendant la lecture de :

$(basename "$file")"

    fi

done


# ==========================================================
# MISE A JOUR HEADER WAV
# ==========================================================

write_u32_le() {

    local value="$1"
    local offset="$2"
    local file="$3"

    local b0=$(( value & 255 ))
    local b1=$(( (value >> 8) & 255 ))
    local b2=$(( (value >> 16) & 255 ))
    local b3=$(( (value >> 24) & 255 ))


    printf '%b' \
        "$(printf '\\%03o\\%03o\\%03o\\%03o' \
            "$b0" "$b1" "$b2" "$b3")" |
        dd \
            of="$file" \
            bs=1 \
            seek="$offset" \
            conv=notrunc \
            status=none
}


FILE_SIZE="$(wc -c < "$LOCAL_WAV")"


if (( FILE_SIZE < 45 )); then

    error "Le WAV concaténé est trop petit."

fi


DATA_SIZE=$((FILE_SIZE - 44))

RIFF_SIZE=$((FILE_SIZE - 8))


if (( FILE_SIZE > 4294967295 )); then

    error "Le WAV dépasse la limite de 4 Go."

fi


if ! write_u32_le "$RIFF_SIZE" 4 "$LOCAL_WAV"; then

    error "Impossible de mettre à jour la taille RIFF."

fi


if ! write_u32_le "$DATA_SIZE" 40 "$LOCAL_WAV"; then

    error "Impossible de mettre à jour la taille DATA."

fi


LOCAL_WAV_SIZE="$(wc -c < "$LOCAL_WAV")"


if (( LOCAL_WAV_SIZE <= 44 )); then

    error "Le WAV local ne contient aucune donnée audio."

fi


echo ""
echo "✓ Audio local prêt"
echo "  Taille : $LOCAL_WAV_SIZE octets"


# ==========================================================
# TRANSCRIPTION
# ==========================================================

echo ""
echo "=========================================="
echo "              TRANSCRIPTION"
echo "=========================================="
echo ""

echo "Whisper travaille..."
echo ""
echo "Modèle : $MODEL_NAME"
echo "Audio  : disque local"
echo ""


if ! "$WHISPER" \
    -m "$MODEL" \
    -f "$LOCAL_WAV" \
    -l fr \
    --no-fallback \
    --no-timestamps \
    -otxt \
    -of "$LOCAL_BASE"
then

    error "Whisper a échoué pendant la transcription."

fi


# ==========================================================
# VALIDATION TRANSCRIPTION
# ==========================================================

if [[ ! -f "$LOCAL_TXT" ]]; then

    error "Whisper n'a pas créé le fichier texte."

fi


if [[ ! -s "$LOCAL_TXT" ]]; then

    error "Whisper a produit une transcription vide."

fi


echo ""
echo "✓ Transcription terminée"


# ==========================================================
# GENERATION DU NOM
# ==========================================================

echo ""
echo "=========================================="
echo "          NOM DE LA SESSION"
echo "=========================================="
echo ""


SMART_NAME="$(generate_smart_name "$LOCAL_TXT")"


if [[ -n "$SMART_NAME" ]]; then

    SESSION="${SESSION}_${SMART_NAME}"

    echo "Nom généré :"
    echo "$SESSION"

else

    echo "Aucun nom pertinent détecté."
    echo "Nom générique conservé :"
    echo "$SESSION"

fi


echo ""


# ==========================================================
# CHEMINS FINAUX
# ==========================================================

ARCHIVE_WAV="$ARCHIVES_DIR/$SESSION.wav"

OUTPUT_TXT="$ROOT_DIR/$SESSION.txt"


COUNTER=1


while [[ -e "$ARCHIVE_WAV" || -e "$OUTPUT_TXT" ]]; do

    ARCHIVE_WAV="$ARCHIVES_DIR/${SESSION}_$COUNTER.wav"

    OUTPUT_TXT="$ROOT_DIR/${SESSION}_$COUNTER.txt"

    COUNTER=$((COUNTER + 1))

done


# ==========================================================
# ARCHIVAGE WAV
# ==========================================================

echo ""
echo "=========================================="
echo "              ARCHIVAGE"
echo "=========================================="
echo ""


echo "Copie du WAV vers la clé..."


if ! cp "$LOCAL_WAV" "$ARCHIVE_WAV"; then

    error "Impossible de copier le WAV final."

fi


FINAL_WAV_SIZE="$(wc -c < "$ARCHIVE_WAV")"


if [[ "$FINAL_WAV_SIZE" != "$LOCAL_WAV_SIZE" ]]; then

    rm -f "$ARCHIVE_WAV"

    error "La copie du WAV final est incomplète."

fi


echo "✓ WAV archivé"


# ==========================================================
# ARCHIVAGE TRANSCRIPTION
# ==========================================================

echo ""
echo "Copie de la transcription..."


if ! cp "$LOCAL_TXT" "$OUTPUT_TXT"; then

    rm -f "$ARCHIVE_WAV"

    error "Impossible de copier la transcription."

fi


if [[ ! -s "$OUTPUT_TXT" ]]; then

    rm -f "$ARCHIVE_WAV"
    rm -f "$OUTPUT_TXT"

    error "La transcription finale est vide."

fi


echo "✓ Transcription enregistrée"


# ==========================================================
# SYNCHRONISATION
# ==========================================================

sync


# ==========================================================
# SUPPRESSION DES SOURCES
# ==========================================================

echo ""
echo "=========================================="
echo "     SUPPRESSION DES ENREGISTREMENTS"
echo "=========================================="
echo ""

echo "Les fichiers finaux sont validés."
echo ""
echo "Suppression des enregistrements sources..."
echo ""


DELETED=0


for file in "${WAV_FILES[@]}"; do

    if rm -f "$file"; then

        echo "  ✓ $(basename "$file")"

        DELETED=$((DELETED + 1))

    else

        echo "  ⚠ Impossible de supprimer : $(basename "$file")"

    fi

done


# ==========================================================
# FIN
# ==========================================================

echo ""
echo "=========================================="
echo "                 TERMINÉ"
echo "=========================================="
echo ""

echo "Session : $SESSION"
echo ""

echo "Transcription :"
echo "  $SESSION.txt"
echo ""

echo "Archive audio :"
echo "  archives/$SESSION.wav"
echo ""

echo "$DELETED enregistrement(s) supprimé(s)."
echo ""

echo "Modèle local :"
echo "  $MODEL"
echo ""

