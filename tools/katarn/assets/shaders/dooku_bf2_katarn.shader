// Count Dooku, Battlefront II model: normal + roughness/metal/AO maps, alpha-tested hair and beard cards.

// Works in rend2 and KatarnEngine. Remove this pk3 to get the model's plain textures back.



models/players/dooku_bf2/material_002
{
	{
		map models/players/dooku_bf2/material_002
		normalMap models/players/dooku_bf2/body_n
		rmoMap models/players/dooku_bf2/body_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/material_001
{
	{
		map models/players/dooku_bf2/material_001
		normalMap models/players/dooku_bf2/hands_n
		rmoMap models/players/dooku_bf2/hands_rmo
		skin 1.0 0.45 0.3 0.5
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/s_heads_countdooku_01_mesh
{
	{
		map models/players/dooku_bf2/s_heads_countdooku_01_mesh
		normalMap models/players/dooku_bf2/head_n
		rmoMap models/players/dooku_bf2/head_rmo
		skin 1.0 0.45 0.3 0.5
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/cape
{
	cull disable
	{
		map models/players/dooku_bf2/cape
		normalMap models/players/dooku_bf2/cape_n
		rmoMap models/players/dooku_bf2/cape_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/s_heads_countdooku_01_haircap_mesh
{
	{
		map models/players/dooku_bf2/s_heads_countdooku_01_haircap_mesh
		alphaFunc GE128
		depthWrite
		normalMap models/players/dooku_bf2/haircap_n
		rmoMap models/players/dooku_bf2/haircap_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/s_heads_countdooku_01_haircards_mesh
{
	cull disable
	{
		map models/players/dooku_bf2/s_heads_countdooku_01_haircards_mesh
		alphaFunc GE128
		depthWrite
		normalMap models/players/dooku_bf2/haircards_n
		rmoMap models/players/dooku_bf2/hair_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/s_heads_countdooku_01_beardcap_mesh
{
	{
		map models/players/dooku_bf2/s_heads_countdooku_01_beardcap_mesh
		alphaFunc GE128
		depthWrite
		normalMap models/players/dooku_bf2/beardcap_n
		rmoMap models/players/dooku_bf2/beard_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/s_heads_countdooku_01_beardcards_mesh
{
	cull disable
	{
		map models/players/dooku_bf2/s_heads_countdooku_01_beardcards_mesh
		alphaFunc GE128
		depthWrite
		normalMap models/players/dooku_bf2/beardcards_n
		rmoMap models/players/dooku_bf2/beard_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/eyelashes_replace
{
	cull disable
	{
		map models/players/dooku_bf2/eyelashes_replace
		alphaFunc GE128
		depthWrite
		rmoMap models/players/dooku_bf2/lash_rmo
		rgbGen lightingDiffuse
	}
}

models/players/dooku_bf2/eyes_replace
{
	{
		map models/players/dooku_bf2/eyes_replace
		rmoMap models/players/dooku_bf2/eyes_rmo
		rgbGen lightingDiffuse
	}
}
