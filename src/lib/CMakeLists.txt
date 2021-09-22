set(SRCS 
    layer.c
    engine.c 
    brush.c
    undo.c
    dali.c)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(daliObj OBJECT)
add_library(dali    SHARED)
target_sources(daliObj PRIVATE ${SRCS})
target_include_directories(daliObj 
    PRIVATE "../include/dali" 
    INTERFACE "../include")
target_link_libraries(daliObj PUBLIC Obsidian::ObsidianObj)
target_compile_definitions(daliObj PUBLIC COAL_SIMPLE_TYPE_NAMES)
target_link_libraries(dali PUBLIC daliObj Obsidian::Obsidian)
add_library(Dali::Dali ALIAS dali)
add_library(Dali::DaliObj ALIAS daliObj)

if(WIN32)
	set_target_properties(dali PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE 
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
