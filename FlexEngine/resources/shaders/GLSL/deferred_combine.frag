#version 400

struct DirectionalLight 
{
	vec3 direction;

	vec3 ambientCol;
	vec3 diffuseCol;
	vec3 specularCol;
};
uniform DirectionalLight dirLight;

struct PointLight 
{
	vec3 position;

	float constant;
	float linear;
	float quadratic;

	vec3 ambientCol;
	vec3 diffuseCol;
	vec3 specularCol;
};
#define NUMBER_POINT_LIGHTS 4
uniform PointLight pointLights[NUMBER_POINT_LIGHTS];

uniform sampler2D in_PositionSampler;
uniform sampler2D in_NormalSampler;
uniform sampler2D in_DiffuseSpecularSampler;

uniform vec4 in_CamPos;

in vec2 ex_TexCoord;

out vec4 fragmentColor;

vec3 DoDirectionalLighting(DirectionalLight dirLight, vec3 diffuseSample, float specularSample, vec3 normal, vec3 viewDir, float specStrength, float specShininess)
{
	if (dirLight.direction == vec3(0, 0, 0)) return vec3(0, 0, 0);

	vec3 lightDir = normalize(-dirLight.direction);

	float diffuseIntensity = max(dot(normal, lightDir), 0.0);

	vec3 reflectDir = reflect(-lightDir, normal);
	float specularIntensity = pow(max(dot(viewDir, reflectDir), 0.0), specShininess);
	
	float specularCol = specStrength * specularIntensity * specularSample;

	vec3 ambient = dirLight.ambientCol * diffuseSample;
	vec3 diffuse = dirLight.diffuseCol * diffuseSample * diffuseIntensity;
	vec3 specular = dirLight.specularCol * specularCol * specularIntensity;

	return (ambient + diffuse + specular);
}

vec3 DoPointLighting(PointLight pointLight, vec3 diffuseSample, float specularSample, vec3 normal, vec3 worldPos, vec3 viewDir, float specStrength, float specShininess)
{
	if (pointLight.constant == 0.0f) return vec3(0, 0, 0);

	vec3 lightDir = normalize(pointLight.position - worldPos);

	float diffuseIntensity = max(dot(normal, lightDir), 0.0);

	vec3 reflectDir = reflect(-lightDir, normal);
	float specularIntensity = pow(max(dot(viewDir, reflectDir), 0.0), specShininess);

	float specularCol = specStrength * specularIntensity * specularSample;

	float distance = length(pointLight.position - worldPos);
	float attenuation = 1.0 / (pointLight.constant + pointLight.linear * distance + pointLight.quadratic * (distance * distance)); 

	vec3 ambient = pointLight.ambientCol * diffuseSample * attenuation;
	vec3 diffuse = pointLight.diffuseCol * diffuseSample * diffuseIntensity * attenuation;
	vec3 specular = pointLight.specularCol * specularCol * specularIntensity * attenuation;

	return (ambient + diffuse + specular);
}

void main()
{
    // retrieve data from gbuffer
    vec3 worldPos = texture(in_PositionSampler, ex_TexCoord).rgb;
    vec3 normal = texture(in_NormalSampler, ex_TexCoord).rgb;
    vec3 diffuse = texture(in_DiffuseSpecularSampler, ex_TexCoord).rgb;
    float specular = texture(in_DiffuseSpecularSampler, ex_TexCoord).a;

	float specStrength = 0.5;
	float specShininess = 32.0;
	
	vec3 viewDir = normalize(in_CamPos.xyz - worldPos);

	vec3 result = DoDirectionalLighting(dirLight, diffuse, specular, normal, viewDir, specStrength, specShininess);

	for (int i = 0; i < NUMBER_POINT_LIGHTS; ++i)
	{
		result += DoPointLighting(pointLights[i], diffuse, specular, normal, worldPos, viewDir, specStrength, specShininess);
	}
	
	fragmentColor = vec4(result, 1.0);
	
	// visualize diffuse lighting:
	//fragmentColor = vec4(vec3(lightIntensity), 1); return;
	
	// visualize normals:
	//fragmentColor = vec4(normal * 0.5 + 0.5, 1); return;
	
	// visualize specular:
	//fragmentColor = specular; return;

	// visualize tex coords:
	//fragmentColor = vec4(ex_TexCoord.xy, 0, 1); return;
	
	// no lighting:
	//fragmentColor = ex_Color; return;
}
