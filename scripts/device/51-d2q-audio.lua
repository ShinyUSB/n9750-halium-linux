-- d2q: tavil ALSA nodes at full volume on creation.
-- Without UCM, WirePlumber's ACP inits the stereo-fallback profile at ~17%
-- (-46.5 dB) — desktop audio is effectively inaudible until this is pinned.
rule = {
	matches = {
		{ { "node.name", "matches", "alsa_output.*sound-tavil*" } },
		{ { "node.name", "matches", "alsa_input.*sound-tavil*" } },
	},
	apply_properties = { ["node.volume"] = 1.0 },
}
table.insert(alsa_monitor.rules, rule)
