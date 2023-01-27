struct ps_input_t {
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

float4 main(ps_input_t input) : SV_TARGET
{
	return input.color;
}
