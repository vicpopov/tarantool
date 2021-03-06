#
# A macro to build the bundled libmisc
macro(libmisc_build)
    set(misc_src
        ${PROJECT_SOURCE_DIR}/third_party/sha1.c
        ${PROJECT_SOURCE_DIR}/third_party/PMurHash.c
        ${PROJECT_SOURCE_DIR}/third_party/base64.c
        ${PROJECT_SOURCE_DIR}/third_party/qsort_arg.c
    )

    if (CC_HAS_WNO_IMPLICIT_FALLTHROUGH)
        # Disable false-positive warnings in switch() {} block
        set_source_files_properties(${PROJECT_SOURCE_DIR}/third_party/base64.c
            PROPERTIES COMPILE_FLAGS -Wno-implicit-fallthrough)
    endif()

    if (NOT HAVE_MEMMEM)
        list(APPEND misc_src
            ${PROJECT_SOURCE_DIR}/third_party/memmem.c
        )
    endif()

    if (NOT HAVE_MEMRCHR)
        list(APPEND misc_src
            ${PROJECT_SOURCE_DIR}/third_party/memrchr.c
        )
    endif()

    if (NOT HAVE_CLOCK_GETTIME)
        list(APPEND misc_src
            ${PROJECT_SOURCE_DIR}/third_party/clock_gettime.c
        )
    endif()

    if (HAVE_OPENMP)
        list(APPEND misc_src
             ${PROJECT_SOURCE_DIR}/third_party/qsort_arg_mt.c)
    endif()

    add_library(misc STATIC ${misc_src})

    unset(misc_src)
endmacro(libmisc_build)
