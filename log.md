[build] FAILED: [code=2] C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/indirect_lighting.rgen.pipeline.spv 
[build] C:\WINDOWS\system32\cmd.exe /C "cd /D C:\Users\pearu\Q2RTX\build\src && "C:\Program Files\CMake\bin\cmake.exe" -E make_directory C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt && C:\VulkanSDK\1.4.328.1\Bin\glslang.exe --target-env vulkan1.2 --quiet -DVKPT_SHADER -V C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/indirect_lighting.rgen -o C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/indirect_lighting.rgen.pipeline.spv"
[build] ERROR: C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/restir_gi.glsl:378: '' :  syntax error, unexpected INTCONSTANT, expecting COMMA or SEMICOLON
[build] ERROR: 1 compilation errors.  No code generated.
[build] 
[build] 
[build] ERROR: Linking ray-generation stage: Missing entry point: Each stage requires one entry point
[build] 
[build] SPIR-V is not generated for failed compile or link
[build] [1041/1386  72% :: 20.861] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/instance_geometry.comp.spv
[build] [1041/1386  72% :: 20.861] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\uhjfilter.cpp.obj
[build] [1041/1386  72% :: 20.861] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/normalize_normal_map.comp.spv
[build] [1041/1386  73% :: 20.862] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/indirect_lighting.rgen.query.spv
[build] FAILED: [code=2] C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/indirect_lighting.rgen.query.spv 
[build] C:\WINDOWS\system32\cmd.exe /C "cd /D C:\Users\pearu\Q2RTX\build\src && "C:\Program Files\CMake\bin\cmake.exe" -E make_directory C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt && C:\VulkanSDK\1.4.328.1\Bin\glslang.exe -S comp --target-env vulkan1.2 --quiet -DVKPT_SHADER -V -DKHR_RAY_QUERY C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/indirect_lighting.rgen -o C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/indirect_lighting.rgen.query.spv"
[build] ERROR: C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/restir_gi.glsl:378: '' :  syntax error, unexpected INTCONSTANT, expecting COMMA or SEMICOLON
[build] ERROR: 1 compilation errors.  No code generated.
[build] 
[build] 
[build] ERROR: Linking compute stage: Missing entry point: Each stage requires one entry point
[build] 
[build] SPIR-V is not generated for failed compile or link
[build] [1041/1386  73% :: 20.862] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer.rmiss.pipeline.spv
[build] [1041/1386  73% :: 20.862] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer.rchit.pipeline.spv
[build] [1041/1386  73% :: 20.862] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_beam.rahit.pipeline.spv
[build] [1041/1386  73% :: 20.868] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_beam.rint.pipeline.spv
[build] [1041/1386  73% :: 20.896] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_explosion.rahit.pipeline.spv
[build] [1041/1386  73% :: 20.918] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_masked.rahit.pipeline.spv
[build] [1041/1386  73% :: 20.939] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_particle.rahit.pipeline.spv
[build] [1041/1386  73% :: 20.953] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/path_tracer_sprite.rahit.pipeline.spv
[build] [1041/1386  73% :: 21.015] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/physical_sky.comp.spv
[build] [1041/1386  73% :: 21.028] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\alc\alc.cpp.obj
[build] [1041/1386  73% :: 21.028] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/physical_sky_space.comp.spv
[build] [1041/1386  73% :: 21.079] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\logging.cpp.obj
[build] [1041/1386  73% :: 21.098] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/primary_rays.rgen.pipeline.spv
[build] [1041/1386  74% :: 21.117] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\mixer\mixer_sse41.cpp.obj
[build] [1041/1386  74% :: 21.117] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/restir_gi_apply.comp.spv
[build] FAILED: [code=2] C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/restir_gi_apply.comp.spv 
[build] C:\WINDOWS\system32\cmd.exe /C "cd /D C:\Users\pearu\Q2RTX\build\src && "C:\Program Files\CMake\bin\cmake.exe" -E make_directory C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt && C:\VulkanSDK\1.4.328.1\Bin\glslang.exe --target-env vulkan1.2 --quiet -DVKPT_SHADER -V -IC:/Users/pearu/Q2RTX/src/refresh/vkpt/fsr C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/restir_gi_apply.comp -o C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/restir_gi_apply.comp.spv"
[build] ERROR: C:/Users/pearu/Q2RTX/src/refresh/vkpt/shader/restir_gi.glsl:378: '' :  syntax error, unexpected INTCONSTANT, expecting COMMA or SEMICOLON
[build] ERROR: 1 compilation errors.  No code generated.
[build] 
[build] 
[build] ERROR: Linking compute stage: Missing entry point: Each stage requires one entry point
[build] 
[build] SPIR-V is not generated for failed compile or link
[build] [1041/1386  74% :: 21.127] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/reflect_refract.rgen.pipeline.spv
[build] [1041/1386  74% :: 21.129] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/primary_rays.rgen.query.spv
[build] [1041/1386  74% :: 21.153] Generating C:/Users/pearu/Q2RTX/game/baseq2/shader_vkpt/reflect_refract.rgen.query.spv
[build] [1041/1386  74% :: 21.454] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\alc\backends\wasapi.cpp.obj
[build] [1041/1386  74% :: 21.464] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\Reference.cpp.obj
[build] [1041/1386  74% :: 21.691] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\voice.cpp.obj
[build] [1041/1386  74% :: 21.716] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\Sigma.cpp.obj
[build] [1041/1386  74% :: 21.752] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\helpers.cpp.obj
[build] [1041/1386  74% :: 21.819] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\Wrapper.cpp.obj
[build] [1041/1386  74% :: 21.987] Building CXX object extern\openal-soft\CMakeFiles\OpenAL.dir\core\hrtf.cpp.obj
[build] [1041/1386  74% :: 22.262] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\InstanceImpl.cpp.obj
[build] [1041/1386  74% :: 22.758] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\Relax.cpp.obj
[build] [1041/1386  75% :: 23.128] Building CXX object extern\RTX-Kit\NRD\CMakeFiles\NRD.dir\Source\Reblur.cpp.obj
[build] [1041/1386  75% :: 24.303] Copying pak0.pak; Copying pak1.pak; Copying pak2.pak; Copying blue_noise.pkz; Copying q2rtx_media.pkz; Copying shaders.pkz; Copying q2rtx.cfg; Copying pt_toggles.cfg; Copying prefetch.txt; Copying maps.lst; Copying q2rtx.menu; Copying maps/; Copying materials/; Copying pics/; Copying players/; Copying video/; Copying nvngx_dlss.dll; Copying nvngx_dlssd.dll; Copying vulkan-1.lib
[build] ninja: build stopped: subcommand failed.
[proc] The command: "C:\Program Files\CMake\bin\cmake.EXE" --build c:/Users/pearu/Q2RTX/build --config RelWithDebInfo --target all -- exited with code: 2
[driver] Build completed: 00:00:24.356
[build] Build finished with exit code 2
