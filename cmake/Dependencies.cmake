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

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
endif()
