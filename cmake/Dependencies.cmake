include_guard(GLOBAL)

find_package(
    Qt6 6.11
    REQUIRED
    COMPONENTS
        Core
        Widgets
        Charts
        Sql
)

find_path(MariaDB_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mariadb mysql REQUIRED)
find_library(MariaDB_LIBRARY NAMES mariadb libmariadb REQUIRED)

add_library(MariaDB::Client UNKNOWN IMPORTED)
set_target_properties(MariaDB::Client PROPERTIES
    IMPORTED_LOCATION "${MariaDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MariaDB_INCLUDE_DIR}"
)

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()
