cmake_minimum_required(VERSION 3.16)
project(raylib_racers VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RR_BUILD_VIEWER "Build the raylib 3D viewer (rr_viewer)" ON)
option(RR_BUILD_BOTS "Build the example robots" ON)

# Everything lands next to the executables: build/rr_race, build/bots/*.so
set(RR_OUT "${CMAKE_BINARY_DIR}")
function(rr_output target dir)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${dir}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${dir}>")
endfunction()

if(MSVC)
  set(RR_WARNINGS /W3)
else()
  set(RR_WARNINGS -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- simulation core (no graphics)
add_library(rr_core STATIC
  src/core/track.cpp
  src/core/car.cpp
  src/core/race.cpp
  src/core/neutral.cpp
  src/core/robot_loader.cpp
  src/core/config.cpp
  src/core/spec.cpp
  src/core/testlog.cpp
  src/core/championship.cpp
  src/core/weekend.cpp
  src/core/robot_driver.cpp)
target_include_directories(rr_core PUBLIC include src/core)
target_compile_options(rr_core PRIVATE ${RR_WARNINGS})
target_compile_definitions(rr_core PUBLIC RR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(rr_core PUBLIC ${CMAKE_DL_LIBS})

add_executable(rr_race apps/headless/main.cpp)
target_link_libraries(rr_race PRIVATE rr_core)
target_compile_options(rr_race PRIVATE ${RR_WARNINGS})
rr_output(rr_race "${RR_OUT}")

# The sandbox: runs one robot in its own locked-down process (--sandbox).
add_executable(rr_bothost apps/bothost/main.cpp)
target_link_libraries(rr_bothost PRIVATE rr_core)
target_compile_options(rr_bothost PRIVATE ${RR_WARNINGS})
rr_output(rr_bothost "${RR_OUT}")
if(WIN32)
  target_link_libraries(rr_core PUBLIC advapi32)
endif()

# ---------------------------------------------------------------- example robots
function(rr_add_robot name)
  add_library(${name} MODULE ${ARGN})
  target_include_directories(${name} PRIVATE include)
  target_compile_options(${name} PRIVATE ${RR_WARNINGS})
  set_target_properties(${name} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  rr_output(${name} "${RR_OUT}/bots")
endfunction()

if(RR_BUILD_BOTS)
  # Every folder under bots/ (except common/) with .c/.cpp files is a robot named
  # after the folder. CONFIGURE_DEPENDS re-runs the scan when files are added.
  file(GLOB RR_BOT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/bots/*/*.c ${CMAKE_CURRENT_SOURCE_DIR}/bots/*/*.cpp)
  set(RR_BOTS "")
  foreach(src ${RR_BOT_SOURCES})
    get_filename_component(dir "${src}" DIRECTORY)
    get_filename_component(bot "${dir}" NAME)
    if(NOT bot STREQUAL "common" AND NOT bot IN_LIST RR_BOTS)
      list(APPEND RR_BOTS ${bot})
    endif()
  endforeach()
  foreach(bot ${RR_BOTS})
    file(GLOB src CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/bots/${bot}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/bots/${bot}/*.cpp)
    rr_add_robot(${bot} ${src})
    if(NOT MSVC)
      target_link_libraries(${bot} PRIVATE m)
    endif()
  endforeach()
  message(STATUS "Robots: ${RR_BOTS}")
endif()

# ---------------------------------------------------------------- viewer
if(RR_BUILD_VIEWER)
  find_package(raylib 5.5 QUIET)
  if(NOT raylib_FOUND)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
      GIT_REPOSITORY https://github.com/raysan5/raylib.git
      GIT_TAG 5.5
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(raylib)
  endif()

  add_executable(rr_viewer
    apps/viewer/main.cpp
    apps/viewer/renderer.cpp
    apps/viewer/director.cpp
    apps/viewer/effects.cpp
    apps/viewer/hud.cpp
    apps/viewer/car_model.cpp
    apps/viewer/engine_sound.cpp
    apps/viewer/surface_sound.cpp
    apps/viewer/race_audio.cpp
    apps/viewer/menu.cpp
    apps/viewer/hud_test.cpp apps/viewer/hud_results.cpp apps/viewer/hud_menu.cpp
    apps/viewer/test_figures.cpp
    apps/viewer/liveries.cpp apps/viewer/short_names.cpp apps/viewer/hud_icons.cpp
    apps/viewer/scenery.cpp)
  target_link_libraries(rr_viewer PRIVATE rr_core raylib)
  target_compile_options(rr_viewer PRIVATE ${RR_WARNINGS} $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>)
  rr_output(rr_viewer "${RR_OUT}")
  if(APPLE)
    target_link_libraries(rr_viewer PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
  endif()

  # RR2's PBR renderer and 3D track, shared by the track view and viewer v2
  add_library(rr_gfx STATIC
    apps/trackview/gfx.cpp
    apps/trackview/track_scene.cpp)
  target_include_directories(rr_gfx PUBLIC apps/trackview)
  target_link_libraries(rr_gfx PUBLIC rr_core raylib)
  target_compile_options(rr_gfx PRIVATE ${RR_WARNINGS} $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>)

  # the track on its own (no race, no cars)
  add_executable(rr_trackview apps/trackview/main.cpp)
  target_link_libraries(rr_trackview PRIVATE rr_gfx)
  target_compile_options(rr_trackview PRIVATE ${RR_WARNINGS} $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>)
  rr_output(rr_trackview "${RR_OUT}")
  if(APPLE)
    target_link_libraries(rr_trackview PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
  endif()

  # viewer v2: races on the new renderer (one car for now)
  # The RR1 viewer's own main, menu, HUD, testing screens, director and sound, built with
  # RR2_RENDERER so that its Renderer is the one in apps/viewer2/renderer2.* (PBR, 2013 cars).
  add_executable(rr_viewer2
    apps/viewer/main.cpp
    apps/viewer/director.cpp
    apps/viewer/hud.cpp apps/viewer/hud_results.cpp apps/viewer/hud_menu.cpp apps/viewer/hud_test.cpp
    apps/viewer/menu.cpp apps/viewer/test_figures.cpp apps/viewer/liveries.cpp
    apps/viewer/short_names.cpp apps/viewer/hud_icons.cpp
    apps/viewer/engine_sound.cpp apps/viewer/surface_sound.cpp apps/viewer/race_audio.cpp
    apps/viewer2/renderer2.cpp
    apps/viewer2/car_render.cpp)
  target_compile_definitions(rr_viewer2 PRIVATE RR2_RENDERER=1)
  target_include_directories(rr_viewer2 PRIVATE apps/viewer apps/viewer2)
  target_link_libraries(rr_viewer2 PRIVATE rr_gfx)
  target_compile_options(rr_viewer2 PRIVATE ${RR_WARNINGS} $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>)
  rr_output(rr_viewer2 "${RR_OUT}")
  if(APPLE)
    target_link_libraries(rr_viewer2 PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
  endif()
endif()

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(test_surfaces tests/surfaces/main.cpp)
target_link_libraries(test_surfaces PRIVATE rr_core)
target_compile_options(test_surfaces PRIVATE ${RR_WARNINGS})
add_test(NAME surfaces COMMAND test_surfaces WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(test_tyres tests/tyres/main.cpp)
target_link_libraries(test_tyres PRIVATE rr_core)
target_compile_options(test_tyres PRIVATE ${RR_WARNINGS})
add_test(NAME tyres COMMAND test_tyres)
if(RR_BUILD_BOTS)
  add_test(NAME race_smoke
    COMMAND rr_race --laps 1 --quiet --car simple --car gapfollow --car racingline
            --json ${CMAKE_BINARY_DIR}/smoke_results.json)
  # The racingline family: Dave, Spongebob and Granny Doris are racingline with other defaults.
  add_test(NAME race_named_bots COMMAND rr_race --laps 1 --quiet --car dave --car spongebob --car granny)
  add_test(NAME race_oval COMMAND rr_race --track oval --laps 1 --quiet --car racingline)
  # Fast tyre wear forces a stop: the racingline robot must pit and finish.
  add_test(NAME race_test_log COMMAND rr_race --laps 2 --quiet --car racingline --no-pits --fuel 8 --tires soft
           --test-log ${CMAKE_BINARY_DIR}/ctest_test_runs)
  add_test(NAME race_pit_stop COMMAND rr_race --laps 6 --wear-rate 6 --quiet --car racingline)
  set_tests_properties(race_pit_stop PROPERTIES PASS_REGULAR_EXPRESSION "racingline +[0-9]+:[0-9.]+ .* [1-9] \\(L[0-9]")
  # After the flag every car drives into the pit lane and parks.
  add_test(NAME race_cool_down COMMAND rr_race --laps 1 --quiet --cool-down
           --car racingline --car racingline --car racingline --car gapfollow --car simple)
  set_tests_properties(race_cool_down PROPERTIES PASS_REGULAR_EXPRESSION "cool-down: 5/5 cars parked")
  # Asymmetric cars: a spec file and development tokens load; an over-budget car is refused.
  add_test(NAME race_specs COMMAND rr_race --laps 1 --quiet --car racingline --spec f1_2006
           --car racingline --dev "top_speed=8,downforce=2,pit_stop=5")
  add_test(NAME race_dev_budget COMMAND rr_race --laps 1 --quiet --car racingline --dev "top_speed=10,downforce=10,handling=10")
  set_tests_properties(race_dev_budget PROPERTIES WILL_FAIL TRUE)
  # The 2013 stats (KERS, DRS, gearbox, pit crew), and an old lineup that still names the retired fuel_efficiency stat.
  add_test(NAME race_dev_2013 COMMAND rr_race --laps 2 --quiet --car racingline --spec f1_2013
           --dev "kers=7,drs=6,gearbox=6,top_speed=6,pit_stop=2,brakes=3" --car racingline --spec f1_2013
           --dev "fuel_efficiency=7,tire_management=8,pit_stop=2")
  # A two-round season: a lineup file starts it, each run races a round and saves the
  # standings; the second run picks up where the first stopped.
  add_test(NAME champ_lineup COMMAND rr_race --save-lineup ${CMAKE_BINARY_DIR}/ctest_lineup.json
           --car racingline --dev "top_speed=7,brakes=3" --car dave --car spongebob --car granny)
  add_test(NAME champ_clean COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/ctest_season.json
           ${CMAKE_BINARY_DIR}/ctest_season_km.json)
  add_test(NAME champ_round1 COMMAND rr_race --championship ${CMAKE_BINARY_DIR}/ctest_season.json
           --lineup ${CMAKE_BINARY_DIR}/ctest_lineup.json --rounds oval:1,circuit:1)
  add_test(NAME champ_round2 COMMAND rr_race --championship ${CMAKE_BINARY_DIR}/ctest_season.json)
  set_tests_properties(champ_round1 PROPERTIES DEPENDS "champ_lineup;champ_clean"
                       PASS_REGULAR_EXPRESSION "after round 1 of 2")
  set_tests_properties(champ_round2 PROPERTIES DEPENDS champ_round1
                       PASS_REGULAR_EXPRESSION "after round 2 of 2")
  # --distance: each round's laps cover about that distance (oval 2.1 km: 3 laps)
  add_test(NAME champ_distance COMMAND rr_race --championship ${CMAKE_BINARY_DIR}/ctest_season_km.json
           --car racingline --car dave --rounds oval,circuit --distance 6.4)
  set_tests_properties(champ_distance PROPERTIES DEPENDS champ_clean RESOURCE_LOCK season PASS_REGULAR_EXPRESSION "Round 1 of 2: [^\n]*, 3 laps")
  set_tests_properties(champ_lineup champ_clean champ_round1 champ_round2 PROPERTIES RESOURCE_LOCK season)
  # Incidents, yellow flags and the VSC (ABI 14): a robot that stops on the track is held, the field runs under
  # the VSC, then the car is released and, still stuck, taken off. incident13 is the same robot as ABI 13.
  rr_add_robot(incident tests/incident/incident.c)
  rr_output(incident "${RR_OUT}/test_bots")
  rr_add_robot(incident13 tests/incident/incident.c)
  target_compile_definitions(incident13 PRIVATE TEST_ABI=13)
  rr_output(incident13 "${RR_OUT}/test_bots")
  add_test(NAME vsc_incident COMMAND rr_race --track oval --laps 3 --car $<TARGET_FILE:incident> --params pos=-0.6,stop=10
           --car $<TARGET_FILE:incident> --params pos=0.5,v=45,heed=1,check=1)
  set_tests_properties(vsc_incident PROPERTIES PASS_REGULAR_EXPRESSION "VSC from lap 1[^\n]*\n[^\n]*stopped\n[^\n]*released\n[^\n]*removed")
  add_test(NAME vsc_off COMMAND rr_race --vsc off --track oval --laps 3 --car $<TARGET_FILE:incident> --params pos=-0.6,stop=10
           --car $<TARGET_FILE:incident> --params pos=0.5,v=45,heed=1,check=1)
  set_tests_properties(vsc_off PROPERTIES FAIL_REGULAR_EXPRESSION "VSC from")
  # a robot of ABI 13 knows nothing of the cap: the host holds it to it
  add_test(NAME vsc_legacy_cap COMMAND rr_race --track oval --laps 3 --car $<TARGET_FILE:incident> --params pos=-0.6,stop=10
           --car $<TARGET_FILE:incident13> --params pos=0.5,v=55,check=1)
  set_tests_properties(vsc_legacy_cap PROPERTIES PASS_REGULAR_EXPRESSION "VSC from lap 1")
  # Weekends: memcheck refuses to start unless the weekend memory carried the
  # earlier sessions (practice, then qualifying) into the race.
  rr_add_robot(memcheck tests/memcheck/memcheck.c)
  rr_output(memcheck "${RR_OUT}/test_bots")  # not offered in the viewer's menus
  # Track limits: all four wheels beyond the white line earns penalties; two wheels beyond it does not
  rr_add_robot(limits tests/limits/limits.c)
  rr_output(limits "${RR_OUT}/test_bots")
  add_test(NAME limits_inside COMMAND rr_race --track oval --laps 2 --quiet --car $<TARGET_FILE:limits> --params pos=-0.9)
  set_tests_properties(limits_inside PROPERTIES FAIL_REGULAR_EXPRESSION "pen \+")
  add_test(NAME limits_outside COMMAND rr_race --track oval --laps 2 --quiet --car $<TARGET_FILE:limits> --params pos=-1.3)
  set_tests_properties(limits_outside PROPERTIES PASS_REGULAR_EXPRESSION "pen \+")
  # Pit road rules (ABI 13): the pitrules robot takes the pit lane every lap. Legal: no penalty. Speeding or crossing
  # the exit line early is owed, and added to the race time unless a stop in the box serves it.
  rr_add_robot(pitrules tests/pitrules/pitrules.c)
  rr_output(pitrules "${RR_OUT}/test_bots")
  add_test(NAME pit_rules_legal COMMAND rr_race --track oval --laps 3 --quiet --car $<TARGET_FILE:pitrules> --params vpit=20)
  set_tests_properties(pit_rules_legal PROPERTIES FAIL_REGULAR_EXPRESSION "pen \\+")
  add_test(NAME pit_rules_speeding COMMAND rr_race --track oval --laps 3 --quiet --car $<TARGET_FILE:pitrules> --params vpit=28)
  set_tests_properties(pit_rules_speeding PROPERTIES PASS_REGULAR_EXPRESSION "pen \\+[0-9]")
  add_test(NAME pit_rules_limiter COMMAND rr_race --track oval --laps 3 --quiet --car $<TARGET_FILE:pitrules> --params vpit=28,limiter=1)
  set_tests_properties(pit_rules_limiter PROPERTIES FAIL_REGULAR_EXPRESSION "pen \\+")
  add_test(NAME pit_rules_exit_line COMMAND rr_race --track oval --laps 3 --quiet --car $<TARGET_FILE:pitrules> --params vpit=20,exit=early)
  set_tests_properties(pit_rules_exit_line PROPERTIES PASS_REGULAR_EXPRESSION "pen \\+[0-9]")
  add_test(NAME pit_rules_served COMMAND rr_race --track oval --laps 3 --quiet --car $<TARGET_FILE:pitrules> --params vpit=28,stop=1)
  set_tests_properties(pit_rules_served PROPERTIES FAIL_REGULAR_EXPRESSION "pen \\+")
  add_test(NAME weekend_memory COMMAND rr_race --track oval --laps 1 --quiet --practice 1 --qualifying
           --car $<TARGET_FILE:memcheck> --params expect=2 --car $<TARGET_FILE:memcheck> --params expect=2)
  add_test(NAME weekend_memory_fresh COMMAND rr_race --track oval --laps 1 --quiet --car $<TARGET_FILE:memcheck> --params expect=0)
  # practice + qualifying + race reach the race with memory from two sessions, not three
  add_test(NAME weekend_memory_wrong COMMAND rr_race --track oval --laps 1 --quiet --practice 1 --qualifying
           --car $<TARGET_FILE:memcheck> --params expect=3)
  set_tests_properties(weekend_memory_wrong PROPERTIES WILL_FAIL TRUE)
  # KERS and DRS (2013 car): the ersdrs robot checks the sensors and prints ERSDRS OK or ERSDRS FAIL.
  rr_add_robot(ersdrs tests/ersdrs/ersdrs.c)
  rr_output(ersdrs "${RR_OUT}/test_bots")
  add_test(NAME ers_drs_practice COMMAND rr_race --track oval --laps 3 --quiet --practice 3
           --car $<TARGET_FILE:ersdrs> --spec f1_2013)
  add_test(NAME ers_drs_race COMMAND rr_race --track oval --laps 4 --quiet --car $<TARGET_FILE:ersdrs> --spec f1_2013)
  add_test(NAME ers_drs_wet COMMAND rr_race --track oval --laps 4 --quiet --wet
           --car $<TARGET_FILE:ersdrs> --spec f1_2013 --params wet=1)
  set_tests_properties(ers_drs_practice ers_drs_race ers_drs_wet PROPERTIES
                       PASS_REGULAR_EXPRESSION "ERSDRS OK" FAIL_REGULAR_EXPRESSION "ERSDRS FAIL")
  # The sandbox: a robot can't write files, and one that crashes, hangs or hogs
  # the CPU only loses its own car.
  rr_add_robot(evil tests/evil/evil.c)
  rr_output(evil "${RR_OUT}/test_bots")
  set(EVIL $<TARGET_FILE:evil>)
  add_test(NAME sandbox_no_files COMMAND rr_race --track oval --laps 1 --quiet --sandbox
           --car ${EVIL} --params write=${CMAKE_BINARY_DIR}/ctest_escaped.txt)
  add_test(NAME sandbox_files_in_process COMMAND rr_race --track oval --laps 1 --quiet
           --car ${EVIL} --params write=${CMAKE_BINARY_DIR}/ctest_escaped_in_process.txt)
  set_tests_properties(sandbox_files_in_process PROPERTIES WILL_FAIL TRUE)  # (no sandbox: it can)
  add_test(NAME sandbox_crash COMMAND rr_race --track oval --laps 1 --sandbox --car simple --car ${EVIL} --params crash=1)
  set_tests_properties(sandbox_crash PROPERTIES PASS_REGULAR_EXPRESSION "simple +[0-9]+:[0-9.]+.*robot crashed")
  add_test(NAME sandbox_hang COMMAND rr_race --track oval --laps 1 --sandbox --car simple --car ${EVIL} --params hang=1)
  set_tests_properties(sandbox_hang PROPERTIES PASS_REGULAR_EXPRESSION "simple +[0-9]+:[0-9.]+.*robot hung")
  add_test(NAME cpu_cap COMMAND rr_race --track oval --laps 1 --cpu-cap 1 --car simple --car ${EVIL} --params cpu=4)
  set_tests_properties(cpu_cap PROPERTIES PASS_REGULAR_EXPRESSION "simple +[0-9]+:[0-9.]+.*over the CPU limit")
endif()

# ---------------------------------------------------------------- install
install(TARGETS rr_race rr_bothost RUNTIME DESTINATION .)
if(RR_BUILD_VIEWER)
  install(TARGETS rr_viewer RUNTIME DESTINATION .)
endif()
if(RR_BUILD_BOTS)
  install(TARGETS ${RR_BOTS} LIBRARY DESTINATION bots RUNTIME DESTINATION bots)
endif()
install(DIRECTORY tracks assets specs DESTINATION .)
