include(CheckCXXSourceRuns)

find_path(GTest_INCLUDE_DIR gtest/gtest.h
          NO_DEFAULT_PATH
          PATHS
          "/opt/dependency/package/include"
          "${PROJECT_SOURCE_DIR}/../thirdparty/googletest/googletest/include"
          "/usr/local/include"
          "/usr/include")

find_path(GMock_INCLUDE_DIR gmock/gmock.h
          NO_DEFAULT_PATH
          PATHS
          "/opt/dependency/package/include"
          "${PROJECT_SOURCE_DIR}/../thirdparty/googletest/googlemock/include"
          "/usr/local/include"
          "/usr/include")

find_library(Gtest_LIBRARY
             NAMES libgtest.a
             HINTS
             "/opt/dependency/package/lib"
             "${PROJECT_SOURCE_DIR}/../thirdparty/googletest/build/googlemock/gtest"
             "/usr/local/lib"
             "/usr/lib")

find_library(Gmock_LIBRARY
             NAMES libgmock.a
             HINTS
             "/opt/dependency/package/lib"
             "${PROJECT_SOURCE_DIR}/../thirdparty/googletest/build/googlemock"
             "/usr/local/lib"
             "/usr/lib")

message(STATUS "Find GoogleTest include path: ${GTest_INCLUDE_DIR}")
message(STATUS "Find GoogleMock include path: ${GMock_INCLUDE_DIR}")
message(STATUS "Find Gtest library path: ${Gtest_LIBRARY}")
message(STATUS "Find Gmock library path: ${Gmock_LIBRARY}")

set(CMAKE_REQUIRED_INCLUDES ${GTest_INCLUDE_DIR} ${GMock_INCLUDE_DIR})
set(CMAKE_REQUIRED_LIBRARIES ${Gtest_LIBRARY} ${Gmock_LIBRARY})
set(CMAKE_REQUIRED_FLAGS "-lpthread")
IF(OS_LINUX)
    SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -lc++")
ENDIF(OS_LINUX)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    GoogleTest
    REQUIRED_VARS
    GTest_INCLUDE_DIR
    GMock_INCLUDE_DIR
    Gtest_LIBRARY
    Gmock_LIBRARY
)

set(GoogleTest_INCLUDE_DIR ${GTest_INCLUDE_DIR} ${GMock_INCLUDE_DIR})
set(GoogleTest_LIBRARIES ${Gtest_LIBRARY} ${Gmock_LIBRARY})
mark_as_advanced(
    GoogleTest_INCLUDE_DIR
    GoogleTest_LIBRARIES)
