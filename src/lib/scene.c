#include "scene.h"
#include "file.h"
#include "coal/util.h"
#include "hell/common.h"
#include <hell/ds.h>
#include "dtags.h"
#include <hell/debug.h>
#include "memory.h"
#include "common.h"
#include "geo.h"
#include "image.h"
#include <string.h>
#define ARCBALL_CAMERA_IMPLEMENTATION
#include "arcball_camera.h"

typedef Obdn_Primitive     Primitive;
typedef Obdn_Xform         Xform;
typedef Obdn_Scene         Scene;
typedef Obdn_Light         Light;
typedef Obdn_Material      Material;
typedef Obdn_Camera        Camera;
typedef Obdn_Texture       Texture;

typedef Obdn_SceneObjectInt obint;

typedef Obdn_PrimitiveHandle PrimitiveHandle;
typedef Obdn_LightHandle LightHandle;
typedef Obdn_MaterialHandle MaterialHandle;
typedef Obdn_TextureHandle TextureHandle;

typedef Obdn_PrimitiveList PrimitiveList;

#define INIT_PRIM_CAP 16
#define INIT_LIGHT_CAP 8
#define INIT_MATERIAL_CAP 8
#define INIT_TEXTURE_CAP 8

#define PRIM(scene, handle) scene->prims[scene->primMap.indices[handle.id]]
#define TEXTURE(scene, handle) scene->textures[scene->texMap.indices[handle.id]]
#define LIGHT(scene, handle) scene->lights[scene->lightMap.indices[handle.id]]
#define MATERIAL(scene, handle) scene->materials[scene->matMap.indices[handle.id]]

#define DPRINT(fmt, ...) hell_DebugPrint(OBDN_DEBUG_TAG_SCENE, fmt, ##__VA_ARGS__)

// lightMap is an indirection from Light Id to the actual light data in the scene.
// invariants are that the active lights in the scene are tightly packed and
// that the lights in the scene are ordered such that their indices are ordered
// within the map.

typedef struct {
    // an array of indices into the object buffers. a handle id is an index into
    // this.
    obint*  indices;
    // a stack of ids that are available for reuse. gets added to when we remove
    // an object and can reclaim its id. the bottom of the stack should always
    // be larger than any other Id used yet. in other words, we should always pull from this 
    // stack for the next id
    Hell_Stack availableIds;
} ObjectMap;

typedef struct Obdn_Scene {
    Obdn_SceneDirtyFlags dirt;
    Obdn_Memory*         memory;
    obint                primCount;
    obint                lightCount;
    obint                materialCount;
    obint                textureCount;
    Obdn_Camera          camera;
    obint                primCapacity;
    Obdn_Primitive*      prims;
    obint                materialCapacity;
    Obdn_Material*       materials;
    obint                textureCapacity;
    Obdn_Texture*        textures;
    obint                lightCapacity;
    Obdn_Light*          lights;
    ObjectMap            primMap;
    ObjectMap            lightMap;
    ObjectMap            matMap;
    ObjectMap            texMap;
} Obdn_Scene;

static void createObjectMap(u32 initObjectCap, u32 initIdStackCap, ObjectMap* map)
{
    map->indices = hell_Malloc(sizeof(map->indices[0]) * initObjectCap);
    hell_CreateStack(initIdStackCap, sizeof(map->indices[0]), NULL, NULL, &map->availableIds);
}

static void growArray(void** curptr, obint* curcap, const u32 elemsize)
{
    assert(*curcap);
    assert(*curptr);
    uint32_t newcap = *curcap * 2;
    void* p = hell_Realloc(*curptr, newcap * elemsize);
    if (!p) 
        hell_Error(HELL_ERR_FATAL, "Growing array capacity failed. Realloc returned Null\n");
    *curptr = p;
    *curcap = newcap;
}

static obint addSceneObject(const void* object, void* objectArray, obint* objectCount, obint* cap, const u32 elemSize, ObjectMap* map)
{
    const obint index = (*objectCount)++;
    if (index == *cap)
    {
        growArray(objectArray, cap, elemSize);
        growArray((void**)&map->indices, cap, elemSize);
    }
    assert(index < *cap);
    obint id = 0;
    if (map->availableIds.count == 0)
        id = index;
    else 
        hell_StackPop(&map->availableIds, &id);
    map->indices[id] = index;
    void* dst = (u8*)(objectArray) + index * elemSize;
    memcpy(dst, object, elemSize);
    return id;
}

static void removeSceneObject(obint id, void* objectArray, obint* objectCount, const u32 elemSize, ObjectMap* map)
{
    void* const       dst   = (u8*)(objectArray) + map->indices[id] * elemSize;
    const void* const src   = (u8*)dst + elemSize;
    const u8* const   end   = (u8*)(objectArray) + *objectCount * elemSize;
    const u32         range = end - (u8*)src;
    memmove(dst, src, range);
    (*objectCount)--;
    hell_StackPush(&map->availableIds, &id);
}

static PrimitiveHandle addPrim(Scene* s, Obdn_Primitive prim)
{
    obint id = addSceneObject(&prim, s->prims, &s->primCount, &s->primCapacity, sizeof(prim), &s->primMap);
    PrimitiveHandle handle = {id};
    s->dirt |= OBDN_SCENE_PRIMS_BIT;
    return handle;
}

static LightHandle addLight(Scene* s, Light light)
{
    obint id = addSceneObject(&light, s->lights, &s->lightCount, &s->lightCapacity, sizeof(light), &s->lightMap);
    LightHandle handle = {id};
    s->dirt |= OBDN_SCENE_LIGHTS_BIT;
    return handle;
}

static TextureHandle addTexture(Scene* s, Obdn_Texture texture)
{
    obint id = addSceneObject(&texture, s->textures, &s->textureCount, &s->textureCapacity, sizeof(texture), &s->texMap);
    TextureHandle handle = {id};
    s->dirt |= OBDN_SCENE_TEXTURES_BIT;
    return handle;
}

static MaterialHandle addMaterial(Scene* s, Obdn_Material material)
{
    obint id = addSceneObject(&material, s->materials, &s->materialCount, &s->materialCapacity, sizeof(s->materials[0]), &s->matMap);
    MaterialHandle handle = {id};
    s->dirt |= OBDN_SCENE_MATERIALS_BIT;
    return handle;
}

static void removePrim(Scene* s, Obdn_PrimitiveHandle handle)
{
    assert(handle.id < s->primCapacity);
    obdn_FreeGeo(&PRIM(s, handle).geo);
    removeSceneObject(handle.id, s->prims, &s->primCount, sizeof(s->prims[0]), &s->primMap);
    s->dirt |= OBDN_SCENE_PRIMS_BIT;
}

static void removeLight(Scene* s, Obdn_LightHandle handle)
{
    assert(handle.id < s->lightCapacity);
    removeSceneObject(handle.id, s->lights, &s->lightCount, sizeof(s->lights[0]), &s->lightMap);
    s->dirt |= OBDN_SCENE_LIGHTS_BIT;
}

static void removeTexture(Scene* s, Obdn_TextureHandle handle)
{
    assert(handle.id < s->textureCapacity);
    obdn_FreeImage(&TEXTURE(s, handle).devImage);
    removeSceneObject(handle.id, s->textures, &s->textureCount, sizeof(s->textures[0]), &s->texMap);
    s->dirt |= OBDN_SCENE_TEXTURES_BIT;
}

static void removeMaterial(Scene* s, Obdn_MaterialHandle handle)
{
    assert(handle.id < s->materialCapacity);
    removeSceneObject(handle.id, s->materials, &s->materialCount, sizeof(s->materials[0]), &s->matMap);
    s->dirt |= OBDN_SCENE_MATERIALS_BIT;
}

static LightHandle addDirectionLight(Scene* s, const Coal_Vec3 dir, const Coal_Vec3 color, const float intensity)
{
    Light light = {
        .type = OBDN_DIRECTION_LIGHT_TYPE,
        .intensity = intensity,
    };
    light.structure.directionLight.dir = dir;
    light.color = color;
    return addLight(s, light);
}

static LightHandle addPointLight(Scene* s, const Coal_Vec3 pos, const Coal_Vec3 color, const float intensity)
{
    Light light = {
        .type = OBDN_LIGHT_POINT_TYPE,
        .intensity = intensity
    };
    light.structure.pointLight.pos = pos;
    light.color = color;
    return addLight(s, light);
}

#define DEFAULT_TEX_DIM 4

static void createDefaultTexture(Obdn_Memory* memory, Texture* texture)
{
    texture->hostBuffer = obdn_RequestBufferRegion(
        memory, 4 * DEFAULT_TEX_DIM * DEFAULT_TEX_DIM /* 4 components, 1 byte each*/,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, OBDN_V_MEMORY_HOST_GRAPHICS_TYPE);
    u8* pxcomponent = texture->hostBuffer.hostData;
    for (int i = 0; i < (DEFAULT_TEX_DIM * DEFAULT_TEX_DIM * 4); i++)
    {
        *pxcomponent++ = UINT8_MAX; // should be full white, hopefully
    }

    texture->devImage = obdn_CreateImageAndSampler(
        memory, DEFAULT_TEX_DIM, DEFAULT_TEX_DIM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 1, VK_FILTER_LINEAR,
        OBDN_V_MEMORY_DEVICE_TYPE);

    obdn_CopyBufferToImage(&texture->hostBuffer, &texture->devImage);

    obdn_TransitionImageLayout(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &texture->devImage);
}

static void printPrimInfoCmd(const Hell_Grimoire* grim, void* scene)
{
    obdn_PrintPrimInfo(scene);
}

static void printTexInfoCmd(const Hell_Grimoire* grim, void* scene)
{
    obdn_PrintTextureInfo(scene);
}

void obdn_CreateScene(Hell_Grimoire* grim, Obdn_Memory* memory, float nearClip, float farClip, Scene* scene)
{
    memset(scene, 0, sizeof(*scene));
    scene->memory = memory;
    scene->camera.xform = coal_Ident_Mat4();
    scene->camera.view = coal_Ident_Mat4();
    Mat4 m = coal_LookAt((Vec3){1, 1, 2}, (Vec3){0, 0, 0}, (Vec3){0, 1, 0});
    scene->camera.xform = m;
    scene->camera.view = coal_Invert4x4(m);
    scene->camera.proj = coal_BuildPerspective(nearClip, farClip);
    // set all xforms to identity
    scene->primCapacity = INIT_PRIM_CAP;
    scene->lightCapacity = INIT_LIGHT_CAP;
    scene->materialCapacity = INIT_MATERIAL_CAP;
    scene->textureCapacity = INIT_TEXTURE_CAP;
    createObjectMap(scene->primCapacity, 8, &scene->primMap);
    createObjectMap(scene->lightCapacity, 8, &scene->lightMap);
    createObjectMap(scene->materialCapacity, 8, &scene->matMap);
    createObjectMap(scene->textureCapacity, 8, &scene->texMap);

    scene->prims = hell_Malloc(scene->primCapacity * sizeof(scene->prims[0]));
    scene->lights = hell_Malloc(scene->lightCapacity * sizeof(scene->lights[0]));
    scene->materials = hell_Malloc(scene->materialCapacity * sizeof(scene->materials[0]));
    scene->textures = hell_Malloc(scene->textureCapacity * sizeof(scene->textures[0]));

    Texture tex = {};
    createDefaultTexture(memory, &tex);
    TextureHandle texhandle = addTexture(scene, tex);
    obdn_SceneCreateMaterial(scene, (Vec3){0, 0.937, 1.0}, 0.8, texhandle, NULL_TEXTURE, NULL_TEXTURE);
    scene->dirt = -1; // dirty everything

    if (grim)
    {
        hell_AddCommand(grim, "priminfo", printPrimInfoCmd, scene);
        hell_AddCommand(grim, "texinfo", printTexInfoCmd, scene);
    }
}

void obdn_BindPrimToMaterial(Scene* scene, Obdn_PrimitiveHandle primhandle, Obdn_MaterialHandle mathandle)
{
    assert(scene->materialCount > mathandle.id);
    PRIM(scene, primhandle).material = mathandle;

    scene->dirt |= OBDN_SCENE_PRIMS_BIT;
}

void obdn_BindPrimToMaterialDirect(Scene* scene, uint32_t directIndex, Obdn_MaterialHandle mathandle)
{
    assert(scene->materialCount > mathandle.id);
    scene->prims[directIndex].material = mathandle;

    scene->dirt |= OBDN_SCENE_PRIMS_BIT;
}

Obdn_PrimitiveHandle obdn_AddPrim(Scene* scene, const Obdn_Geometry geo, const Coal_Mat4 xform, MaterialHandle mat)
{
    Obdn_Primitive prim = {
        .geo = geo,
        .xform = COAL_MAT4_IDENT,
        .material = mat
    };
    prim.xform = xform;
    return addPrim(scene, prim);
}

Obdn_PrimitiveHandle obdn_LoadPrim(Scene* scene, const char* filePath, const Coal_Mat4 xform, MaterialHandle mat)
{
    Obdn_FileGeo fprim;
    int r = obdn_ReadFileGeo(filePath, &fprim);
    assert(r);
    Obdn_Geometry prim = obdn_CreateGeoFromFileGeo(scene->memory, &fprim);
    obdn_TransferGeoToDevice(scene->memory, &prim);
    obdn_FreeFileGeo(&fprim);
    obdn_Announce("Loaded prim at %s\n", filePath);
    return obdn_AddPrim(scene, prim, xform, mat);
}

Obdn_TextureHandle obdn_LoadTexture(Obdn_Scene* scene, const char* filePath, const uint8_t channelCount)
{
    Texture texture = {0};

    VkFormat format;

    switch (channelCount) 
    {
        case 1: format = VK_FORMAT_R8_UNORM; break;
        case 3: format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 4: format = VK_FORMAT_R8G8B8A8_UNORM; break;
        default: DPRINT("ChannelCount %d not support.\n", channelCount); return NULL_TEXTURE;
    }

    obdn_LoadImage(scene->memory, filePath, channelCount, format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 
            VK_IMAGE_ASPECT_COLOR_BIT, 
            1, VK_FILTER_LINEAR, OBDN_V_MEMORY_DEVICE_TYPE, 
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, &texture.devImage);

    return addTexture(scene, texture);
}

Obdn_MaterialHandle obdn_SceneCreateMaterial(Obdn_Scene* scene, Vec3 color, float roughness, 
        Obdn_TextureHandle albedoId, Obdn_TextureHandle roughnessId, Obdn_TextureHandle normalId)
{
    Obdn_Material mat = {0};

    mat.color = color;
    mat.roughness = roughness;
    mat.textureAlbedo    = albedoId;
    mat.textureRoughness = roughnessId;
    mat.textureNormal    = normalId;

    return addMaterial(scene, mat);
}

Obdn_LightHandle obdn_CreateDirectionLight(Scene* scene, const Vec3 color, const Vec3 direction)
{
    return addDirectionLight(scene, direction, color, 1.0);
}

Obdn_LightHandle obdn_SceneCreatePointLight(Scene* scene, const Vec3 color, const Vec3 position)
{
    return addPointLight(scene, position, color, 1.0);
}

#define HOME_POS    {0.0, 0.0, 1.0}
#define HOME_TARGET {0.0, 0.0, 0.0}
#define HOME_UP     {0.0, 1.0, 0.0}
#define ZOOM_RATE   0.005
#define PAN_RATE    0.1
#define TUMBLE_RATE 2

void obdn_UpdateCamera_LookAt(Scene* scene, Vec3 pos, Vec3 target, Vec3 up)
{
    scene->camera.xform = coal_LookAt(pos, target, up);
    scene->camera.view  = coal_Invert4x4(scene->camera.xform);
    scene->dirt |= OBDN_SCENE_CAMERA_VIEW_BIT;
}

static Vec3 prevpos;
static Vec3 prevtarget;
static Vec3 prevup;

void obdn_UpdateCamera_ArcBall(Scene* scene, Vec3* target, int screenWidth, int screenHeight, float dt, int xprev, int x, int yprev, int y, bool panning, bool tumbling, bool zooming, bool home)
{
    Vec3 pos = coal_GetTranslation_Mat4(scene->camera.xform);
    Vec3 up = coal_GetLocalY_Mat4(scene->camera.xform);
    int zoom_ticks = 0;
    if (zooming)
        zoom_ticks = x - xprev;
    else
        zoom_ticks = 0;
    if (home)
    {
        pos =    (Vec3)HOME_POS;
        *target = (Vec3)HOME_TARGET;
        up =     (Vec3)HOME_UP;
    }
    //pos = m_RotateY_Vec3(dt, &pos);
    arcball_camera_update(pos.e, target->e, up.e, NULL, dt, 
            ZOOM_RATE, PAN_RATE, TUMBLE_RATE, screenWidth, screenHeight, xprev, x, yprev, y, 
            panning, tumbling, zoom_ticks, 0);
    Mat4 m = coal_LookAt(pos, *target, up);
    scene->camera.xform = m;
    scene->camera.view  = coal_Invert4x4(scene->camera.xform);
    scene->dirt |= OBDN_SCENE_CAMERA_VIEW_BIT;
}

void obdn_UpdateLight(Scene* scene, LightHandle handle, float intensity)
{
    LIGHT(scene, handle).intensity = intensity;
    scene->dirt |= OBDN_SCENE_LIGHTS_BIT;
}

void obdn_UpdatePrimXform(Scene* scene, PrimitiveHandle handle, const Mat4 delta)
{
    Coal_Mat4 M = coal_Mult_Mat4(PRIM(scene, handle).xform, delta);
    PRIM(scene, handle).xform = M;
    scene->dirt |= OBDN_SCENE_XFORMS_BIT;
}

void obdn_AddPrimToList(Obdn_PrimitiveHandle handle, Obdn_PrimitiveList* list)
{
    list->primIds[list->primCount] = handle.id;
    list->primCount++;
}

void obdn_ClearPrimList(Obdn_PrimitiveList* list)
{
    list->primCount = 0;
}

void obdn_CleanUpScene(Obdn_Scene* scene)
{
    for (int i = 0; i < scene->primCount; i++)
    {
        obdn_FreeGeo(&scene->prims[i].geo);
    }
    for (int i = 1; i <= scene->textureCount; i++) // remember 1 is the first valid texture index
    {
        obdn_FreeImage(&scene->textures[i].devImage);   
        if (scene->textures[i].hostBuffer.hostData)
            obdn_FreeBufferRegion(&scene->textures[i].hostBuffer);
    }
    memset(scene, 0, sizeof(*scene));
}

void obdn_RemovePrim(Obdn_Scene* s, Obdn_PrimitiveHandle handle)
{
    removePrim(s, handle);
}

void obdn_SceneAddDirectionLight(Scene* s, Coal_Vec3 dir, Coal_Vec3 color, float intensity)
{
    addDirectionLight(s, dir, color, intensity);
}

void obdn_SceneAddPointLight(Scene* s, Coal_Vec3 pos, Coal_Vec3 color, float intensity)
{
    addPointLight(s, pos, color, intensity);
}

void obdn_RemoveLight(Scene* s, LightHandle id)
{
    removeLight(s, id);
}

void obdn_PrintLightInfo(const Scene* s)
{
    hell_Print("====== Scene: light info =======\n");
    hell_Print("Light count: %d\n", s->lightCount);
    for (int i = 0; i < s->lightCount; i++)
    {
        hell_Print("Light index %d P ", i);
        hell_Print_Vec3(s->lights[i].structure.pointLight.pos.e);
        hell_Print(" C ");
        hell_Print_Vec3(s->lights[i].color.e);
        hell_Print(" I  %f\n", s->lights[i].intensity);
    }
    hell_Print("Light map: ");
    for (int i = 0; i < s->lightCapacity; i++)
    {
        hell_Print(" %d:%d ", i, s->lightMap.indices[i]);
    }
    hell_Print("\n");
}

void obdn_PrintTextureInfo(const Scene* s)
{
    hell_Print("====== Scene: texture info =======\n");
    hell_Print("Texture count: %d\n", s->textureCount);
    for (int i = 0; i < s->textureCount; i++)
    {
        const Texture* tex = &s->textures[i];
        const Obdn_Image* img = &tex->devImage;
        hell_Print("Texture index %d\n", i); 
        hell_Print("Width %d Height %d Size %d \n", img->extent.width, img->extent.height, img->size);
        hell_Print("Format %d \n", img->format);
        hell_Print("\n");
    }
    hell_Print("Texture map: ");
    for (int i = 0; i < s->textureCapacity; i++)
    {
        hell_Print(" %d:%d ", i, s->texMap.indices[i]);
    }
    hell_Print("\n");
}

void obdn_PrintPrimInfo(const Scene* s)
{
    hell_Print("====== Scene: primitive info =======\n");
    hell_Print("Prim count: %d\n", s->primCount);
    for (int i = 0; i < s->primCount; i++)
    {
        hell_Print("Prim %d material id %d\n", i, s->prims[i].material.id); 
        const Material* mat = &MATERIAL(s, s->prims[i].material);
        hell_Print("Material: handle id %d color %f %f %f roughness %f\n", s->prims[i].material.id, mat->color.r, mat->color.g, mat->color.b, mat->roughness);
        hell_Print("Material: Albedo TextureHandle: %d\n", mat->textureAlbedo.id);
        hell_Print_Mat4(s->prims[i].xform.e);
        hell_Print("\n");
    }
    hell_Print("Prim map: ");
    for (int i = 0; i < s->primCapacity; i++)
    {
        hell_Print(" %d:%d ", i, s->primMap.indices[i]);
    }
    hell_Print("\n");
}

void obdn_UpdateLightColor(Obdn_Scene* scene, Obdn_LightHandle handle, float r, float g, float b)
{
    LIGHT(scene, handle).color.r = r;
    LIGHT(scene, handle).color.g = g;
    LIGHT(scene, handle).color.b = b;
    scene->dirt |= OBDN_SCENE_LIGHTS_BIT;
}

void obdn_UpdateLightPos(Obdn_Scene* scene, Obdn_LightHandle handle, float x, float y, float z)
{
    LIGHT(scene, handle).structure.pointLight.pos.x = x;
    LIGHT(scene, handle).structure.pointLight.pos.y = y;
    LIGHT(scene, handle).structure.pointLight.pos.z = z;
    scene->dirt |= OBDN_SCENE_LIGHTS_BIT;
}

void obdn_UpdateLightIntensity(Obdn_Scene* scene, Obdn_LightHandle handle, float i)
{
    LIGHT(scene, handle).intensity = i;
    scene->dirt |= OBDN_SCENE_LIGHTS_BIT;
}

Obdn_Scene* obdn_AllocScene(void)
{
    return hell_Malloc(sizeof(Scene));
}

Mat4 obdn_GetCameraView(const Obdn_Scene* scene)
{
    return scene->camera.view;
}

Mat4 obdn_GetCameraProjection(const Obdn_Scene* scene)
{
    return scene->camera.proj;
}

Obdn_Primitive* obdn_GetPrimitive(const Obdn_Scene* s, uint32_t id)
{
    PrimitiveHandle handle = {id};
    return &PRIM(s, handle);
}

uint32_t obdn_GetPrimCount(const Obdn_Scene* s)
{
    return s->primCount;
}

Obdn_SceneDirtyFlags obdn_GetSceneDirt(const Obdn_Scene* s)
{
    return s->dirt;
}

void obdn_SceneClearDirt(Obdn_Scene* s)
{
    s->dirt = 0;
}

void obdn_SceneAddCube(Obdn_Scene* s, Mat4 xform, MaterialHandle mathandle, bool clockwise)
{
    Obdn_Geometry cube = obdn_CreateCube(s->memory, clockwise);
    obdn_AddPrim(s, cube, xform, mathandle);
}

Obdn_Texture* obdn_GetTexture(const Obdn_Scene* s, Obdn_TextureHandle handle)
{
    return &TEXTURE(s, handle);
}

Obdn_Material* obdn_GetMaterial(const Obdn_Scene* s, Obdn_MaterialHandle handle)
{
    return &MATERIAL(s, handle);
}

Obdn_TextureHandle obdn_SceneCreateTexture(Obdn_Scene* scene, Obdn_V_Image image)
{
    Obdn_Texture tex = {0};
    tex.devImage = image;
    return addTexture(scene, tex);
}

uint32_t obdn_SceneGetTextureCount(const Obdn_Scene* s)
{
    return s->textureCount;
}

Obdn_Texture* obdn_SceneGetTextures(const Obdn_Scene* s)
{
    return s->textures;
}

uint32_t       obdn_SceneGetMaterialCount(const Obdn_Scene* s)
{
    return s->materialCount;
}

Obdn_Material* obdn_SceneGetMaterials(const Obdn_Scene* s)
{
    return s->materials;
}

uint32_t obdn_SceneGetMaterialIndex(const Obdn_Scene* s, Obdn_MaterialHandle handle)
{
    return s->matMap.indices[handle.id];
}

uint32_t obdn_SceneGetTextureIndex(const Obdn_Scene* s, Obdn_TextureHandle handle)
{
    return s->texMap.indices[handle.id];
}

void obdn_SceneDirtyTextures(Obdn_Scene* s)
{
    s->dirt |= OBDN_SCENE_TEXTURES_BIT;
}

void obdn_SceneSetGeoDirect(Obdn_Scene* s, Obdn_Geometry geo, u32 directIndex)
{
    s->prims[directIndex].geo = geo;
    s->dirt |= OBDN_SCENE_PRIMS_BIT;
}

void obdn_SceneFreeGeoDirect(Obdn_Scene* s, u32 directIndex)
{
    obdn_FreeGeo(&s->prims[directIndex].geo);
    memset(&s->prims[directIndex].geo, 0, sizeof(s->prims[directIndex].geo));;
}

bool obdn_SceneHasGeoDirect(Obdn_Scene* s, u32 directIndex)
{
    if (s->prims[0].geo.vertexCount) return true;
    return false;
}

void obdn_SceneSetCameraView(Obdn_Scene* scene, const Coal_Mat4 m)
{
    scene->camera.view = m;
    scene->dirt |= OBDN_SCENE_CAMERA_VIEW_BIT;
}

void obdn_SceneSetCameraProjection(Obdn_Scene* scene, const Coal_Mat4 m)
{
    scene->camera.proj = m;
    scene->dirt |= OBDN_SCENE_CAMERA_PROJ_BIT;
}
