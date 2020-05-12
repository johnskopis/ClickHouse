# This strings autochanged from release_lib.sh:
set(VERSION_REVISION 54434)
set(VERSION_MAJOR 20)
set(VERSION_MINOR 4)
set(VERSION_PATCH 3)
set(VERSION_GITHASH 49c37c0787b59a509d40a633e8b2b64533466009)
set(VERSION_DESCRIBE v20.4.3.1-stable)
set(VERSION_STRING 20.4.3.1)
# end of autochange

set(VERSION_EXTRA "" CACHE STRING "")
set(VERSION_TWEAK "" CACHE STRING "")

if (VERSION_TWEAK)
    string(CONCAT VERSION_STRING ${VERSION_STRING} "." ${VERSION_TWEAK})
endif ()

if (VERSION_EXTRA)
    string(CONCAT VERSION_STRING ${VERSION_STRING} "." ${VERSION_EXTRA})
endif ()

set (VERSION_NAME "${PROJECT_NAME}")
set (VERSION_FULL "${VERSION_NAME} ${VERSION_STRING}")
set (VERSION_SO "${VERSION_STRING}")

math (EXPR VERSION_INTEGER "${VERSION_PATCH} + ${VERSION_MINOR}*1000 + ${VERSION_MAJOR}*1000000")

if(YANDEX_OFFICIAL_BUILD)
    set(VERSION_OFFICIAL " (official build)")
endif()
