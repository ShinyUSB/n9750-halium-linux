#!/bin/sh
# d2q mic capture route — mirrors stock /vendor/etc/mixer_paths_tavil.xml
#
# Three physical mics (verified 2026-11-08):
#   main-mic  = DMIC0 -> DEC5 -> SLIM TX5 (MIC BIAS1)   [bottom, primary]
#   sub-mic   = DMIC2 -> DEC6 -> SLIM TX6 (MIC BIAS3)   [top]
#   3rd-mic   = DMIC5 -> DEC7 -> SLIM TX7 (MIC BIAS4)   [third]
#   4th-mic   = DMIC4 -> DEC8 -> SLIM TX8 is a MIRROR of DMIC5 (corr=1.0000),
#               stock mutes it: "DEC8 Volume" = 0. Not a real input.
#
# Modes:
#   main   (default)  mono hw:0,0 capture from the primary mic
#   multi             4ch capture: ch0=main ch1=sub ch2=3rd ch3=mirror
#
# The FE routing is "audio-record": MultiMedia1 Mixer SLIM_0_TX=1.
# MIC BIAS1/3/4 come up automatically via the card's qcom,audio-routing DAPM pairs.

MODE="${1:-main}"
C="amixer -c0 cset"

$C name='MultiMedia1 Mixer SLIM_0_TX' 1 >/dev/null

case "$MODE" in
main)
	$C name='SLIM_0_TX Channels' One >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX5' 1 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX6' 0 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX7' 0 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX8' 0 >/dev/null
	$C name='CDC_IF TX5 MUX' DEC5 >/dev/null
	$C name='ADC MUX5' DMIC >/dev/null
	$C name='DMIC MUX5' DMIC0 >/dev/null
	$C name='DEC5 Volume' 84 >/dev/null
	$C name='DEC8 Volume' 0 >/dev/null
	;;
multi)
	$C name='SLIM_0_TX Channels' Four >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX5' 1 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX6' 1 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX7' 1 >/dev/null
	$C name='AIF1_CAP Mixer SLIM TX8' 1 >/dev/null
	$C name='CDC_IF TX5 MUX' DEC5 >/dev/null
	$C name='CDC_IF TX6 MUX' DEC6 >/dev/null
	$C name='CDC_IF TX7 MUX' DEC7 >/dev/null
	$C name='CDC_IF TX8 MUX' DEC8 >/dev/null
	$C name='ADC MUX5' DMIC >/dev/null
	$C name='ADC MUX6' DMIC >/dev/null
	$C name='ADC MUX7' DMIC >/dev/null
	$C name='ADC MUX8' DMIC >/dev/null
	$C name='DMIC MUX5' DMIC0 >/dev/null
	$C name='DMIC MUX6' DMIC2 >/dev/null
	$C name='DMIC MUX7' DMIC5 >/dev/null
	$C name='DMIC MUX8' DMIC4 >/dev/null
	$C name='DEC5 Volume' 84 >/dev/null
	$C name='DEC6 Volume' 84 >/dev/null
	$C name='DEC7 Volume' 84 >/dev/null
	$C name='DEC8 Volume' 0 >/dev/null
	;;
*)
	echo "usage: $0 [main|multi]" >&2
	exit 1
	;;
esac

echo "d2q-mic: route applied, mode=$MODE"
