/*
 * user/lib/ftmodule_min.h
 * -----------------------------------------------------------------------------
 * FreeType 精简模块注册表（SukiOS 仅用 TrueType 渲染，去除 Type1/CFF/PCF/BDF/
 * SVG/SDF 等无关驱动以降低编译体积并避免链接无关符号）。
 *
 * 编译 FreeType 时通过
 *   -DFT_CONFIG_MODULE_H="\"user/lib/ftmodule_min.h\""
 * 覆盖默认 ftmodule.h。仅注册 TTF 渲染所需的类。
 */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
