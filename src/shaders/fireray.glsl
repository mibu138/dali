void fireRay(mat4 viewInv, mat4 projInv, vec2 st /*screen space ray origin*/, vec2 bpos /*brush pos*/)
{
    // for the origin we do the opposite of direction and zero out all but w 
    // so that we only extract the translation from the camera. (viewInv == cam.xform)
    vec4 origin = viewInv * vec4(0, 0, 0, 1);
    // see my notes on projection matrices. basically this is a short hand 
    // matrix multiple of projInv to the brush position. 
    // we only want to apply it to the brush offset in screen space 
    // to avoid squashing or stretching the shape of the brush when 
    // the projection matrix is 'non-uniform' (ie when the viewport width and 
    // height are not equal).
    st = st + vec2(projInv[0][0] * bpos.x, projInv[1][1] * bpos.y);
    // still a bit curious about the -1 in z but it works.
    // the 0 in w will kill off any translation in the camera, which we 
    // want since this is a direction. we normalize so that the ray 
    // length is controlled only by tMin and tMax.
    vec4 target = normalize(vec4(st.x, st.y, -1, 0));
    vec4 dir    = viewInv * target;

    uint  rayFlags = gl_RayFlagsOpaqueEXT;
    float tMin     = 0.001;
    float tMax     = 10000.0;

    traceRayEXT(topLevelAS, // acceleration structure
            rayFlags,       // rayFlags
            0xFF,           // cullMask
            0,              // sbtRecordOffset
            0,              // sbtRecordStride
            0,              // missIndex
            origin.xyz,     // ray origin
            tMin,           // ray min range
            dir.xyz,        // ray direction
            tMax,           // ray max range
            0               // payload (location = 0)
    );
}
