vec4 over(const vec4 a, const vec4 b)
{
    //const vec3 color = a.rgb * a.a + b.rgb * b.a * (1. - a.a);
    const vec3 color = a.rgb + b.rgb * (1. - a.a);
    const float alpha = a.a + b.a * (1. - a.a);
    return vec4(color, alpha);
}
