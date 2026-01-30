set(BOARD_SRCS "")
if(NOT DEFINED BOARD OR BOARD STREQUAL "" OR BOARD STREQUAL ".")
    set(BOARD "adv")
endif()
string(TOLOWER "${BOARD}" BOARD_LOWER)
if(BOARD_LOWER STREQUAL "adv")
    list(APPEND BOARD_SRCS
        "drivers/keyboard_adv.c"
        "drivers/buzzer_adv.c"
    )
elseif(BOARD_LOWER STREQUAL "k132")
    list(APPEND BOARD_SRCS
        "drivers/keyboard_k132.c"
        "drivers/buzzer_k132.c"
    )
else()
    message(FATAL_ERROR "Invalid BOARD: ${BOARD}. Use -DBOARD=adv or -DBOARD=k132.")
endif()

idf_component_register(
    SRCS 
        "main.c"
        "uart_handler.c"
        "settings.c"
        "screen_manager.c"
        "drivers/display.c"
        "drivers/screenshot.c"
        "drivers/battery.c"
        ${BOARD_SRCS}
        "ui/text_ui.c"
        "screens/home_screen.c"
        "screens/wifi_scan_screen.c"
        "screens/network_list_screen.c"
        "screens/network_info_screen.c"
        "screens/attack_select_screen.c"
        "screens/deauth_screen.c"
        "screens/evil_twin_screen.c"
        "screens/evil_twin_name_screen.c"
        "screens/html_select_screen.c"
        "screens/sae_overflow_screen.c"
        "screens/handshaker_screen.c"
        "screens/sniffer_screen.c"
        "screens/sniffer_results_screen.c"
        "screens/station_deauth_screen.c"
        "screens/sniffer_probes_screen.c"
        "screens/global_attacks_screen.c"
        "screens/blackout_screen.c"
        "screens/global_handshaker_screen.c"
        "screens/text_input_screen.c"
        "screens/global_portal_html_screen.c"
        "screens/global_portal_screen.c"
        "screens/sniffer_dog_screen.c"
        "screens/wardrive_screen.c"
        "screens/sniff_karma_menu_screen.c"
        "screens/global_sniffer_screen.c"
        "screens/karma_probes_screen.c"
        "screens/karma_html_screen.c"
        "screens/karma_attack_screen.c"
        "screens/bt_menu_screen.c"
        "screens/airtag_scan_screen.c"
        "screens/bt_scan_screen.c"
        "screens/bt_locator_screen.c"
        "screens/bt_locator_track_screen.c"
        "screens/placeholder_screen.c"
        "screens/deauth_detector_screen.c"
        "screens/compromised_menu_screen.c"
        "screens/evil_twin_passwords_screen.c"
        "screens/portal_data_screen.c"
        "screens/handshakes_screen.c"
        "screens/data_detail_screen.c"
        "screens/settings_screen.c"
        "screens/uart_pins_screen.c"
        "screens/vendor_lookup_screen.c"
        "screens/gps_module_screen.c"
        "screens/gps_raw_screen.c"
        "screens/network_attacks_screen.c"
        "screens/wifi_connect_screen.c"
        "screens/arp_hosts_screen.c"
        "screens/arp_attack_screen.c"
        "screens/rogue_ap_ssid_screen.c"
        "screens/rogue_ap_password_screen.c"
        "screens/rogue_ap_html_screen.c"
        "screens/rogue_ap_screen.c"
    INCLUDE_DIRS 
        "."
        "drivers"
        "screens"
        "ui"
    REQUIRES 
        driver
        esp_lcd
        esp_timer
        esp_driver_uart
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2s
        fatfs
        vfs
        sdmmc
        esp_adc
        nvs_flash
)

if(BOARD_LOWER STREQUAL "adv")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_ADV=1)
elseif(BOARD_LOWER STREQUAL "k132")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_K132=1)
endif()
