if vulkan.found()

vk_deps = [volk, libglslang, vma, vulkan, spirv_reflect, gloffscreen]
if not get_option('libretro')
  vk_deps += [sdl]
endif

specific_ss.add([vk_deps,
	files(
		'blit.c',
		'buffer.c',
		'command.c',
		'debug.c',
		'display.c',
		'draw.c',
		'glsl.c',
		'gpuprops.c',
		'image.c',
		'instance.c',
		'renderer.c',
		'reports.c',
		'shaders.c',
		'surface-compute.c',
		'surface.c',
		'texture.c',
		'vertex.c',
		)
	])

endif
