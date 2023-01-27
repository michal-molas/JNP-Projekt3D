struct vs_output_t {
	float4 position : SV_POSITION;
	float4 color : COLOR;
};

vs_output_t main(float3 pos : POSITION, float4 col : COLOR) {
	vs_output_t result;
	result.position = float4(pos.x, pos.y, pos.z, 1.0f);
	result.color = col;
	return result;
}