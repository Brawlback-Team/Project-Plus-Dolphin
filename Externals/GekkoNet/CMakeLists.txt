add_library(GekkoNet STATIC
  src/backend.cpp
  src/event.cpp
  src/game_session.cpp
  src/gekkonet.cpp
  src/input.cpp
  src/net.cpp
  src/player.cpp
  src/spectator_session.cpp
  src/storage.cpp
  src/stress_session.cpp
  src/sync.cpp
)

target_include_directories(GekkoNet
PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

# Dolphin supplies the UDP transport through its traversed ENet socket. Exclude
# GekkoNet's standalone Asio adapter so Dolphin does not vendor a second stack.
target_compile_definitions(GekkoNet PUBLIC GEKKONET_STATIC GEKKONET_NO_ASIO)
target_compile_features(GekkoNet PUBLIC cxx_std_17)

if(COMMAND dolphin_disable_warnings)
  dolphin_disable_warnings(GekkoNet)
endif()

if(MSVC)
  target_compile_options(GekkoNet PRIVATE /GR /EHsc /W0)
endif()
