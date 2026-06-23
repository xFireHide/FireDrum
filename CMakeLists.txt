cmake_minimum_required(VERSION 3.22)

project(FireKit VERSION 1.0.0 LANGUAGES C CXX)

# C++20 para concepts/atomics modernos; sem extensões de compilador.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- JUCE -------------------------------------------------------------------
# Opção A (recomendada): tenha o JUCE como submódulo em ./JUCE
#   git submodule add https://github.com/juce-framework/JUCE
# Opção B: aponte JUCE_DIR para uma instalação existente.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# --- App standalone ---------------------------------------------------------
juce_add_gui_app(FireKit
    PRODUCT_NAME "FireKit"
    COMPANY_NAME "SomBateria")

target_sources(FireKit PRIVATE
    Source/Main.cpp
    Source/SerialSource.cpp)

# Headers-only (template-heavy) ficam visíveis pela pasta Source.
target_include_directories(FireKit PRIVATE Source)

target_compile_definitions(FireKit PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:FireKit,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:FireKit,JUCE_VERSION>")

target_link_libraries(FireKit PRIVATE
    juce::juce_audio_utils      # devices + formats + basics + gui
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_gui_basics
    juce::juce_core
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
