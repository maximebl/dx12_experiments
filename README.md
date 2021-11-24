# dx12_experiments
DirectX12 physically based deferred renderer implemented from scratch with the goal of using it as a framework for future research and experimentation in computer graphics. 
# Features list
## Deferred renderer based on DirectX12
![](https://dsm01pap001files.storage.live.com/y4mXqWUmkYoqhxf0fbGyX-90XdhzWkpfssPL25l0tmUIEVPHaOxGpt-WmOpcm0y3aeODCsD5hJJB4Fcc9slVpcbhdn8ObcBkHDh50exgz94r0Um97ZGFXHeLP0cqMUwbfg2opBeDKif-q_pHLMdHY2MNHLYUoZ2hzTEYQfKLmNk_6HCDUC0ZTBkdQw0ViYrFg6x?width=2560&height=848&cropmode=none)
* Descriptor heap staging system to allow for quick binding of HLSL descriptors.
* Necessary graphics and compute queue synchronization to allow triple frame buffering support.
* Cook-Torrance specular BRDF for direct illumination.
* Blinn-Phong implementation used to contrast with other shading techniques.
* Volumetric halos (point lights).
* Multithreaded spot lights shadow maps.
* Point lights and point shadows generated in a geometry shader.
* Basic post processing including reinhard tonemapping and gamma correction.
* C++ code hot reloading.
* Basic CPU and GPU profiler.
* Basic arena allocator.

## Image-based lighting
![](https://dsm01pap001files.storage.live.com/y4mSihl67ZrcMjM1HjHbPqUg6CHTyi1FC-Hgeb2i0SAC-s3qvqyUw3ZBS-VBqYrnCYRAfaA8qepMWPl-Ks_NE2MKvipxBNoqWf6U3_2gLrODES67BDltWiD4e0H5ZSA4oxzhAiD-bagG1ildmKq9PDLvx6Ih7NclVO2uKYDn53ahdrDBgjx5ChzcRd8Rzr6huMK?width=2560&height=644&cropmode=none)
* Mipmap generation using simple linear interpolation.
* Specular BRDF look-up table pre-calculation.
* Diffuse irradiance pre-calculation using HDR environment map convolution.
* Specular irradiance pre-calculation using GGX NDF importance sampling.

## GPU driven particle systems
![](https://dsm01pap001files.storage.live.com/y4m14Co-8akQAgZYJUx4gB8JivXTDSXEQYdzkxvppMfHYg_NhVxsCh-D_HrJ9SpxheUhweR7fQG5tt9sZr_Lc6Njr0eupWN0F8tyGu9w9sVlMNy5UJeMS1TcdYlOzvuHlQblMGPiWxFAo1Ju3Cgz1LmZIuT6hsZ_OEYv6swdNsZPrn3Uvnha5zLj9Oxp-bQO1zB?width=2032&height=966&cropmode=none)
* Takes advantage of DirectX12's indirect execution to frustum cull, simulate and render on the GPU.
* Geometry shader point expansion into billboards.
* Implementation of depth-fading for better visual result when billboards intersect other geometry.





