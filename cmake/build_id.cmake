# SPDX-License-Identifier: AGPL-3.0-or-later
# build_id.h: the version and the commit the build is of ("-dirty" with changes not committed),
# written only when it changes.
execute_process(COMMAND git describe --always --dirty --abbrev=10 WORKING_DIRECTORY ${SRC} OUTPUT_VARIABLE ID
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT ID)
    set(ID "unknown")
endif()
set(TEXT "#define SLY1_VERSION \"${VERSION}\"\n#define SLY1_BUILD_ID \"${ID}\"\n")
set(WAS "")
if(EXISTS ${OUT})
    file(READ ${OUT} WAS)
endif()
if(NOT WAS STREQUAL TEXT)
    file(WRITE ${OUT} "${TEXT}")
endif()
