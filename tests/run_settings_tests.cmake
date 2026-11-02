file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY "${TEST_BINARY}" DESTINATION "${TEST_DIR}")
# Portable mode: settings.ini beside the executable keeps every write inside
# this folder instead of the developer's %APPDATA%\Tinta
file(WRITE "${TEST_DIR}/settings.ini" "[Settings]\nhasAskedFileAssociation=1\nlanguage=en\n")
get_filename_component(TEST_NAME "${TEST_BINARY}" NAME)
execute_process(COMMAND "${TEST_DIR}/${TEST_NAME}" --settings-tests
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Settings persistence tests failed: ${result}")
endif()
