set(SRCS
    applyPaint.frag
    comp2a.frag
    comp2.frag
    comp3a.frag
    comp4a.frag
    comp.frag
    layerStack.frag
    paint.rchit
    paint.rgen
    paint.rmiss
    paint-vec2.rchit
    post.frag
    post.vert
    raster.frag
    raster.vert
    select-float.rchit
    select.rchit
    select.rgen
    select.rmiss)

set(GLC glslc)
set(GLCFLAGS "--target-env=vulkan1.2")
set(DIR ${CMAKE_CURRENT_SOURCE_DIR})

foreach(SRC ${SRCS})
    get_filename_component(FILE_NAME ${SRC} NAME)
    set(SPV "${PROJECT_BINARY_DIR}/src/shaders/${FILE_NAME}.spv")
    set(CMD ${GLC} ${GLCFLAGS} ${DIR}/${SRC} -o ${SPV})
    add_custom_command(
        OUTPUT ${SPV}
        COMMAND ${CMD}
        DEPENDS ${SRC})
    list(APPEND SPVS ${SPV})
endforeach(SRC)

add_custom_target(painter-shaders DEPENDS ${SPVS})

install(FILES ${SPVS}
    DESTINATION ${SHADER_INSTALL_PREFIX}${LIBRARY_NAME})
