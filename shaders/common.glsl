vec3 over(const vec4 a, const vec4 b)
{
    return a.rgb * a.a + b.rgb * b.a * (1. - a.a);
}
