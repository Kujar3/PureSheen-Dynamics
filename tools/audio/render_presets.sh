#!/bin/sh
# Integration render check (Git Bash, from the repo root):
#   python tools/audio/make_test_mix.py test_audio/int_mix.wav
#   sh tools/audio/render_presets.sh [buildDir] [input.wav]
# Renders the input through the default chain and every global factory preset into out/renders and prints
# integrated loudness, true peak, LRA, latency and the NaN / click check (tools/audio/check_clicks.py) per preset.
B=${1:-build-int}
IN=${2:-test_audio/int_mix.wav}
D="$B/PureSheenDev_artefacts/Release/PureSheenDev.exe"
mkdir -p out/renders
printf "%-22s %9s %9s %7s %6s  %s\n" preset "LUFS-I" "TP dBTP" "LRA" "lat" "check"
for P in "Default" "Soft Sheen" "Streaming Ready" "Loud and Clear" "Club Loud" "Wide Supersaw Master" \
         "Deep Bass Anchor" "Acoustic Air" "Warm Glue" "Limit Only" "Radio Pop" "Rock Drive" "Hip-Hop Knock" \
         "Mid-Side Detail" "Modern Loud Balance" "Gentle Acoustic Master" "Heavy Low End Master"          "Clean Vocal Pop"; do
    F="out/renders/$(echo "$P" | tr ' ' '_').wav"
    LOG=$("$D" render "$IN" "$F" --preset "$P" 2>&1)
    OUTL=$(echo "$LOG" | grep "output:")
    I=$(echo "$OUTL" | sed -E 's/.*integrated (-?[0-9.]+|-inf) LUFS.*/\1/')
    L=$(echo "$OUTL" | sed -E 's/.*LRA (-?[0-9.]+) LU.*/\1/')
    T=$(echo "$OUTL" | sed -E 's/.*true peak (-?[0-9.]+|-inf) dBTP.*/\1/')
    LAT=$(echo "$LOG" | grep "latency" | sed -E 's/.*latency ([0-9]+).*/\1/')
    C=$(python tools/audio/check_clicks.py "$IN" "$F")
    printf "%-22s %9s %9s %7s %6s  %s\n" "$P" "$I" "$T" "$L" "$LAT" "$C"
done
