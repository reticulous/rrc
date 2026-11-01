include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
# Conditional sources: esp-idf/conditional/<name>/ is compiled only when <name>
# is staged. The LCD app (RrcApp) lives in conditional/spangap-lcd/ and the
# mono-OLED status page in conditional/tinylcd/ — no #if anywhere.
idf_component_register(
    SRCS
        "src/rrc.cpp"
        "src/rrc_cbor.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        ${SPANGAP_REQUIRES}
        json
        esp_timer
)
