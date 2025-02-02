IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    SET(OS_LINUX true CACHE INTERNAL "Linux operating system")
ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    SET(OS_MACOSX true CACHE INTERNAL "Mac Darwin operating system")
ELSE(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    MESSAGE(FATAL_ERROR "Unsupported OS: \"${CMAKE_SYSTEM_NAME}\"")
ENDIF(CMAKE_SYSTEM_NAME STREQUAL "Linux")

IF(CMAKE_COMPILER_IS_GNUCXX)
    EXECUTE_PROCESS(COMMAND ${CMAKE_CXX_COMPILER} -dumpfullversion OUTPUT_VARIABLE GCC_COMPILER_VERSION)
    
    IF (NOT GCC_COMPILER_VERSION)
        MESSAGE(FATAL_ERROR "Cannot get gcc version")
    ENDIF (NOT GCC_COMPILER_VERSION)
    
    STRING(REGEX MATCHALL "[0-9]+" GCC_COMPILER_VERSION ${GCC_COMPILER_VERSION})
    
    LIST(GET GCC_COMPILER_VERSION 0 GCC_COMPILER_VERSION_MAJOR)
    LIST(GET GCC_COMPILER_VERSION 1 GCC_COMPILER_VERSION_MINOR)
    
    SET(GCC_COMPILER_VERSION_MAJOR ${GCC_COMPILER_VERSION_MAJOR} CACHE INTERNAL "gcc major version")
    SET(GCC_COMPILER_VERSION_MINOR ${GCC_COMPILER_VERSION_MINOR} CACHE INTERNAL "gcc minor version")
    
    MESSAGE(STATUS "checking compiler: GCC (${GCC_COMPILER_VERSION_MAJOR}.${GCC_COMPILER_VERSION_MINOR}.${GCC_COMPILER_VERSION_PATCH})")
ELSE(CMAKE_COMPILER_IS_GNUCXX)
    EXECUTE_PROCESS(COMMAND ${CMAKE_C_COMPILER} --version  OUTPUT_VARIABLE COMPILER_OUTPUT)
    IF(COMPILER_OUTPUT MATCHES "clang")
        SET(CMAKE_COMPILER_IS_CLANG true CACHE INTERNAL "using clang as compiler")
        MESSAGE(STATUS "checking compiler: CLANG")
    ELSE(COMPILER_OUTPUT MATCHES "clang")
        MESSAGE(FATAL_ERROR "Unsupported compiler: \"${CMAKE_CXX_COMPILER}\"")
    ENDIF(COMPILER_OUTPUT MATCHES "clang")
ENDIF(CMAKE_COMPILER_IS_GNUCXX)
