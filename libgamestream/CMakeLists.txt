set(SO_VERSION 4)

if (ENABLE_VIDAA)
  aux_source_directory(./ GAMESTREAM_SRC_LIST)
  list(FILTER GAMESTREAM_SRC_LIST EXCLUDE REGEX "discover\\.c$")
  list(APPEND GAMESTREAM_SRC_LIST ./discover_stub.c)
  aux_source_directory(../third_party/h264bitstream GAMESTREAM_SRC_LIST)

  aux_source_directory(../third_party/moonlight-common-c/enet MOONLIGHT_COMMON_SRC_LIST)
  aux_source_directory(../third_party/moonlight-common-c/src MOONLIGHT_COMMON_SRC_LIST)
  aux_source_directory(../third_party/moonlight-common-c/reedsolomon MOONLIGHT_COMMON_SRC_LIST)

  add_library(moonlight-common STATIC ${MOONLIGHT_COMMON_SRC_LIST})
  add_library(gamestream STATIC ${GAMESTREAM_SRC_LIST})

  target_include_directories(moonlight-common PRIVATE
    ../third_party/moonlight-common-c/reedsolomon
    ../third_party/moonlight-common-c/enet/include
    "${VIDAA_VENDOR_ROOT}/openssl-1.1.1i/include"
  )
  target_include_directories(gamestream PRIVATE
    ../third_party/moonlight-common-c/src
    ../third_party/h264bitstream
    "${VIDAA_VENDOR_ROOT}/curl-7.77.0/include"
    "${VIDAA_VENDOR_ROOT}/openssl-1.1.1i/include"
    "${VIDAA_VENDOR_ROOT}/expat-2.2.10/expat/lib"
    "${VIDAA_VENDOR_ROOT}/vidaa-include"
  )
  target_link_libraries(gamestream PUBLIC
    moonlight-common
    Threads::Threads
    ${CMAKE_DL_LIBS}
    "${VIDAA_VENDOR_ROOT}/../curl-build-vidaa/lib/libcurl.a"
    "${VIDAA_VENDOR_ROOT}/openssl-1.1.1i/libssl.a"
    "${VIDAA_VENDOR_ROOT}/openssl-1.1.1i/libcrypto.a"
    "${VIDAA_SYSROOT}/usr/local/lib/libexpat.so.1.6.12"
    "${VIDAA_SYSROOT}/usr/local/lib/libuuid.so.1.3.0"
  )
  return()
endif()

find_package(LibUUID REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL 1.1.0 REQUIRED)
find_package(EXPAT REQUIRED)

pkg_check_modules(AVAHI REQUIRED avahi-client)

aux_source_directory(./ GAMESTREAM_SRC_LIST)
aux_source_directory(../third_party/h264bitstream GAMESTREAM_SRC_LIST)

aux_source_directory(../third_party/moonlight-common-c/enet MOONLIGHT_COMMON_SRC_LIST)
aux_source_directory(../third_party/moonlight-common-c/src MOONLIGHT_COMMON_SRC_LIST)
aux_source_directory(../third_party/moonlight-common-c/reedsolomon MOONLIGHT_COMMON_SRC_LIST)

add_library(moonlight-common SHARED ${MOONLIGHT_COMMON_SRC_LIST})

add_library(gamestream SHARED ${GAMESTREAM_SRC_LIST})
target_link_libraries(gamestream moonlight-common)

set_target_properties(gamestream PROPERTIES SOVERSION ${SO_VERSION} VERSION ${PROJECT_VERSION})
set_target_properties(moonlight-common PROPERTIES SOVERSION ${SO_VERSION} VERSION ${PROJECT_VERSION})

target_include_directories(gamestream PRIVATE ../third_party/moonlight-common-c/src ../third_party/h264bitstream ${AVAHI_INCLUDE_DIRS} ${LibUUID_INCLUDE_DIRS})
target_include_directories(moonlight-common PRIVATE ../third_party/moonlight-common-c/reedsolomon ../third_party/moonlight-common-c/enet/include)
target_link_libraries(gamestream ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES} ${EXPAT_LIBRARIES} ${AVAHI_LIBRARIES} ${LibUUID_LIBRARIES})

target_link_libraries(gamestream ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

install(TARGETS gamestream moonlight-common DESTINATION ${CMAKE_INSTALL_LIBDIR})
