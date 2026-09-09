# esp_emote_gen_player still requires the legacy component name `json`.
# Forward it to the existing managed cJSON component without compiling a
# second copy of cJSON.
idf_component_register(REQUIRES cjson)
