# abspresso/ holds unmodified copies of ABSpresso's shared code (tools/sync_abspresso.sh).
# hardware/, ui/ and compat/ are the Yoto Player Gen 2 side.
# setup/ is this player's own setup portal; it replaces ABSpresso's portal.c, which is
# copied with the rest but not built.
idf_component_register(SRC_DIRS "." "hardware" "ui" "setup" "web" "abspresso/network" "abspresso/app"
                       EXCLUDE_SRCS "abspresso/network/portal.c"
                       INCLUDE_DIRS "." "hardware" "ui" "setup" "web" "compat" "abspresso/network" "abspresso/app")

# ABSpresso's player logs its pipeline statistics once a second when this is defined
# (buffer fill, bytes in, decode errors, slowest audio write, underruns). On while audio
# quality on this board is being investigated.
target_compile_definitions(${COMPONENT_LIB} PRIVATE PLAYER_STATUS_LOG)
