void fireRay(mat4 viewInv, mat4 projInv, vec2 st /*screen space ray origin*/)
{
    //vec4 origin = viewInv * projInv * vec4(st.x, st.y, 0, 1);
    vec4 origin = viewInv * vec4(0, 0, 0, 1);
    //vec4 target = projInv * vec4(st.x, st.y, 0, 1);
    vec4 target = vec4(projInv[0][0] * st.x, -st.y, -1, 0);
    vec4 dir    = viewInv * vec4(normalize(target).xyz, 0);
    //vec4 dir    = viewInv * vec4(st.x, st.y, 100, 1);
    dir = normalize(dir);

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
