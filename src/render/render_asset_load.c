/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "public/render.h"
#include "render_private.h"
#include "vertex.h"
#include "material.h"
#include "render_gl.h"

#include "../asset_load.h"
#include "../map/public/tile.h"

#include <assert.h>
#include <ctype.h>
#define __USE_POSIX
#include <string.h>

#define STREVAL(a) STR(a)
#define STR(a) #a

#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool al_read_vertex(SDL_RWops *stream, struct vertex *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "v %f %f %f", &out->pos.x, &out->pos.y, &out->pos.z))
        goto fail;

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "vt %f %f", &out->uv.x, &out->uv.y))
        goto fail;

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "vn %f %f %f", &out->normal.x, &out->normal.y, &out->normal.z))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!strstr(line, "vw"))
        goto fail;

    char *string = line;
    char *saveptr;
    int i;

    /* Write 0.0 weights by default */
    memset(out->weights, 0, sizeof(out->weights));
    memset(out->joint_indices, 0, sizeof(out->joint_indices));

    /* Consume the first token, the attribute name 'vw' */
    string = strtok_r(line, " \t", &saveptr);
    for(i = 0; i < 6; i++) {

        string = strtok_r(NULL, " \t", &saveptr);
        if(!string)
            break;

        if(!sscanf(string, "%d/%f", &out->joint_indices[i], &out->weights[i]))
            goto fail;
    }

    if(i == 0)
        goto fail;

    READ_LINE(stream, line, fail); 
    if(!sscanf(line, "vm %d", &out->material_idx))
        goto fail;

    return true;

fail:
    return false;
}

static bool al_read_material(SDL_RWops *stream, const char *basedir, struct material *out)
{
    char line[MAX_LINE_LEN];

    /* Consume the first line with the name - we don't use it currently */
    READ_LINE(stream, line, fail);

    char *saveptr;
    char *mat_name = strtok_r(line, " \t\n", &saveptr);
    mat_name = strtok_r(NULL, " \t\n", &saveptr);
    if(0 == strcmp(mat_name, "__none__"))
        return true;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " ambient %f", &out->ambient_intensity))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " diffuse %f %f %f", &out->diffuse_clr.x, &out->diffuse_clr.y, &out->diffuse_clr.z))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " specular %f %f %f", &out->specular_clr.x, &out->specular_clr.y, &out->specular_clr.z))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " texture %" STREVAL(sizeof(out->texname)) "s",  out->texname))
        goto fail;
    out->texname[sizeof(out->texname)-1] = '\0';

    if(!R_Texture_GetForName(out->texname, &out->texture.id)
    && !R_Texture_Load(basedir, out->texname, &out->texture.id))
        goto fail;

    return true;

fail:
    return false;
}

void al_patch_vbuff_adjacency_info(GLuint VBO, const struct tile *tiles, size_t width, size_t height)
{
    for(int r = 0; r < height; r++) {
        for(int c = 0; c < width; c++) {
            R_GL_TilePatchVertsBlend(VBO, tiles, width, height, r, c);
        }
    }
}

size_t al_priv_buffsize_from_header(const struct pfobj_hdr *header)
{
    size_t ret = 0;

    ret += sizeof(struct render_private);
    ret += header->num_materials * sizeof(struct material);

    return ret;
}


/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

/*
 * Render private buff layout:
 *
 *  +---------------------------------+ <-- base
 *  | struct render_private[1]        |
 *  +---------------------------------+
 *  | struct material[num_materials]  |
 *  +---------------------------------+
 *
 */

void *R_AL_PrivFromStream(const char *base_path, const struct pfobj_hdr *header, SDL_RWops *stream)
{
    struct render_private *priv = malloc(al_priv_buffsize_from_header(header));
    if(!priv)
        goto fail_alloc_priv;

    size_t vbuff_sz = header->num_verts * sizeof(struct vertex);
    struct vertex *vbuff = malloc(vbuff_sz);
    if(!vbuff)
        goto fail_alloc_vbuff;

    priv->mesh.num_verts = header->num_verts;
    priv->num_materials = header->num_materials;
    priv->materials = (void*)(priv + 1);

    for(int i = 0; i < header->num_verts; i++) {
        if(!al_read_vertex(stream, &vbuff[i]))
            goto fail_parse;
    }

    for(int i = 0; i < header->num_materials; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(stream, base_path, &priv->materials[i])) 
            goto fail_parse;
    }

    R_GL_Init(priv, (header->num_as > 0) ? "mesh.animated.textured-phong" : "mesh.static.textured-phong", vbuff);
    free(vbuff);
    return priv;

fail_parse:
    free(vbuff);
fail_alloc_vbuff:
    free(priv);
fail_alloc_priv:
    return NULL;
}

void R_AL_DumpPrivate(FILE *stream, void *priv_data)
{
    struct render_private *priv = priv_data;
    struct vertex *vbuff = glMapNamedBuffer(priv->mesh.VBO, GL_READ_ONLY);
    assert(vbuff);

    /* Write verticies */
    for(int i = 0; i < priv->mesh.num_verts; i++) {

        struct vertex *v = &vbuff[i];

        fprintf(stream, "v %.6f %.6f %.6f\n", v->pos.x, v->pos.y, v->pos.z); 
        fprintf(stream, "vt %.6f %.6f \n", v->uv.x, v->uv.y); 
        fprintf(stream, "vn %.6f %.6f %.6f\n", v->normal.x, v->normal.y, v->normal.z);

        fprintf(stream, "vw ");
        for(int j = 0; j < 4; j++) {

            if(v->weights[j]) {
                fprintf(stream, "%d/%.6f ", v->joint_indices[j], v->weights[j]);
            }
        }
        fprintf(stream, "\n");

        fprintf(stream, "vm %d\n", v->material_idx); 
    }

    glUnmapNamedBuffer(priv->mesh.VBO);

    /* Write materials */
    for(int i = 0; i < priv->num_materials; i++) {
    
        struct material *m = &priv->materials[i];

        /* We don't keep track of the material names; this isn't strictly necessary */
        char name[32];
        snprintf(name, sizeof(name), "Material.%d", i + 1);

        fprintf(stream, "material %s\n", name);
        fprintf(stream, "\tambient %.6f\n", m->ambient_intensity);
        fprintf(stream, "\tdiffuse %.6f %.6f %.6f\n", 
            m->diffuse_clr.x, m->diffuse_clr.y, m->diffuse_clr.z);
        fprintf(stream, "\tspecular %.6f %.6f %.6f\n", 
            m->specular_clr.x, m->specular_clr.y, m->specular_clr.z);
        fprintf(stream, "\ttexture %s\n", m->texname);
    }
}

size_t R_AL_PrivBuffSizeForChunk(size_t tiles_width, size_t tiles_height, size_t num_mats)
{
    size_t ret = 0;

    ret += sizeof(struct render_private);
    ret += sizeof(struct material) * num_mats;

    return ret;
}

bool R_AL_InitPrivFromTilesAndMats(SDL_RWops *mats_stream, size_t num_mats, 
                                  const struct tile *tiles, size_t width, size_t height, 
                                  void *priv_buff, const char *basedir)
{
    size_t num_verts = VERTS_PER_TILE * (width * height);

    struct render_private *priv = priv_buff;
    char *unused_base = (char*)priv_buff + sizeof(struct render_private);
    size_t vbuff_sz = num_verts * sizeof(struct vertex);

    struct vertex *vbuff = malloc(vbuff_sz);
    if(!vbuff)
        goto fail_alloc;

    priv->mesh.num_verts = num_verts;
    priv->num_materials = num_mats;
    priv->materials = (void*)unused_base;

    for(int r = 0; r < height; r++) {
        for(int c = 0; c < width; c++) {

            const struct tile *curr = &tiles[r * width + c];
            struct vertex *vert_base = &vbuff[ (r * width + c) * VERTS_PER_TILE ];

            R_GL_TileGetVertices(curr, vert_base, r, c);
        }
    }

    for(int i = 0; i < num_mats; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(mats_stream, basedir, &priv->materials[i])) 
            goto fail_parse;
    }

    R_GL_Init(priv, "terrain", vbuff);
    al_patch_vbuff_adjacency_info(priv->mesh.VBO, tiles, width, height);

    free(vbuff);
    return true;

fail_parse:
    free(vbuff);
fail_alloc:
    return false;
}

bool R_AL_UpdateMats(SDL_RWops *mats_stream, size_t num_mats, void *priv_buff)
{
    struct render_private *priv = priv_buff;
    extern const char *g_basepath;

    for(int i = 0; i < num_mats; i++) {

        priv->materials[i].texture.tunit = GL_TEXTURE0 + i;
        if(!al_read_material(mats_stream, g_basepath, &priv->materials[i])) 
            return false;
    }
    return true;
}

