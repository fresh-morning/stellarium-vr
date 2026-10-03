# The session binds to the app's GLX context, so it needs X11 and GLX.
IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
     MESSAGE(FATAL_ERROR "The OpenXR plugin supports Linux (GLX) only")
ENDIF()

FIND_PACKAGE(OpenXR REQUIRED)
FIND_PACKAGE(OpenGL REQUIRED COMPONENTS GLX)
FIND_PACKAGE(X11 REQUIRED)

ADD_LIBRARY(OpenXR-static STATIC OpenXR.hpp OpenXR.cpp OpenXRMenu.hpp OpenXRMenu.cpp)
TARGET_INCLUDE_DIRECTORIES(OpenXR-static PRIVATE ${CMAKE_BINARY_DIR}/plugins/OpenXR/src)
TARGET_LINK_LIBRARIES(OpenXR-static Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::OpenGL
     OpenXR::openxr_loader OpenGL::GLX X11::X11)
SET_TARGET_PROPERTIES(OpenXR-static PROPERTIES OUTPUT_NAME "OpenXR")
# OpenXR structs start as {XR_TYPE_...}, the rest zeroed on purpose.
SET_TARGET_PROPERTIES(OpenXR-static PROPERTIES COMPILE_FLAGS "-DQT_STATICPLUGIN -Wno-missing-field-initializers")
ADD_DEPENDENCIES(AllStaticPlugins OpenXR-static)
SET_TARGET_PROPERTIES(OpenXR-static PROPERTIES FOLDER "plugins/OpenXR")
