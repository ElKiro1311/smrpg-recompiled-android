Snapdragon Game Super Resolution (SGSR), by Qualcomm
Open source under the 3-clause BSD license (LICENSE.txt). Origin: https://github.com/SnapdragonStudios/snapdragon-gsr

What the port uses (its shaders, adapted to the runtime's conventions and written inside the code):
  - SGSR 1 (sgsr/v1/include/glsl/sgsr1_shader_mobile.frag): the menu's "Upscaling: Snapdragon GSR 1",
    runtime/nvn/vulkan/shaders.c ("sgsr1.frag") and exec.c post_process. RGBA mode, edge threshold 8/255, sharpness 2.
  - SGSR 2 (its conversion and upscaling shaders, sgsr2_convert and sgsr2_upscale): the menu's "Anti-aliasing: Snapdragon
    GSR 2", runtime/nvn/vulkan/shaders.c and exec.c (gsr2_dispatch).
