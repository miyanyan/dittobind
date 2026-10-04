set(prefix "${BUILD}/install-test/prefix")
set(consumer "${BUILD}/install-test/consumer")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --config "${CONFIG}" --prefix "${prefix}"
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
set(platform_argument)
if(PLATFORM)
    list(APPEND platform_argument -A "${PLATFORM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/tests/install_consumer" -B "${consumer}"
    -G "${GENERATOR}" ${platform_argument} "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DPython_EXECUTABLE=${PYTHON}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${consumer}" --config "${CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${consumer}" -C "${CONFIG}" --output-on-failure
    COMMAND_ERROR_IS_FATAL ANY)
