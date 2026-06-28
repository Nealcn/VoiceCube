# The top-level CMakeLists.txt for the M5AtomS3 Voice Stick firmware
cmake_minimum_required(VERSION 3.16)

# Let the build system discover ALL components automatically,
# including managed_components/ from the component manager.
# (Explicit COMPONENTS list would exclude managed components.)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(atoms3_voicestick)
