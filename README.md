# dx12_experiments
DirectX12 physically based deferred renderer implemented from scratch with the goal of using it as a framework for future research and experimentation in computer graphics. 
# Features list
* Deferred renderer based on DirectX12.
  * Descriptor heap staging system to allow for quick binding of HLSL descriptors.
  * Necessary graphics and compute queue synchronization to allow triple frame buffering support.
* Cook-Torrance specular BRDF for direct illumination.
* Specular and diffuse IBL for indirect illumination.
  * Mipmap generation using simple linear interpolation.
  * Specular BRDF look-up table pre-calculation.
  * Diffuse irradiance pre-calculation using HDR environment map convolution.
  * Specular irradiance pre-calculation using GGX NDF importance sampling.
* Blinn-Phong implementation used to contrast with other shading techniques.
* GPU driven particle systems.
  * Takes advantage of DirectX12's indirect execution to frustum cull, simulate and render on the GPU.
  * Geometry shader point expansion into billboards.
  * Implementation of depth-fading for better visual result when billboards intersect other geometry.
* Multithreaded spot lights shadow maps.
* Volumetric halos (point lights).
* Point lights and point shadows generated in a geometry shader.
* Basic post processing including reinhard tonemapping and gamma correction.
* C++ code hot reloading.
* Basic CPU and GPU profiler.
* Basic arena allocator.





