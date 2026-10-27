# port/3ds/hudbake_recipe_id.cmake -- the HUD bake's recipe id (gdx3ds_hudbake.c's recipeTag).
#
# One hash over the recipe sources (gdx3ds_hudbake.c + gdx3ds_hudbake.h, CRLF folded to LF so a
# Windows checkout hashes like a Unix one). The console build and the PC pack tool
# (tools/hudpack) both include this file, so a hudbake.cache written on the PC is accepted by
# exactly the console builds whose recipes produce the same pixels, and refused (then rebaked) by
# any other. Editing either source re-runs the configure step (CMAKE_CONFIGURE_DEPENDS).
#
#   gdx3ds_hudbake_recipe_id(<out-var> <dir holding gdx3ds_hudbake.c/.h>)
#   -> <out-var> = 0x<8 hex digits>u
function(gdx3ds_hudbake_recipe_id out_var src_dir)
    set(text "")
    foreach(name gdx3ds_hudbake.c gdx3ds_hudbake.h)
        set(path "${src_dir}/${name}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" body)
        string(REPLACE "\r\n" "\n" body "${body}")
        string(APPEND text "${name}\n${body}")
    endforeach()
    string(SHA256 digest "${text}")
    string(SUBSTRING "${digest}" 0 8 id)
    set(${out_var} "0x${id}u" PARENT_SCOPE)
endfunction()
