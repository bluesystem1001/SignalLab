include_guard(GLOBAL)

function(signallab_set_compiler_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name}
            PRIVATE
                /W4
                /permissive-
                /utf-8
        )
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -finput-charset=UTF-8
                -fexec-charset=UTF-8
        )
    endif()
endfunction()
