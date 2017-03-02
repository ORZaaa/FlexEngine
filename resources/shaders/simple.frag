#version 400

//uniform sampler2D texTest;
uniform float in_Time;

in vec3 ex_Color;

out vec4 fragmentColor;

void main() {
	fragmentColor = vec4(ex_Color, 1.0);
	fragmentColor.r += in_Time;
	fragmentColor.r = fract(fragmentColor.r * 5) + fract((fragmentColor.g) * 3);
}

