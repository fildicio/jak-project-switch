# PG.2 shader survey (T45, AI-assisted)

Date: 2026-10-19

Step 1 counts (reviewer-tested): 94 lines, 54 OK / 38 NEEDS-BLOCK / 2 ODD-VERSION.
Step 2: converted every OK shader into /tmp and compiled each with UAM: 40 UAM-OK / 14 UAM-FAIL. Every UAM-FAIL maps 1:1 to a CONVERT-FAIL (converter `conv()` raised an assertion, so no .glsl was emitted and UAM saw an empty file).

No opinions, no plans: raw command output only.

## Survey

```
OK           collision.frag                   missing=-
NEEDS-BLOCK  collision.vert                   missing=camera_position wireframe wireframe_enabled mode version collision_mode_mask collision_event_mask collision_material_mask collision_skip_mask collision_skip_hide_mask collision_skip_nomask_allowed has-uniform-block
ODD-VERSION  debug_red.frag                   missing=-
ODD-VERSION  debug_red.vert                   missing=-
OK           depth_cue.frag                   missing=-
NEEDS-BLOCK  depth_cue.vert                   missing=u_color u_depth
NEEDS-BLOCK  direct2.frag                     missing=alpha_reject color_mult
OK           direct2.vert                     missing=-
NEEDS-BLOCK  direct_basic.frag                missing=scissor_enable game_sizes
OK           direct_basic.vert                missing=-
NEEDS-BLOCK  direct_basic_textured.frag       missing=color_mult alpha_mult alpha_sub ta0 scissor_enable greater game_sizes
NEEDS-BLOCK  direct_basic_textured.vert       missing=offscreen_mode
NEEDS-BLOCK  direct_basic_textured_multi_unit.frag missing=alpha_reject color_mult alpha_mult alpha_sub ta0 scissor_enable game_sizes
NEEDS-BLOCK  direct_basic_textured_multi_unit.vert missing=offscreen_mode
NEEDS-BLOCK  emerc.frag                       missing=ignore_alpha decal_enable
NEEDS-BLOCK  emerc.vert                       missing=fog_constants perspective_matrix fade has-uniform-block
OK           etie.frag                        missing=-
NEEDS-BLOCK  etie.vert                        missing=envmap_tod_tint persp0 persp1 cam_no_persp
OK           etie_base.frag                   missing=-
NEEDS-BLOCK  etie_base.vert                   missing=persp0 persp1 cam_no_persp
OK           eye.frag                         missing=-
OK           eye.vert                         missing=-
NEEDS-BLOCK  generic.frag                     missing=alpha_reject color_mult warp_sample_mode
NEEDS-BLOCK  generic.vert                     missing=mat_32 fog_constants scale mat_23 mat_33 use_full_matrix full_matrix warp_sample_mode
OK           glow_depth_copy.frag             missing=-
OK           glow_depth_copy.vert             missing=-
NEEDS-BLOCK  glow_draw.frag                   missing=glow_boost
OK           glow_draw.vert                   missing=-
OK           glow_probe.frag                  missing=-
OK           glow_probe.vert                  missing=-
OK           glow_probe_downsample.frag       missing=-
OK           glow_probe_downsample.vert       missing=-
OK           glow_probe_on_grid.frag          missing=-
OK           glow_probe_on_grid.vert          missing=-
OK           glow_probe_read.frag             missing=-
OK           glow_probe_read.vert             missing=-
OK           glow_probe_read_debug.frag       missing=-
OK           glow_probe_read_debug.vert       missing=-
OK           hfrag.frag                       missing=-
NEEDS-BLOCK  hfrag.vert                       missing=fog_hack_threshold
OK           hfrag_montage.frag               missing=-
OK           hfrag_montage.vert               missing=-
NEEDS-BLOCK  merc2.frag                       missing=ignore_alpha light_dir0_fade light_dir1_fade_en decal_enable
NEEDS-BLOCK  merc2.vert                       missing=light_dir0_fade light_dir1_fade_en light_dir2 light_col0 light_col1 light_col2 light_ambient fog_constants perspective_matrix has-uniform-block
NEEDS-BLOCK  ocean_common.frag                missing=color_mult alpha_mult bucket
NEEDS-BLOCK  ocean_common.vert                missing=bucket
OK           ocean_envmap.frag                missing=-
OK           ocean_envmap.vert                missing=-
OK           ocean_envmap_haze.frag           missing=-
OK           ocean_envmap_haze.vert           missing=-
OK           ocean_texture.frag               missing=-
OK           ocean_texture.vert               missing=-
NEEDS-BLOCK  ocean_texture_mipmap.frag        missing=alpha_intensity
NEEDS-BLOCK  ocean_texture_mipmap.vert        missing=scale
OK           plain_texture.frag               missing=-
OK           plain_texture.vert               missing=-
NEEDS-BLOCK  post_processing.frag             missing=color_mult color_add
OK           post_processing.vert             missing=-
NEEDS-BLOCK  post_processing_fsr.frag         missing=color_mult color_add con0 inv_input_size
OK           post_processing_fsr.vert         missing=-
NEEDS-BLOCK  shadow.frag                      missing=color_uniform
OK           shadow.vert                      missing=-
NEEDS-BLOCK  shadow2.frag                     missing=color_uniform
NEEDS-BLOCK  shadow2.vert                     missing=clear_mode fog perspective_x perspective_y perspective_z perspective_w
OK           shrub.frag                       missing=-
OK           shrub.vert                       missing=-
NEEDS-BLOCK  simple_texture.frag              missing=color
NEEDS-BLOCK  simple_texture.vert              missing=tex_coord_0 tex_coord_1
OK           sky.frag                         missing=-
OK           sky.vert                         missing=-
OK           sky_blend.frag                   missing=-
OK           sky_blend.vert                   missing=-
OK           slow_time.frag                   missing=-
NEEDS-BLOCK  slow_time.vert                   missing=amount
NEEDS-BLOCK  solid_color.frag                 missing=fragment_color
OK           solid_color.vert                 missing=-
OK           splash.frag                      missing=-
NEEDS-BLOCK  splash.vert                      missing=u_res u_tex
OK           sprite3_3d.frag                  missing=-
NEEDS-BLOCK  sprite3_3d.vert                  missing=hud_matrix hud_hvdf_offset hud_hvdf_user pfog0 min_scale max_scale deg_to_rad inv_area basis_x basis_y xy_array xyz_array st_array
OK           sprite_3d.frag                   missing=-
NEEDS-BLOCK  sprite_3d.vert                   missing=hud_matrix hud_hvdf_offset hud_hvdf_user pfog0 min_scale max_scale bonus deg_to_rad inv_area basis_x basis_y hmge_scale xy_array xyz_array st_array
OK           sprite_distort.frag              missing=-
NEEDS-BLOCK  sprite_distort.vert              missing=u_color
OK           sprite_distort_instanced.frag    missing=-
NEEDS-BLOCK  sprite_distort_instanced.vert    missing=u_color
NEEDS-BLOCK  tex_anim.frag                    missing=rgba enable_tex set_alpha tcc channel_scramble alpha_multiply minimum maximum slime_scroll
NEEDS-BLOCK  tex_anim.vert                    missing=positions uvs rgba
OK           tfrag3.frag                      missing=-
OK           tfrag3.vert                      missing=-
OK           tfrag3_no_tex.frag               missing=-
OK           tfrag3_no_tex.vert               missing=-
OK           tie_wind.frag                    missing=-
OK           tie_wind.vert                    missing=-
```

## UAM

```
UAM-OK collision.frag
UAM-OK depth_cue.frag
UAM-FAIL direct2.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-FAIL direct_basic.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK etie_base.frag
UAM-OK etie.frag
UAM-OK eye.frag
UAM-FAIL eye.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK glow_depth_copy.frag
UAM-FAIL glow_depth_copy.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK glow_draw.vert
UAM-OK glow_probe_downsample.frag
UAM-OK glow_probe_downsample.vert
UAM-OK glow_probe.frag
UAM-OK glow_probe_on_grid.frag
UAM-OK glow_probe_on_grid.vert
UAM-OK glow_probe_read_debug.frag
UAM-FAIL glow_probe_read_debug.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK glow_probe_read.frag
UAM-FAIL glow_probe_read.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK glow_probe.vert
UAM-OK hfrag.frag
UAM-FAIL hfrag_montage.frag: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK hfrag_montage.vert
UAM-FAIL ocean_envmap.frag: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK ocean_envmap_haze.frag
UAM-OK ocean_envmap_haze.vert
UAM-FAIL ocean_envmap.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK ocean_texture.frag
UAM-OK ocean_texture.vert
UAM-OK plain_texture.frag
UAM-OK plain_texture.vert
UAM-OK post_processing_fsr.vert
UAM-OK post_processing.vert
UAM-FAIL shadow.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK shrub.frag
UAM-OK shrub.vert
UAM-OK sky_blend.frag
UAM-OK sky_blend.vert
UAM-OK sky.frag
UAM-FAIL sky.vert: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK slow_time.frag
UAM-OK solid_color.vert
UAM-FAIL splash.frag: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK sprite3_3d.frag
UAM-OK sprite_3d.frag
UAM-FAIL sprite_distort.frag: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-FAIL sprite_distort_instanced.frag: Shader failed to compile. 0:1(1): error: syntax error, unexpected $end  
UAM-OK tfrag3.frag
UAM-OK tfrag3_no_tex.frag
UAM-OK tfrag3_no_tex.vert
UAM-OK tfrag3.vert
UAM-OK tie_wind.frag
UAM-OK tie_wind.vert
```

## Missing uniforms

Distinct non-sampler uniform names in the `missing=` column, one per line with the number of shaders needing it (counts from the survey).

```
   7 color_mult
   3 u_color
   3 scissor_enable
   3 game_sizes
   3 fog_constants
   3 alpha_reject
   3 alpha_mult
   2 xyz_array
   2 xy_array
   2 warp_sample_mode
   2 ta0
   2 st_array
   2 scale
   2 rgba
   2 pfog0
   2 perspective_matrix
   2 persp1
   2 persp0
   2 offscreen_mode
   2 min_scale
   2 max_scale
   2 light_dir1_fade_en
   2 light_dir0_fade
   2 inv_area
   2 ignore_alpha
   2 hud_matrix
   2 hud_hvdf_user
   2 hud_hvdf_offset
   2 deg_to_rad
   2 decal_enable
   2 color_uniform
   2 color_add
   2 cam_no_persp
   2 bucket
   2 basis_y
   2 basis_x
   2 alpha_sub
   1 wireframe_enabled
   1 wireframe
   1 version
   1 uvs
   1 use_full_matrix
   1 u_tex
   1 u_res
   1 u_depth
   1 tex_coord_1
   1 tex_coord_0
   1 tcc
   1 slime_scroll
   1 set_alpha
   1 positions
   1 perspective_z
   1 perspective_y
   1 perspective_x
   1 perspective_w
   1 mode
   1 minimum
   1 maximum
   1 mat_33
   1 mat_32
   1 mat_23
   1 light_dir2
   1 light_col2
   1 light_col1
   1 light_col0
   1 light_ambient
   1 inv_input_size
   1 hmge_scale
   1 greater
   1 glow_boost
   1 full_matrix
   1 fragment_color
   1 fog_hack_threshold
   1 fog
   1 fade
   1 envmap_tod_tint
   1 enable_tex
   1 con0
   1 color
   1 collision_skip_nomask_allowed
   1 collision_skip_mask
   1 collision_skip_hide_mask
   1 collision_mode_mask
   1 collision_material_mask
   1 collision_event_mask
   1 clear_mode
   1 channel_scramble
   1 camera_position
   1 bonus
   1 amount
   1 alpha_multiply
   1 alpha_intensity
```

## Reviewer correction (2026-10-19, AI-assisted)

The UAM-FAIL lines above ("syntax error, unexpected $end" at 0:1) were a bug in
`scripts/ai/pg2-convert-shader.py`, not in the shaders: the script asserted on
`SCISSOR_*`/`HEIGHT_SCALE` forms and on a missing blank line after `#version`,
AFTER it had already created an empty output file. Fixed (token swaps now match
gen-dksh.py; output written only on success). Re-run: **every OK shader now
compiles with UAM** (9 + 5 previously failing → UAM-OK). The 8 shaders already in
`deko3d_renderer/shaders/` still reproduce byte-for-byte.
