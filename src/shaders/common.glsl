vec4 over(const vec4 a, const vec4 b)
{
    //const vec3 color = a.rgb * a.a + b.rgb * b.a * (1. - a.a);
    const vec3 color = a.rgb + b.rgb * (1. - a.a);
    const float alpha = a.a + b.a * (1. - a.a);
    return vec4(color, alpha);
}

vec2 rotateUV(vec2 uv, float a)
{
    mat2 R = mat2(cos(a), -sin(a), sin(a), cos(a));
    uv -= vec2(0.5, 0.5);
    uv = R * uv;
    uv += vec2(0.5, 0.5);
    return uv;
}
