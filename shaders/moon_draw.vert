varying vec2 tc;

void main()
{
	tc            = gl_MultiTexCoord0;
	gl_Position   = ftransform();
	vec3 normal   = normalize(gl_NormalMatrix * gl_Normal); // eye space
	gl_FrontColor = add_light_comp(normal, 4); // only light 4
}
