add_executable(uwb_unit_tests
    unit/smoke_test.cpp
)

target_compile_features(uwb_unit_tests PRIVATE cxx_std_20)
target_link_libraries(uwb_unit_tests PRIVATE
    Catch2::Catch2WithMain
    uwb::protocol
    uwb::domain
    uwb::server_core
    uwb::client_core
)

uwb_apply_warnings(uwb_unit_tests)
uwb_apply_sanitizers(uwb_unit_tests)

include(Catch)
catch_discover_tests(uwb_unit_tests PROPERTIES LABELS "unit")

# Hardware-in-the-loop tests are opt-in and never part of the default test run.
if(UWB_ENABLE_HIL_TESTS)
    add_executable(uwb_hil_tests
        hil/hil_smoke_test.cpp
    )

    target_compile_features(uwb_hil_tests PRIVATE cxx_std_20)
    target_link_libraries(uwb_hil_tests PRIVATE
        Catch2::Catch2WithMain
        uwb::protocol
        uwb::domain
    )

    target_compile_definitions(uwb_hil_tests PRIVATE
        UWB_HIL_SERIAL_DEVICE="${HIL_SERIAL_DEVICE}"
        UWB_HIL_PICO_HOST="${HIL_PICO_HOST}"
    )

    uwb_apply_warnings(uwb_hil_tests)

    catch_discover_tests(uwb_hil_tests PROPERTIES LABELS "hil")
endif()
