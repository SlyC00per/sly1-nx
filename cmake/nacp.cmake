# SPDX-License-Identifier: AGPL-3.0-or-later
# The title's record (what the homebrew menu shows: name, author, version) made from build_id.h, so
# that the version shown names the build: "0.9.0-8326f03" (a "+" after it with changes not
# committed), or the version alone with RELEASE.
file(READ ${ID_HEADER} TEXT)
string(REGEX MATCH "SLY1_VERSION \"([^\"]*)\"" _ "${TEXT}")
set(VERSION "${CMAKE_MATCH_1}")
string(REGEX MATCH "SLY1_BUILD_ID \"([^\"]*)\"" _ "${TEXT}")
set(ID "${CMAKE_MATCH_1}")
set(LABEL "${VERSION}")
if(NOT RELEASE)
    string(SUBSTRING "${ID}" 0 7 SHORT)
    set(LABEL "${VERSION}-${SHORT}")
    if(ID MATCHES "dirty")
        set(LABEL "${LABEL}+")
    endif()
endif()
execute_process(COMMAND "${NACPTOOL}" --create "${NAME}" "${AUTHOR}" "${LABEL}" "${OUT}" RESULT_VARIABLE FAILED)
if(FAILED)
    message(FATAL_ERROR "nacptool failed")
endif()
